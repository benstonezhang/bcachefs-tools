/*
 * list: List filesystem metadata in textual form.
 *
 * Lists btree contents in human-readable text. Operates on unmounted
 * devices in read-only mode; if the filesystem is mounted (device, mount
 * point, or UUID), keys are listed via the kernel instead.
 * Modes: keys (default) prints key/value pairs, formats shows btree node
 * packing format, nodes shows btree node keys, nodes-ondisk shows the raw
 * on-disk representation.
 *
 * Ported from src/commands/list.rs.
 *
 * GPLv2
 */

#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <sys/ioctl.h>

#include "libbcachefs.h"
#include "btree/iter.h"
#include "debug/debug.h"
#include "init/fs.h"
#include "sb/members.h"
#include "cmds.h"

enum list_modes {
	MODE_KEYS,
	MODE_FORMATS,
	MODE_NODES,
	MODE_NODES_ON_DISK,
};

struct list_opts {
	enum btree_id btree;
	enum bch_bkey_type bkey_type;
	unsigned level;
	struct bpos start;
	struct bpos end;
	enum list_modes mode;
	bool fsck;
	unsigned verbose;
};

static void list_usage(void)
{
	puts("bcachefs list - list filesystem metadata in textual form\n"
	     "Usage: bcachefs list [OPTION]... <devices>\n"
	     "\n"
	     "Operates on unmounted devices in read-only mode; if the filesystem\n"
	     "is mounted (device, mount point, or UUID), keys are listed via the\n"
	     "kernel instead.\n"
	     "\n"
	     "Options:\n"
	     "  -b, --btree=btree      Btree to list from (default: extents)\n"
	     "  -k, --type=type        Bkey type to list\n"
	     "  -l, --level=level      Btree depth to descend to (0 = leaves, default: 0)\n"
	     "  -s, --start=pos        Start position to list from (default: POS_MIN)\n"
	     "  -e, --end=pos          End position (default: SPOS_MAX)\n"
	     "  -m, --mode=mode        possible values: keys, formats, nodes, nodes-ondisk. (default: keys)\n"
	     "  -f, --fsck             Check the filesystem first\n"
	     "  -c, --colorize         Enable colorization (ignored)\n"
	     "  -v, --verbose          Verbose mode\n"
	     "  -h, --help             Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int list_keys(struct bch_fs *c, struct list_opts *opts)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	unsigned flags = BTREE_ITER_prefetch;

	if (!opts->start.snapshot)
		flags |= BTREE_ITER_all_snapshots;

	bch2_trans_begin(trans);
	bch2_trans_node_iter_init(trans, &iter, opts->btree, opts->start, 0, opts->level, flags);

	for_each_btree_key_max_continue(trans, iter, opts->end, flags, k, ({
		if (opts->bkey_type < KEY_TYPE_MAX &&
		    k.k->type != opts->bkey_type)
			continue;

		struct printbuf buf = PRINTBUF;
		bch2_bkey_val_to_text(&buf, c, k);
		printf("%s\n", buf.buf);
		printbuf_exit(&buf);
		0;
	}));

	bch2_trans_put(trans);
	return 0;
}

static int list_btree_nodes(struct bch_fs *c, struct list_opts *opts)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	unsigned flags = BTREE_ITER_prefetch;

	for (unsigned level = opts->level; level < BTREE_MAX_DEPTH; level++) {
		bch2_trans_node_iter_init(trans, &iter, opts->btree, opts->start, 0,
					  level, flags);

		while (1) {
			struct btree *b;
			lockrestart_do(iter.trans,
				       PTR_ERR_OR_ZERO(b = bch2_btree_iter_peek_node(&iter)));
			if (!b)
				break;
			if (bpos_cmp(b->key.k.p, opts->end) > 0)
				break;

			struct printbuf buf = PRINTBUF;
			switch (opts->mode) {
			case MODE_FORMATS:
				bch2_btree_node_to_text(&buf, c, b);
				break;
			case MODE_NODES:
				bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(&b->key));
				break;
			case MODE_NODES_ON_DISK:
				bch2_btree_node_ondisk_to_text(&buf, c, b);
				break;
			default:
				break;
			}
			printf("%s\n", buf.buf);
			printbuf_exit(&buf);

			bch2_btree_iter_advance(&iter);
		}

		bch2_trans_iter_exit(&iter);
	}

	bch2_trans_put(trans);
	return 0;
}

/**
 * list_keys_online - List keys from a mounted filesystem: the keys come
 * from the kernel via BCH_IOCTL_QUERY_BTREE_KEYS, and are formatted with
 * a userspace bch_fs opened noexcl|nostart alongside the mount - never
 * started, so the journal is never read; everything key formatting needs
 * (extent entry tables, member names, disk groups) comes from the
 * superblock. Output is identical to the offline path by construction.
 *
 * Ported from src/commands/list.rs:list_keys_online and
 * src/wrappers/online_iter.rs.
 */
static int list_keys_online(struct bchfs_handle *fs, struct bch_fs *c,
			    struct list_opts *opts)
{
	struct bch_ioctl_query_btree_keys arg = {
		.btree = opts->btree,
		.level = opts->level,
		.flags = opts->start.snapshot == 0
			? BCH_IOCTL_QUERY_BTREE_KEYS_all_snapshots : 0,
		.start = opts->start,
		.end = opts->end,
	};

	size_t buf_size = 1 << 20;
	void *buf = xmalloc(buf_size);

	while (1) {
		arg.buf = (u64)buf;
		arg.buf_size = buf_size;
		arg.used = 0;

		if (ioctl(fs->ioctl_fd, BCH_IOCTL_QUERY_BTREE_KEYS, &arg)) {
			/* Buffer can't fit even one key - grow and retry: */
			if (errno == ERANGE && buf_size < 1 << 30) {
				buf_size *= 2;
				buf = xrealloc(buf, buf_size);
				continue;
			}
			die("BCH_IOCTL_QUERY_BTREE_KEYS: %s", strerror(errno));
		}

		u32 pos = 0;
		while (pos < arg.used) {
			u32 rest = arg.used - pos;

			/* Trust boundary for buffer contents from the ioctl:
			 * validate the record header before stepping by it,
			 * or a zero/short u64s spins us forever or walks off
			 * the valid region. */
			if (rest < sizeof(struct bkey))
				die("BCH_IOCTL_QUERY_BTREE_KEYS: %s",
				    strerror(EPROTO));

			struct bkey_i *k = (struct bkey_i *)((u8 *)buf + pos);
			unsigned bytes = k->k.u64s * sizeof(u64);

			if (bytes < sizeof(struct bkey) || bytes > rest)
				die("BCH_IOCTL_QUERY_BTREE_KEYS: %s",
				    strerror(EPROTO));
			pos += bytes;

			if (bpos_cmp(k->k.p, opts->end) > 0)
				goto done;

			if (opts->bkey_type < KEY_TYPE_MAX &&
			    k->k.type != opts->bkey_type)
				continue;

			struct printbuf out = PRINTBUF;
			bch2_bkey_val_to_text(&out, c, bkey_i_to_s_c(k));
			printf("%s\n", out.buf);
			printbuf_exit(&out);
		}

		if (arg.done)
			break;
	}
done:
	free(buf);
	return 0;
}

static int list_online(struct bchfs_handle *fs, struct bch_fs *c,
		       struct list_opts *opts)
{
	if (opts->mode != MODE_KEYS)
		die("only 'keys' mode is supported on a mounted filesystem");
	if (opts->fsck)
		die("--fsck requires the filesystem to be unmounted; use "
		    "'bcachefs fsck' for online fsck");

	return list_keys_online(fs, c, opts);
}

int cmd_list(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "btree", required_argument, NULL, 'b' },
		{ "type", required_argument, NULL, 'k' },
		{ "level", required_argument, NULL, 'l' },
		{ "start", required_argument, NULL, 's' },
		{ "end", required_argument, NULL, 'e' },
		{ "mode", required_argument, NULL, 'm' },
		{ "fsck", no_argument, NULL, 'f' },
		{ "colorize", no_argument, NULL, 'c' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct list_opts opts = {
		.btree = BTREE_ID_extents,
		.bkey_type = KEY_TYPE_MAX,
		.level = 0,
		.start = POS_MIN,
		.end = SPOS_MAX,
		.mode = MODE_KEYS,
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "b:k:l:s:e:m:fc:vh", longopts,
				  NULL)) != -1)
		switch (opt) {
		case 'b':
			opts.btree = read_flag_list_or_die(
				optarg, __bch2_btree_ids, "btree id");
			break;
		case 'k':
			opts.bkey_type = read_string_list_or_die(
				optarg, bch2_bkey_types, "bkey type");
			break;
		case 'l':
			if (kstrtouint(optarg, 10, &opts.level))
				die("invalid level %s", optarg);
			break;
		case 's':
			opts.start = bpos_parse(optarg);
			break;
		case 'e':
			opts.end = bpos_parse(optarg);
			break;
		case 'm':
			if (!strcmp(optarg, "keys"))
				opts.mode = MODE_KEYS;
			else if (!strcmp(optarg, "formats"))
				opts.mode = MODE_FORMATS;
			else if (!strcmp(optarg, "nodes"))
				opts.mode = MODE_NODES;
			else if (!strcmp(optarg, "nodes-ondisk"))
				opts.mode = MODE_NODES_ON_DISK;
			else
				die("invalid mode %s", optarg);
			break;
		case 'f':
			opts.fsck = true;
			break;
		case 'c':
			/* ignored for now, parity with rust */
			break;
		case 'v':
			opts.verbose = 1;
			break;
		case 'h':
			list_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply device(s)");

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_opts bch_opts = bch2_opts_empty();
	opt_set(bch_opts, noexcl, true);
	opt_set(bch_opts, nochanges, true);
	opt_set(bch_opts, read_only, true);
	opt_set(bch_opts, norecovery, true);
	opt_set(bch_opts, degraded, BCH_DEGRADED_very);
	opt_set(bch_opts, errors, BCH_ON_ERROR_continue);

	if (opts.fsck) {
		opt_set(bch_opts, fix_errors, FSCK_FIX_yes);
		opt_set(bch_opts, norecovery, false);
	}
	if (opts.verbose)
		opt_set(bch_opts, verbose, true);

	/* The filesystem is mounted: read keys through the kernel. */
	struct bchfs_handle fs;
	int mounted_ret = bcache_fs_open_if_mounted_any(devs.data, devs.nr,
							&fs);
	if (mounted_ret < 0)
		die("error opening filesystem: %s", strerror(-mounted_ret));

	if (mounted_ret == 0) {
		fprintf(stderr, "filesystem is mounted, listing via the kernel\n");

		/*
		 * For formatting them we still want a bch_fs - everything
		 * to_text needs is derived from the superblock - so open one
		 * noexcl|nostart: no exclusive claim on the mounted devices,
		 * never started, journal never read. Opened from the member
		 * block devices (from sysfs) - the path we were given may be
		 * a mount point or UUID, which aren't openable as devices.
		 */
		dev_names member_devs = bchu_fs_get_devices(fs);
		darray_const_str member_paths = { 0 };

		darray_for_each(member_devs, n) {
			if (n->online) {
				char *dev = mprintf("/dev/%s", n->dev);
				darray_push(&member_paths, dev);
			}
		}

		if (!member_paths.nr)
			die("no online member devices for mounted filesystem");

		opt_set(bch_opts, nostart, true);
		struct bch_fs *c = bch2_fs_open(&member_paths, &bch_opts);
		if (IS_ERR(c))
			die("error opening %s: %s", member_paths.data[0],
			    bch2_err_str(PTR_ERR(c)));

		int ret = list_online(&fs, c, &opts);

		bch2_fs_stop(c);
		for (unsigned i = 0; i < member_paths.nr; i++)
			free((void *)member_paths.data[i]);
		darray_exit(&member_paths);
		dev_names_free(&member_devs);
		bcache_fs_close(fs);
		darray_exit(&devs);
		return ret;
	}

	bcache_fs_close(fs);

	struct bch_fs *c = bch2_fs_open(&devs, &bch_opts);
	if (IS_ERR(c))
		die("error opening %s: %s", devs.data[0],
		    bch2_err_str(PTR_ERR(c)));

	int ret = 0;
	if (opts.mode == MODE_KEYS)
		ret = list_keys(c, &opts);
	else
		ret = list_btree_nodes(c, &opts);

	bch2_fs_stop(c);
	darray_exit(&devs);
	return ret;
}
