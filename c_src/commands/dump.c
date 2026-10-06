/*
 * dump: Dump filesystem metadata to a qcow2 image.
 * undump: Convert a qcow2 image back to a raw device image.
 *
 * Dump walks the btree and journal to identify all metadata locations and
 * exports them to a sparse qcow2 image. Supports sanitizing inline data and
 * filenames for privacy.
 *
 * Ported from src/commands/dump.rs.
 */

#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "btree/cache.h"
#include "btree/iter.h"
#include "btree/read.h"
#include "sb/members.h"
#include "init/error.h"
#include "init/fs.h"
#include "data/extents.h"
#include "data/checksum.h"
#include "journal/read.h"
#include "util/vstructs.h"
#include "cmds.h"

/* Sanitize implementation */

static bool csum_type_is_encryption(unsigned csum_type)
{
	return csum_type == BCH_CSUM_chacha20_poly1305_80 ||
	       csum_type == BCH_CSUM_chacha20_poly1305_128;
}

/*
 * Crypto for the sanitize path. These drive the wrapped bch2_encrypt /
 * bch2_checksum over the same byte ranges the kernel's bset_encrypt() and
 * csum_vstruct() cover, so the metadata dump can decrypt, edit, and
 * re-checksum bsets and journal entries. @vstruct_bytes is the whole
 * vstruct's byte length; the checksum covers everything after the leading
 * csum field, and journal encryption everything after the encrypted_start
 * marker. Encryption is symmetric, so the same call both decrypts and
 * re-encrypts.
 */
static int jset_encrypt(struct bch_fs *c, struct jset *j, unsigned csum_type,
			size_t vstruct_bytes)
{
	size_t off = offsetof(struct jset, encrypted_start);

	return bch2_encrypt(c, csum_type, journal_nonce(j),
			    (void *)j + off, vstruct_bytes - off);
}

static void jset_csum_set(struct bch_fs *c, struct jset *j, unsigned csum_type,
			  size_t vstruct_bytes)
{
	size_t off = sizeof(struct bch_csum);

	j->csum = bch2_checksum(c, csum_type, journal_nonce(j),
				(void *)j + off, vstruct_bytes - off);
}

/* @node points at the btree_node (first bset) or btree_node_entry, whose
 * first field is the csum; @i is the bset within it. */
static void bset_csum_set(struct bch_fs *c, void *node, struct bset *i,
			  unsigned bset_byte_offset, unsigned csum_type,
			  size_t vstruct_bytes)
{
	size_t off = sizeof(struct bch_csum);

	*(struct bch_csum *)node = bch2_checksum(c, csum_type,
		btree_nonce(i, bset_byte_offset), node + off,
		vstruct_bytes - off);
}

/*
 * What the write path should do to each buffer before dumping it.
 */
struct sanitize_opts {
	/* Zero inline data extents, and (with sanitize_filenames) scramble
	 * dirent names. */
	bool sanitize;
	bool sanitize_filenames;
	/* Keep only the lowest-device-index replica of each btree_ptr,
	 * rewriting the rest to an invalid device. */
	bool single_replica;
};

/*
 * De-replicate a btree pointer key in place: keep the pointer on the lowest
 * device index and set every other pointer's device to BCH_SB_MEMBER_INVALID.
 * Returns whether it changed anything. The surviving replica is the one the
 * dump actually wrote, and the only one the read path will consider.
 */
static bool derep(struct bch_fs *c, struct bkey_s k)
{
	struct bkey_ptrs ptrs = bch2_bkey_ptrs(k);
	struct bch_extent_ptr *min_ptr = NULL;
	u8 min_dev = BCH_SB_MEMBER_INVALID;

	bkey_for_each_ptr(ptrs, ptr) {
		if (ptr->dev != BCH_SB_MEMBER_INVALID &&
		    (min_ptr == NULL || ptr->dev < min_dev)) {
			min_ptr = ptr;
			min_dev = ptr->dev;
		}
	}

	if (!min_ptr)
		return false;

	bool modified = false;
	bkey_for_each_ptr(ptrs, ptr2) {
		if (ptr2->dev != min_dev) {
			ptr2->dev = BCH_SB_MEMBER_INVALID;
			modified = true;
		}
	}

	return modified;
}

/*
 * Sanitize a bkey value region in-place, dispatched by key type. Handles
 * both --single-replica de-replication (btree pointers) and --sanitize
 * scrubbing (inline data, filenames). Called for every key in both btree
 * nodes and journal entries, so de-replication reaches the btree_root
 * pointers carried in the journal as well as the interior/leaf pointers in
 * btree nodes. Returns whether it modified.
 */
static bool sanitize_val(struct bch_fs *c, struct bkey_s k,
			 struct sanitize_opts *opts)
{
	switch (k.k->type) {
	case KEY_TYPE_btree_ptr:
	case KEY_TYPE_btree_ptr_v2:
		return opts->single_replica ? derep(c, k) : false;
	case KEY_TYPE_inline_data:
		if (!opts->sanitize)
			return false;
		memset(k.v, 0, bkey_val_bytes(k.k));
		return true;
	case KEY_TYPE_indirect_inline_data:
		if (!opts->sanitize)
			return false;
		if (bkey_val_bytes(k.k) > 8)
			memset((u8 *)k.v + 8, 0, bkey_val_bytes(k.k) - 8);
		return true;
	case KEY_TYPE_dirent:
		if (!opts->sanitize || !opts->sanitize_filenames)
			return false;
		if (bkey_val_bytes(k.k) > 9)
			memset((u8 *)k.v + 9, 'X', bkey_val_bytes(k.k) - 9);
		return true;
	default:
		return false;
	}
}

/* Walk unpacked bkey_i entries in a jset_entry data region and sanitize. */
static bool sanitize_journal_keys(struct bch_fs *c, void *buf, size_t start,
				  size_t end, struct sanitize_opts *opts)
{
	bool modified = false;
	size_t pos = start;

	while (pos + 3 <= end) {
		struct bkey_i *k = buf + pos;
		if (!k->k.u64s)
			break;

		size_t key_bytes = k->k.u64s * 8;
		if (pos + key_bytes > end)
			break;

		if (sanitize_val(c, bkey_i_to_s(k), opts))
			modified = true;

		pos += key_bytes;
	}

	return modified;
}

static void sanitize_journal(struct bch_fs *c, void *buf, size_t len,
			     struct sanitize_opts *opts)
{
	u64 jset_magic = le64_to_cpup((void *)c->disk_sb.sb->uuid.b) ^
			 0x245235c1a3625032;
	size_t pos = 0;

	while (pos + offsetof(struct jset, _data) <= len) {
		struct jset *j = buf + pos;
		if (le64_to_cpu(j->magic) != jset_magic)
			break;

		size_t vstruct_bytes = vstruct_bytes(j);
		if (vstruct_bytes > len - pos)
			break;

		unsigned csum_type = JSET_CSUM_TYPE(j);
		bool modified = false;

		if (csum_type_is_encryption(csum_type)) {
			if (!c->chacha20_key_set) {
				fprintf(stderr,
					"found encrypted journal entry on non-encrypted filesystem\n");
				return;
			}

			if (jset_encrypt(c, j, csum_type, vstruct_bytes)) {
				fprintf(stderr,
					"error decrypting journal entry\n");
				return;
			}
			modified = true;
		}

		void *data_end = buf + pos + vstruct_bytes;
		struct jset_entry *entry = j->start;

		while (vstruct_next(entry) <= (struct jset_entry *)data_end) {
			if (jset_entry_is_key(entry) &&
			    sanitize_journal_keys(c, entry->start, 0,
						  entry->u64s * 8, opts))
				modified = true;

			entry = vstruct_next(entry);
		}

		if (modified) {
			/* Re-encrypt (symmetric) if encrypted, then recompute
			 * the csum so the entry stays checksum-valid instead
			 * of csum-cleared. */
			if (csum_type_is_encryption(csum_type) &&
			    jset_encrypt(c, j, csum_type, vstruct_bytes)) {
				fprintf(stderr,
					"error re-encrypting journal entry\n");
				return;
			}
			jset_csum_set(c, j, csum_type, vstruct_bytes);
		}

		pos += round_up(vstruct_bytes, block_bytes(c));
	}
}

static void sanitize_btree(struct bch_fs *c, void *buf, size_t len,
			   struct sanitize_opts *opts)
{
	u64 bset_magic = le64_to_cpup((void *)c->disk_sb.sb->uuid.b) ^
			 0x90135c78b99e07f5;
	bool first = true;
	u64 seq = 0;
	unsigned format_key_u64s = BKEY_U64s;
	size_t pos = 0;
	size_t bset_byte_offset = 0;

	/* The node's packed-key format lives in the btree_node header at the
	 * start of the buffer; packed keys are unpacked against it. */
	struct bkey_format *format = buf + offsetof(struct btree_node, format);

	while (pos < len) {
		struct bset *i;
		size_t vstruct_bytes, data_off;

		if (first) {
			struct btree_node *bn = buf + pos;
			if (pos + offsetof(struct btree_node, keys) +
				    offsetof(struct bset, _data) >
			    len)
				break;
			if (le64_to_cpu(bn->magic) != bset_magic)
				break;

			i = &bn->keys;
			data_off = pos + offsetof(struct btree_node, keys) +
				   offsetof(struct bset, _data);
			format_key_u64s = bn->format.key_u64s;
			seq = i->seq;
			vstruct_bytes = offsetof(struct btree_node, keys) +
					vstruct_bytes(i);
		} else {
			struct btree_node_entry *bne = buf + pos;
			if (pos + offsetof(struct btree_node_entry, keys) +
				    offsetof(struct bset, _data) >
			    len)
				break;

			i = &bne->keys;
			data_off = pos +
				   offsetof(struct btree_node_entry, keys) +
				   offsetof(struct bset, _data);
			if (i->seq != seq)
				break;
			vstruct_bytes =
				offsetof(struct btree_node_entry, keys) +
				vstruct_bytes(i);
		}

		if (pos + vstruct_bytes > len)
			break;

		unsigned csum_type = BSET_CSUM_TYPE(i);
		bool modified = false;

		if (csum_type_is_encryption(csum_type)) {
			if (!c->chacha20_key_set) {
				fprintf(stderr,
					"found encrypted btree node on non-encrypted filesystem\n");
				return;
			}

			if (bset_encrypt(c, i, bset_byte_offset)) {
				fprintf(stderr,
					"error decrypting btree node\n");
				return;
			}
			modified = true;
		}

		void *key_end = buf + data_off + le16_to_cpu(i->u64s) * 8;
		void *key_pos = buf + data_off;

		while (key_pos + 3 <= key_end) {
			struct bkey *k = key_pos;
			if (!k->u64s)
				break;

			size_t key_bytes = k->u64s * 8;
			if (key_pos + key_bytes > key_end)
				break;

			unsigned key_hdr_u64s =
				k->format == 0 ? format_key_u64s : BKEY_U64s;
			size_t val_off = key_hdr_u64s * 8;

			if (val_off < key_bytes) {
				void *vs = key_pos + val_off;

				/* An unpacked key (KEY_FORMAT_CURRENT) is a
				 * bkey_i in place; a packed key is unpacked
				 * into a local bkey using the node format only
				 * - no struct btree, so none of
				 * btree_node_read_done's repair runs. Either
				 * way the value is pointed at in place, so
				 * sanitize_val's edits land on the buffer. */
				struct bkey u;
				struct bkey_s kk;

				if (k->format == 0) {
					memset(&u, 0, sizeof(u));
					__bch2_bkey_unpack_key(format, &u,
							       (void *)k);
					kk = (struct bkey_s) {
						.k = &u,
						.v = vs,
					};
				} else {
					kk = bkey_i_to_s((void *)k);
				}

				if (sanitize_val(c, kk, opts))
					modified = true;
			}

			key_pos += key_bytes;
		}

		if (modified) {
			/* Re-encrypt (symmetric) if encrypted, then recompute
			 * the bset csum over the btree_node (first bset) or
			 * btree_node_entry. */
			if (csum_type_is_encryption(csum_type))
				bset_encrypt(c, i, bset_byte_offset);
			bset_csum_set(c, buf + pos, i, bset_byte_offset,
				      csum_type, vstruct_bytes);
		}

		first = false;
		size_t advance = round_up(vstruct_bytes, block_bytes(c));
		bset_byte_offset += advance;
		pos += advance;
	}
}

/* Dump implementation */

struct dump_dev {
	ranges sb;
	ranges journal;
	ranges btree;
};

static void dump_node_collect(struct bch_fs *c, struct dump_dev *devs,
			      struct bkey_s_c k, bool single_replica)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);

	if (single_replica) {
		/* Dump only the replica on the lowest device index; the other
		 * ptrs are rewritten to an invalid device in the btree write
		 * path. */
		struct bch_extent_ptr *min_ptr = NULL;
		u8 min_dev = BCH_SB_MEMBER_INVALID;

		bkey_for_each_ptr(ptrs, ptr) {
			if (ptr->dev < c->sb.nr_devices && c->devs[ptr->dev] &&
			    (min_ptr == NULL || ptr->dev < min_dev)) {
				min_ptr = (struct bch_extent_ptr *)ptr;
				min_dev = ptr->dev;
			}
		}

		if (min_ptr)
			range_add(&devs[min_ptr->dev].btree,
				  min_ptr->offset << 9, c->opts.btree_node_size);
		return;
	}

	bkey_for_each_ptr(ptrs, ptr) if (ptr->dev < c->sb.nr_devices &&
					 c->devs[ptr->dev])
		range_add(&devs[ptr->dev].btree, ptr->offset << 9,
			  c->opts.btree_node_size);
}

static void get_sb_journal(struct bch_fs *c, struct bch_dev *ca,
			   bool entire_journal, struct dump_dev *d)
{
	struct bch_sb *sb = ca->disk_sb.sb;

	/* Superblock layout: */
	range_add(&d->sb, BCH_SB_LAYOUT_SECTOR << 9,
		  sizeof(struct bch_sb_layout));

	/* Superblock copies: */
	for (unsigned i = 0; i < sb->layout.nr_superblocks; i++)
		range_add(&d->sb, le64_to_cpu(sb->layout.sb_offset[i]) << 9,
			  vstruct_bytes(sb));

	/* Journal buckets: */
	for (unsigned i = 0; i < ca->journal.nr; i++)
		if (entire_journal ||
		    ca->journal.bucket_seq[i] >= c->journal.last_seq_ondisk)
			range_add(&d->journal,
				  bucket_bytes(ca) * ca->journal.buckets[i],
				  bucket_bytes(ca));
}

static void dump_usage(void)
{
	puts("bcachefs dump - dump filesystem metadata\n"
	     "Usage: bcachefs dump [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -o output       Output qcow2 image(s)\n"
	     "  -f, --force     Force; overwrite when needed\n"
	     "  -s, --sanitize[=data|filenames]\n"
	     "                  Sanitize inline data and optionally filenames\n"
	     "      --single-replica\n"
	     "                  Dump only the lowest-device-index replica of each btree node\n"
	     "      --nojournal Don't dump entire journal, just dirty entries\n"
	     "      --noexcl    Open devices with O_NOEXCL (not recommended)\n"
	     "  -v, --verbose   Verbose output\n"
	     "  -h, --help      Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static void write_sanitized_ranges(struct qcow2_image *img, ranges *ranges,
				   struct bch_fs *c,
				   qcow2_sanitize_fn sanitize_fn,
				   struct sanitize_opts *opts)
{
	ranges_sort_merge(ranges);

	darray_for_each(*ranges, r) {
		size_t len = r->end - r->start;
		void *buf = xmalloc(len);
		xpread(img->infd, buf, len, r->start);
		sanitize_fn(c, buf, len, opts);
		qcow2_image_write_buf(img, buf, len, r->start);
		free(buf);
	}
}

int cmd_dump(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' },
		{ "sanitize", optional_argument, NULL, 's' },
		{ "single-replica", no_argument, NULL, '1' },
		{ "nojournal", no_argument, NULL, 'j' },
		{ "noexcl", no_argument, NULL, 'e' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct bch_opts opts = bch2_opts_empty();
	char *out = NULL;
	bool force = false, entire_journal = true;
	bool sanitize = false, sanitize_filenames = false, single_replica = false;
	int opt;

	opt_set(opts, direct_io, false);
	opt_set(opts, read_only, true);
	opt_set(opts, nochanges, true);
	opt_set(opts, norecovery, true);
	opt_set(opts, degraded, BCH_DEGRADED_very);
	opt_set(opts, errors, BCH_ON_ERROR_continue);
	opt_set(opts, fix_errors, FSCK_FIX_no);

	while ((opt = getopt_long(argc, argv, "o:fs:vh", longopts, NULL)) != -1)
		switch (opt) {
		case 'o':
			out = optarg;
			break;
		case 'f':
			force = true;
			break;
		case 's':
			sanitize = true;
			if (optarg && !strcmp(optarg, "filenames"))
				sanitize_filenames = true;
			else if (optarg && strcmp(optarg, "data"))
				die("Bad sanitize option: %s", optarg);
			break;
		case '1':
			single_replica = true;
			break;
		case 'j':
			entire_journal = false;
			break;
		case 'e':
			opt_set(opts, noexcl, true);
			break;
		case 'v':
			opt_set(opts, verbose, true);
			break;
		case 'h':
			dump_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!out)
		die("Please supply output filename");
	if (!argc)
		die("Please supply device(s) to dump");

	if (sanitize_filenames)
		printf("Sanitizing filenames and inline data extents\n");
	else if (sanitize)
		printf("Sanitizing inline data extents\n");
	if (single_replica)
		printf("Dumping only the lowest-device-index replica of each btree node\n");

	darray_const_str devs_list = get_or_split_cmdline_devs(argc, argv);
	struct bch_fs *c = bch2_fs_open(&devs_list, &opts, NULL);
	if (IS_ERR(c))
		die("error opening devices: %s", bch2_err_str(PTR_ERR(c)));

	struct dump_dev *devs = xcalloc(c->sb.nr_devices, sizeof(*devs));
	unsigned nr_online = 0;

	for_each_online_member(c, ca, 0)
	{
		if ((sanitize || single_replica) &&
		    (ca->mi.bucket_size % (c->opts.block_size >> 9)) != 0)
			die("device %u has unaligned buckets, cannot sanitize or de-replicate",
			    ca->dev_idx);

		get_sb_journal(c, ca, entire_journal, &devs[ca->dev_idx]);
		nr_online++;
	}

	for (unsigned i = 0; i < btree_id_nr_alive(c); i++) {
		struct btree_trans *trans = bch2_trans_get(c);
		int ret = for_each_btree_node(
			trans, iter, i, POS_MIN, 1, 0, b, ({
				struct btree_node_iter iter;
				struct bkey u;
				struct bkey_s_c k;
				for_each_btree_node_key_unpack(b, k, &iter, &u)
					dump_node_collect(c, devs, k, single_replica);
				0;
			}));
		if (ret)
			die("error %s walking btree %s", bch2_err_str(ret),
			    bch2_btree_id_str(i));

		struct btree *b = bch2_btree_id_root(c, i)->b;
		if (b && !btree_node_fake(b))
			dump_node_collect(c, devs, bkey_i_to_s_c(&b->key),
					  single_replica);
		bch2_trans_put(trans);
	}

	for_each_online_member(c, ca, 0)
	{
		int out_flags = O_WRONLY | O_CREAT | O_TRUNC |
				(force ? 0 : O_EXCL);
		char *path = nr_online > 1 ?
				     mprintf("%s.%u.qcow2", out, ca->dev_idx) :
					   mprintf("%s.qcow2", out);

		int out_fd = xopen(path, out_flags, 0600);
		free(path);

		struct qcow2_image *img = qcow2_image_open(
			ca->disk_sb.bdev->bd_fd, out_fd,
			max_t(unsigned, c->opts.btree_node_size / 8,
			      block_bytes(c)));

		qcow2_image_write_ranges(img, &devs[ca->dev_idx].sb);

		/* The journal carries the btree_root pointers, so it goes
		 * through the modify path for --single-replica (to
		 * de-replicate them) as well as --sanitize. */
		if (sanitize || single_replica) {
			struct sanitize_opts sopts = {
				.sanitize = sanitize,
				.sanitize_filenames = sanitize_filenames,
				.single_replica = single_replica,
			};

			write_sanitized_ranges(img, &devs[ca->dev_idx].journal,
					       c, sanitize_journal, &sopts);
			write_sanitized_ranges(img, &devs[ca->dev_idx].btree,
					       c, sanitize_btree, &sopts);
		} else {
			qcow2_image_write_ranges(img, &devs[ca->dev_idx].journal);
			qcow2_image_write_ranges(img, &devs[ca->dev_idx].btree);
		}

		qcow2_image_close(img);

		xclose(out_fd);

		darray_exit(&devs[ca->dev_idx].sb);
		darray_exit(&devs[ca->dev_idx].journal);
		darray_exit(&devs[ca->dev_idx].btree);
	}

	free(devs);
	bch2_fs_stop(c);
	darray_exit(&devs_list);
	return 0;
}

/* Undump implementation */

static void undump_usage(void)
{
	puts("bcachefs undump - convert qcow2 dump files back to raw device images\n"
	     "Usage: bcachefs undump [OPTION]... <files>\n"
	     "\n"
	     "Options:\n"
	     "  -f, --force     Overwrite existing output files\n"
	     "  -h, --help      Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_undump(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	bool force = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "fh", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			force = true;
			break;
		case 'h':
			undump_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply qcow2 file(s) to convert");

	for (int i = 0; i < argc; i++) {
		char *infile_path = argv[i];
		char *suffix = ".qcow2";
		size_t len = strlen(infile_path);

		if (len <= strlen(suffix) ||
		    strcmp(infile_path + len - strlen(suffix), suffix))
			die("%s not a qcow2 image?", infile_path);

		char *outfile_path = strdup(infile_path);
		outfile_path[len - strlen(suffix)] = '\0';

		if (!force && !access(outfile_path, F_OK))
			die("%s already exists", outfile_path);

		int infd = xopen(infile_path, O_RDONLY);
		int outfd = xopen(
			outfile_path,
			O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL), 0600);

		printf("Restoring %s -> %s\n", infile_path, outfile_path);
		qcow2_to_raw(infd, outfd);

		xclose(infd);
		xclose(outfd);
		free(outfile_path);
	}

	return 0;
}
