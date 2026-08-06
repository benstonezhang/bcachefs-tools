/*
 * kill_btree_node: Debugging tool for corrupting specific btree nodes.
 *
 * Walks the btree at a given level and damages the on-disk location of the Nth
 * node, simulating media corruption. Used for testing recovery paths — fsck
 * should detect and repair the damage.
 *
 * --error picks what the damage looks like, because recovery takes visibly
 * different paths depending on how far a node gets through validation:
 *
 *   zero  the first block is gone, so nothing parses and there is no evidence
 *         the node was ever there beyond the parent's pointer
 *   csum  the node header is intact - the parent's btree_ptr_v2 seq matches,
 *         min_key and the written count are readable - and one bset fails its
 *         checksum, with every key in it still byte-for-byte correct. This is
 *         the shape field reports arrive in, and it isolates the read path's
 *         accept-or-reject decision from any question of key damage.
 *   keys  same, but the flipped byte is in the key data, so the bset fails its
 *         checksum *and* holds a corrupt key. This is what exercises per-key
 *         validation, which is the only thing between an accepted bad-checksum
 *         bset and the btree.
 *
 * The checksum covers [sizeof(bch_csum), vstruct_end), and vstruct_end is
 * sizeof(btree_node) + keys.u64s * 8 - not the whole block. So the byte to flip
 * has to be one the bset actually reaches: presplit_shard_boundaries leaves
 * plenty of nodes whose first bset is empty, and on those the first key byte is
 * already past the end, so flipping it changes nothing and the injection is a
 * silent no-op.
 *
 * Anything that stops the requested damage from landing is a hard error here,
 * never a warning: this is a test instrument, and a caller that asked for
 * corruption and silently didn't get it goes on to "verify" recovery against an
 * intact filesystem and passes. Exit status is the only signal a shell script
 * gets, so it has to mean "the node is damaged".
 *
 * Safety: Opens the filesystem read-only (no in-memory modifications), then
 * does raw pread()/pwrite() to the block device fd. The O_DIRECT alignment
 * constraint comes from the block device being opened with O_DIRECT by the
 * kernel code.
 *
 * Ported from src/commands/kill_btree_node.rs.
 */

#include <fcntl.h>
#include <getopt.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libbcachefs.h"
#include "btree/iter.h"
#include "btree/cache.h"
#include "init/error.h"
#include "init/fs.h"
#include "sb/members.h"
#include "cmds.h"

static void kill_btree_node_usage(void)
{
	puts("bcachefs kill_btree_node - make btree nodes unreadable (debugging tool)\n"
	     "Usage: bcachefs kill_btree_node [OPTION]... <devices>\n"
	     "\n"
	     "Damages the on-disk location of the Nth node at a given level, simulating\n"
	     "media corruption. Used for testing recovery paths - fsck should detect and\n"
	     "repair the damage.\n"
	     "\n"
	     "--error picks what the damage looks like:\n"
	     "  zero  the first block is gone, so nothing parses\n"
	     "  csum  the node header is intact and one bset fails its checksum, with\n"
	     "        every key in it still byte-for-byte correct\n"
	     "  keys  same, but the flipped byte is in the key data, so the bset fails\n"
	     "        its checksum and holds a corrupt key\n"
	     "\n"
	     "Options:\n"
	     "  -n, --node btree:level:idx            Node to kill\n"
	     "  -e, --error zero|csum|keys            Kind of damage to inject (default: zero)\n"
	     "  -d, --dev  dev                        Device index (default: kill all replicas)\n"
	     "  -h, --help                            Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

struct kill_node {
	unsigned btree;
	unsigned level;
	u64 idx;
};

enum error_type {
	ERROR_ZERO,
	ERROR_CSUM,
	ERROR_KEYS,
};

static const char * const error_str[] = {
	[ERROR_ZERO]	= "Zero",
	[ERROR_CSUM]	= "Csum",
	[ERROR_KEYS]	= "Keys",
};

struct bset_loc {
	size_t at;
	size_t sector;
	size_t u64s;
};

static u16 node_u16(const u8 *node, size_t at)
{
	__le16 v;

	memcpy(&v, node + at, sizeof(v));
	return le16_to_cpu(v);
}

/*
 * The node's first bset, keys or not.
 *
 * --error csum doesn't need a key to be at stake, and a node whose bsets are
 * all empty still has to be corruptible: on a freshly formatted filesystem
 * every btree leaf is one of those.
 */
static struct bset_loc first_bset(const u8 *node)
{
	size_t at = sizeof(struct btree_node) - sizeof(struct bset);

	return (struct bset_loc) {
		.at	= at,
		.sector	= 0,
		.u64s	= node_u16(node, at + offsetof(struct bset, u64s)),
	};
}

/*
 * The first bset that actually holds keys, or NULL if there isn't one.
 *
 * Skipping empty bsets matters: the first bset comes back u64s 0 not just on
 * freshly split nodes but on plenty of populated ones, and corrupting an empty
 * bset gives the read path nothing to decide - no key is at risk.
 *
 * Layout is what bch2_btree_node_read_done() walks: the first bset is the
 * btree_node itself, each later one a btree_node_entry at the running sector
 * offset, each occupying vstruct_sectors i.e. round_up(header + u64s * 8,
 * block_size).
 */
static struct bset_loc *first_bset_with_keys(const u8 *node, size_t node_len,
					     size_t written_sectors,
					     size_t block_size,
					     struct bset_loc *out)
{
	size_t off = 0;

	while (off < written_sectors * 512) {
		/* bset offset within its container, and the container's size */
		size_t bset_at, header;

		if (!off) {
			bset_at = sizeof(struct btree_node) - sizeof(struct bset);
			header = sizeof(struct btree_node);
		} else {
			bset_at = off + sizeof(struct btree_node_entry) - sizeof(struct bset);
			header = sizeof(struct btree_node_entry);
		}

		size_t u64s_at = bset_at + offsetof(struct bset, u64s);
		if (u64s_at + 2 > node_len)
			break;

		size_t u64s = node_u16(node, u64s_at);

		if (u64s) {
			out->at = bset_at;
			out->sector = off / 512;
			out->u64s = u64s;
			return out;
		}

		/* empty bset: skip past it and try the next */
		size_t sectors = DIV_ROUND_UP(header + u64s * 8, block_size) *
				  block_size / 512;
		if (!sectors)
			break;
		off += sectors * 512;
	}

	return NULL;
}

/*
 * Damage the node at @offset on @fd. Returns 0 on success, negative errno on
 * failure. On failure the reason has already been printed to stderr.
 */
static int damage_node(int fd, u64 offset, u32 block_size,
		       unsigned written, enum error_type error,
		       const void *zeroes)
{
	if (error == ERROR_ZERO) {
		ssize_t ret = pwrite(fd, zeroes, block_size, offset);

		if (ret != (ssize_t)block_size) {
			fprintf(stderr,
				"pwrite error: expected %u got %zd %m\n",
				block_size, ret);
			return -errno ?: -EIO;
		}
		return 0;
	}

	/* the whole written extent, so we can walk its bsets */
	size_t buf_size = max((size_t)written * 512, (size_t)block_size);
	void *buf;

	xposix_memalign(&buf, block_size, buf_size);

	ssize_t ret = pread(fd, buf, buf_size, offset);

	if (ret != (ssize_t)buf_size) {
		fprintf(stderr, "pread error: expected %zu got %zd %m\n",
			buf_size, ret);
		free(buf);
		return -errno ?: -EIO;
	}

	struct bset_loc loc, fb, *bset;

	bset = first_bset_with_keys(buf, buf_size, written, block_size, &loc);
	if (!bset && error == ERROR_CSUM) {
		fb = first_bset(buf);
		bset = &fb;
	}

	if (!bset) {
		fprintf(stderr,
			"no bset with keys in this %u sector node, so --error keys "
			"has nothing to corrupt\n", written);
		free(buf);
		return -ENOENT;
	}

	/* the byte to flip and what it is, for reporting */
	size_t victim;
	const char *what;

	if (error == ERROR_CSUM) {
		/* journal_seq sits in the bset header, so it's covered by the
		 * checksum whatever u64s is, and nothing reads it before the
		 * checksum is verified - the node header stays parseable */
		victim = bset->at + offsetof(struct bset, journal_seq);
		what = "bset header";
	} else {
		/* midway into the keys: inside the checksummed region, and
		 * corrupts a key rather than a header field */
		victim = bset->at + sizeof(struct bset) + bset->u64s * 8 / 2;
		what = "key data";
	}

	if (victim >= buf_size) {
		fprintf(stderr,
			"bset at node offset %zu claims u64s %zu, putting the byte "
			"to corrupt (%zu) past the end of the %zu byte node\n",
			bset->sector, bset->u64s, victim, buf_size);
		free(buf);
		return -EINVAL;
	}

	fprintf(stderr, "  corrupting %s of bset at node offset %zu/%u, u64s %zu\n",
		what, bset->sector, written, bset->u64s);

	((u8 *)buf)[victim] ^= 0xff;

	ret = pwrite(fd, buf, buf_size, offset);
	if (ret != (ssize_t)buf_size) {
		fprintf(stderr, "pwrite error: expected %zu got %zd %m\n",
			buf_size, ret);
		free(buf);
		return -errno ?: -EIO;
	}

	free(buf);
	return 0;
}

int cmd_kill_btree_node(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "node", required_argument, NULL, 'n' },
		{ "error", required_argument, NULL, 'e' },
		{ "dev", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct bch_opts opts = bch2_opts_empty();
	DARRAY(struct kill_node) kill_nodes = {};
	enum error_type error = ERROR_ZERO;
	int opt, dev_idx = -1;

	opt_set(opts, read_only, true);

	while ((opt = getopt_long(argc, argv, "n:e:d:h", longopts, NULL)) != -1)
		switch (opt) {
		case 'n': {
			char *p = optarg;
			const char *str_btree = strsep(&p, ":");
			const char *str_level = strsep(&p, ":");
			const char *str_idx = strsep(&p, ":");

			if (!str_btree)
				die("invalid node spec: %s", optarg);

			struct kill_node n = {
				.btree = read_string_list_or_die(
					str_btree, __bch2_btree_ids,
					"btree id"),
				.level = 0,
				.idx = 0,
			};

			if (str_level && (kstrtouint(str_level, 10, &n.level) ||
					  n.level >= BTREE_MAX_DEPTH))
				die("invalid level: %s (max %u)", str_level,
				    BTREE_MAX_DEPTH - 1);

			if (str_idx && kstrtoull(str_idx, 10, &n.idx))
				die("invalid index: %s", str_idx);

			darray_push(&kill_nodes, n);
			break;
		}
		case 'e':
			if (!strcmp(optarg, "zero"))
				error = ERROR_ZERO;
			else if (!strcmp(optarg, "csum"))
				error = ERROR_CSUM;
			else if (!strcmp(optarg, "keys"))
				error = ERROR_KEYS;
			else
				die("invalid error type: %s (zero, csum, keys)",
				    optarg);
			break;
		case 'd':
			if (kstrtoint(optarg, 10, &dev_idx))
				die("invalid device index: %s", optarg);
			break;
		case 'h':
			kill_btree_node_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply device(s)");

	if (!kill_nodes.nr)
		die("no nodes specified (use -n btree:level:idx)");

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	if (IS_ERR(c))
		die("error opening devices: %s", bch2_err_str(PTR_ERR(c)));

	int ret = 0;
	void *zeroes;

	xposix_memalign(&zeroes, c->opts.block_size, c->opts.block_size);
	memset(zeroes, 0, c->opts.block_size);

	struct btree_trans *trans = bch2_trans_get(c);

	darray_for_each(kill_nodes, i)
	{
		bool found = false;
		unsigned damaged = 0;
		bool damage_failed = false;
		int damage_err = 0;

		int walk_ret = for_each_btree_node(
			trans, iter, i->btree, POS_MIN, i->level, 0, b, ({
				if (b->c.level != (u8)i->level)
					continue;

				if (i->idx > 0) {
					i->idx--;
					continue;
				}

				found = true;
				struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(
					bkey_i_to_s_c(&b->key));
				bkey_for_each_ptr(ptrs, ptr)
				{
					if (dev_idx >= 0 &&
					    ptr->dev != (unsigned)dev_idx)
						continue;

					struct bch_dev *ca =
						bch2_dev_tryget(c, ptr->dev);
					if (!ca)
						continue;

					struct printbuf buf = PRINTBUF;
					bch2_btree_id_to_text(&buf, i->btree);
					prt_printf(&buf, " l=%u\n  ", i->level);
					bch2_bkey_val_to_text(
						&buf, c,
						bkey_i_to_s_c(&b->key));

					fprintf(stderr,
						"damaging btree node (%s) on dev %u %s\n",
						error_str[error], ptr->dev,
						buf.buf);
					printbuf_exit(&buf);

					/*
					 * Stop on the first failure rather
					 * than carrying on: a caller that
					 * asked for damage and didn't get it
					 * will otherwise go on to "verify"
					 * recovery against an intact
					 * filesystem.
					 */
					damage_err = damage_node(
						ca->disk_sb.bdev->bd_fd,
						ptr->offset << 9,
						c->opts.block_size, b->written,
						error, zeroes);
					bch2_dev_put(ca);

					if (damage_err) {
						damage_failed = true;
						break;
					}
					damaged++;
				}

				1; /* break after first match */
			}));

		if (walk_ret < 0) {
			fprintf(stderr, "error walking btree nodes: %s\n",
				bch2_err_str(walk_ret));
			ret = walk_ret;
			break;
		}

		if (!found) {
			fprintf(stderr, "node at specified index not found\n");
			ret = -ENOENT;
			break;
		}

		if (damage_failed) {
			ret = damage_err;
			break;
		}

		if (!damaged) {
			fprintf(stderr,
				"%s l=%u idx found, but no replica was damaged - "
				"no pointer matched --dev %d\n",
				bch2_btree_id_str(i->btree), i->level, dev_idx);
			ret = -ENOENT;
			break;
		}
	}

	bch2_trans_put(trans);
	bch2_fs_stop(c);
	darray_exit(&kill_nodes);
	darray_exit(&devs);
	free(zeroes);

	return ret;
}
