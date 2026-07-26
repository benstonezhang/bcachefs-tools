/*
 * kill_btree_node: Debugging tool for corrupting specific btree nodes.
 *
 * Walks the btree at a given level and writes zeroes to the on-disk location
 * of the Nth node, simulating media corruption. Used for testing recovery
 * paths — fsck should detect and repair the damage.
 *
 * Safety: Opens the filesystem read-only (no in-memory modifications), then
 * does raw pwrite() to the block device fd.
 *
 * Ported from src/commands/kill_btree_node.rs.
 */

#include <fcntl.h>
#include <getopt.h>
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
	     "Options:\n"
	     "  -n, --node btree:level:idx            Node to kill\n"
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

int cmd_kill_btree_node(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "node", required_argument, NULL, 'n' },
		{ "dev", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct bch_opts opts = bch2_opts_empty();
	DARRAY(struct kill_node) kill_nodes = {};
	int opt, dev_idx = -1;

	opt_set(opts, read_only, true);

	while ((opt = getopt_long(argc, argv, "n:d:h", longopts, NULL)) != -1)
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
						"killing btree node on dev %u %s\n",
						ptr->dev, buf.buf);
					printbuf_exit(&buf);

					ssize_t wrote = pwrite(
						ca->disk_sb.bdev->bd_fd, zeroes,
						c->opts.block_size,
						ptr->offset << 9);
					bch2_dev_put(ca);

					if (wrote !=
					    (ssize_t)c->opts.block_size) {
						fprintf(stderr,
							"pwrite error: expected %u got %zd %m\n",
							c->opts.block_size,
							wrote);
						ret = -EIO;
					}
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
	}

	bch2_trans_put(trans);
	bch2_fs_stop(c);
	darray_exit(&kill_nodes);
	darray_exit(&devs);
	free(zeroes);

	return ret;
}
