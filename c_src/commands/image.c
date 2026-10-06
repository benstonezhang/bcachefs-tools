/*
 * Image create/update commands — ported from src/commands/image.rs.
 *
 * Uses a temporary second device for metadata, writes data sequentially to the
 * primary device, then migrates metadata to the primary and drops the temp
 * device.
 *
 * GPLv2
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "alloc/background.h"
#include "alloc/foreground.h"
#include "alloc/accounting.h"
#include "alloc/accounting_format.h"
#include "alloc/disk_groups.h"
#include "btree/cache.h"
#include "btree/update.h"
#include "data/update.h"
#include "data/move.h"
#include "init/dev.h"
#include "init/fs.h"
#include "journal/reclaim.h"
#include "sb/members.h"
#include "cmds.h"
#include "crypto.h"

static u64 count_input_size(int dirfd)
{
	int fd = dup(dirfd);
	if (fd < 0)
		return 0;

	DIR *dir = fdopendir(fd);
	struct dirent *d;
	u64 bytes = 0;

	if (!dir) {
		close(fd);
		return 0;
	}

	while ((errno = 0), (d = readdir(dir))) {
		struct stat stat = xfstatat(fd, d->d_name, AT_SYMLINK_NOFOLLOW);

		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..") ||
		    !strcmp(d->d_name, "lost+found"))
			continue;

		bytes += stat.st_blocks << 9;

		if (mode_to_type(stat.st_mode) == DT_DIR) {
			int subfd = openat(fd, d->d_name, O_RDONLY | O_NOATIME);
			if (subfd >= 0) {
				bytes += count_input_size(subfd);
				xclose(subfd);
			}
		}
	}

	if (errno)
		die("readdir error: %m");
	closedir(dir);
	return bytes;
}

struct move_btree_args {
	bool move_alloc;
	unsigned target;
};

static int move_btree_pred(struct btree_trans *trans, void *_arg,
			   enum btree_id btree, struct bkey_s_c k,
			   struct bch_inode_opts *io_opts,
			   struct data_update_opts *data_opts)
{
	struct move_btree_args *args = _arg;

	data_opts->target = args->target;

	if (k.k->type != KEY_TYPE_btree_ptr_v2)
		return 0;

	if (!args->move_alloc && btree_id_is_alloc(btree))
		return 0;

	data_opts->write_flags |= BCH_WRITE_only_specified_devs;
	return 1;
}

static int move_btree(struct bch_fs *c, bool move_alloc, unsigned target_dev)
{
	bch2_journal_flush_all_pins(&c->journal);

	struct move_btree_args args = {
		.move_alloc = move_alloc,
		.target = dev_to_target(target_dev),
	};

	struct moving_context ctxt;
	bch2_moving_ctxt_init(&ctxt, c, NULL, NULL, writepoint_hashed(1),
			      false);
	int ret = 0;

	for (unsigned btree = 0; btree < BTREE_ID_NR; btree++) {
		if (!move_alloc && btree_id_is_alloc(btree))
			continue;

		for (unsigned level = 1; level < BTREE_MAX_DEPTH; level++) {
			ret = bch2_move_data_btree(&ctxt, POS_MIN, SPOS_MAX,
						   move_btree_pred, &args,
						   btree, level);
			if (ret)
				goto err;
		}
	}
err:
	bch2_moving_ctxt_exit(&ctxt);
	return ret;
}

static int get_nbuckets_used(struct bch_fs *c, u64 *nbuckets)
{
	struct btree_trans *trans = bch2_trans_get(c);
	struct btree_iter iter;
	bch2_trans_iter_init(trans, &iter, BTREE_ID_alloc, POS(0, U64_MAX), 0);
	struct bkey_s_c k;
	int ret = lockrestart_do(
		trans, bkey_err(k = bch2_btree_iter_peek_prev(&iter)));
	if (!ret && k.k->type != KEY_TYPE_alloc_v4)
		ret = -ENOENT;
	if (ret) {
		fprintf(stderr, "error looking up last alloc key: %s\n",
			bch2_err_str(ret));
		goto err;
	}

	*nbuckets = (k.k->p.offset + 1);
err:
	bch2_trans_iter_exit(&iter);
	bch2_trans_put(trans);
	return ret;
}

static void prt_sectors(struct printbuf *out, u64 v)
{
	prt_printf(out, "\t");
	prt_human_readable_u64(out, v << 9);
	prt_printf(out, "\r\n");
}

static void print_data_type_usage(struct printbuf *out, struct bch_dev *ca,
				  struct bch_dev_usage_full *usage,
				  unsigned data_type)
{
	struct bch_dev_usage_type *d = &usage->d[data_type];

	if (d->buckets) {
		bch2_prt_data_type(out, data_type);
		prt_sectors(out, bucket_to_sector(ca, d->buckets));
	}
	if (d->fragmented) {
		bch2_prt_data_type(out, data_type);
		prt_printf(out, " fragmented");
		prt_sectors(out, d->fragmented);
	}
}

static void print_image_usage(struct bch_fs *c, bool keep_alloc, u64 nbuckets)
{
	struct printbuf buf = PRINTBUF;
	buf.human_readable_units = true;

	struct bch_dev_usage_full usage = bch2_dev_usage_full_read(c->devs[0]);
	struct bch_dev *ca = c->devs[0];

	print_data_type_usage(&buf, ca, &usage, BCH_DATA_sb);
	print_data_type_usage(&buf, ca, &usage, BCH_DATA_journal);
	print_data_type_usage(&buf, ca, &usage, BCH_DATA_btree);

	{
		printbuf_indent_add(&buf, 2);

		for (unsigned i = 0; i < BTREE_ID_NR; i++) {
			if (btree_id_is_alloc(i) && !keep_alloc)
				continue;

			struct disk_accounting_pos acc_k = {
				.type = BCH_DISK_ACCOUNTING_btree,
				.btree.id = i,
			};
			struct bpos p = disk_accounting_pos_to_bpos(&acc_k);
			u64 v[1];
			bch2_accounting_mem_read(c, p, v, 1);

			if (v[0]) {
				bch2_btree_id_to_text(&buf, i);
				prt_sectors(&buf, v[0]);
			}
		}
		printbuf_indent_sub(&buf, 2);
	}

	/* User data via replicas accounting */
	struct disk_accounting_pos acc_k = {
		.type = BCH_DISK_ACCOUNTING_replicas,
		.replicas.data_type = BCH_DATA_user,
		.replicas.nr_devs = 1,
		.replicas.nr_required = 1,
	};
	acc_k.replicas.devs[0] = 0;

	struct bpos p = disk_accounting_pos_to_bpos(&acc_k);
	u64 v[3];
	bch2_accounting_mem_read(c, p, v, 1);
	prt_printf(&buf, "user");
	prt_sectors(&buf, v[0]);

	if (usage.d[BCH_DATA_user].fragmented) {
		prt_printf(&buf, "user fragmented");
		prt_sectors(&buf, usage.d[BCH_DATA_user].fragmented);
	}

	bch2_printbuf_tabstop_align(&buf);

	/* Compression stats */
	bool compression_header = false;
	for (unsigned i = 1; i < BCH_COMPRESSION_TYPE_NR; i++) {
		struct disk_accounting_pos acc_k = {
			.type = BCH_DISK_ACCOUNTING_compression,
			.compression.type = i,
		};
		struct bpos p = disk_accounting_pos_to_bpos(&acc_k);
		bch2_accounting_mem_read(c, p, v, 3);

		if (!v[0])
			continue;

		if (!compression_header) {
			prt_printf(
				&buf,
				"compression type\tcompressed\runcompressed\rratio\r\n");
			printbuf_indent_add(&buf, 2);
		}
		compression_header = true;

		u64 sectors_uncompressed = v[1];
		u64 sectors_compressed = v[2];

		bch2_prt_compression_type(&buf, i);
		prt_printf(&buf, "\t");

		prt_human_readable_u64(&buf, sectors_compressed << 9);
		prt_printf(&buf, "\r");

		if (i == BCH_COMPRESSION_TYPE_incompressible) {
			prt_newline(&buf);
			continue;
		}

		prt_human_readable_u64(&buf, sectors_uncompressed << 9);
		if (sectors_uncompressed > 0)
			prt_printf(&buf, "\r%llu%%\r\n",
				   sectors_compressed * 100 /
					   sectors_uncompressed);
		else
			prt_printf(&buf, "\r\n");
	}

	if (compression_header) {
		bch2_printbuf_tabstop_align(&buf);
		printbuf_indent_sub(&buf, 2);
	}

	prt_printf(&buf, "image size");
	prt_sectors(&buf, bucket_to_sector(ca, nbuckets));

	bch2_printbuf_tabstop_align(&buf);
	printf("%s", buf.buf);
	printbuf_exit(&buf);
}

/*
 * Finalize image: move btree to primary device, truncate, strip alloc info.
 */
static int finish_image(struct bch_fs *c, bool keep_alloc, unsigned verbosity)
{
	int ret;
	struct bch_member *m;

	if (verbosity > 1)
		printf("moving %stree to primary device\n",
		       keep_alloc ? "" : "non-alloc ");

	{
		guard(mutex_noio)(&c->sb_lock);
		m = bch2_members_v2_get_mut(c->disk_sb.sb, 0);
		SET_BCH_MEMBER_DATA_ALLOWED(m, BCH_MEMBER_DATA_ALLOWED(m) |
						       BIT(BCH_DATA_btree));
		bch2_write_super(c);
	}

	bch2_dev_allocator_set_rw(c, c->devs[0], true);

	ret = move_btree(c, keep_alloc, 0);
	if (ret) {
		fprintf(stderr,
			"error migrating btree from temporary device: %s\n",
			bch2_err_str(ret));
		return ret;
	}

	bch2_fs_read_only(c);

	u64 nbuckets;
	ret = get_nbuckets_used(c, &nbuckets);
	if (ret)
		return ret;

	if (verbosity > 0)
		print_image_usage(c, keep_alloc, nbuckets);

	if (ftruncate(c->devs[0]->disk_sb.bdev->bd_fd,
		      nbuckets * bucket_bytes(c->devs[0]))) {
		fprintf(stderr, "truncate error: %m\n");
		return -errno;
	}

	guard(mutex_noio)(&c->sb_lock);
	if (!keep_alloc) {
		if (verbosity > 1)
			printf("Stripping alloc info\n");
		strip_fs_alloc(c);
	}

	rcu_assign_pointer(c->devs[1], NULL);

	m = bch2_members_v2_get_mut(c->disk_sb.sb, 0);
	SET_BCH_MEMBER_DATA_ALLOWED(m, BCH_MEMBER_DATA_ALLOWED(m) |
					       BIT(BCH_DATA_journal));
	m->nbuckets = cpu_to_le64(nbuckets);

	for_each_online_member(c, ca, 0)
	{
		struct bch_member *m =
			bch2_members_v2_get_mut(c->disk_sb.sb, ca->dev_idx);
		SET_BCH_MEMBER_RESIZE_ON_MOUNT(m, true);
	}

	c->disk_sb.sb->features[0] |=
		cpu_to_le64(BIT_ULL(BCH_FEATURE_small_image));

	struct bch_sb_field_members_v2 *mi =
		bch2_sb_field_get(c->disk_sb.sb, members_v2);
	unsigned u64s = DIV_ROUND_UP(sizeof(struct bch_sb_field_members_v2) +
					     le16_to_cpu(mi->member_bytes),
				     sizeof(u64));
	bch2_sb_field_resize(&c->disk_sb, members_v2, u64s);
	c->disk_sb.sb->nr_devices = 1;
	SET_BCH_SB_MULTI_DEVICE(c->disk_sb.sb, false);

	bch2_write_super(c);

	return 0;
}

static void image_create(struct bch_opt_strs fs_opt_strs,
			 struct bch_opts fs_opts,
			 struct format_opts format_opts,
			 struct dev_opts dev_opts, const char *src_path,
			 bool keep_alloc, unsigned verbosity)
{
	int src_fd = xopen(src_path, O_RDONLY);

	if (!S_ISDIR(xfstat(src_fd).st_mode))
		die("%s is not a directory", src_path);

	u64 input_bytes = count_input_size(src_fd);
	lseek(src_fd, 0, SEEK_SET);

	dev_opts_list devs = {};
	darray_push(&devs, dev_opts);

	dev_opts.path = mprintf("%s.metadata", devs.data[0].path);
	darray_push(&devs, dev_opts);

	if (!access(devs.data[1].path, F_OK))
		die("temporary metadata device %s already exists",
		    devs.data[1].path);

	opt_set(devs.data[0].opts, data_allowed, BIT(BCH_DATA_user));
	opt_set(devs.data[1].opts, data_allowed,
		BIT(BCH_DATA_journal) | BIT(BCH_DATA_btree));

	u64 target_size = max(input_bytes * 2, (u64)64 << 20);

	darray_for_each(devs, dev)
	{
		int ret = open_for_format(dev, BLK_OPEN_CREAT, false);
		if (ret)
			die("Error opening %s: %s", dev->path, strerror(-ret));

		if (ftruncate(dev->bdev->bd_fd, target_size))
			die("ftruncate error: %m");
	}

	format_opts.no_sb_at_end = true;
	struct bch_sb *sb =
		bch2_format(fs_opt_strs, fs_opts, format_opts, devs);
	if (verbosity > 1) {
		struct printbuf buf = PRINTBUF;
		buf.human_readable_units = true;

		bch2_sb_to_text(&buf, NULL, sb, false,
				1 << BCH_SB_FIELD_members_v2);
		printf("%s", buf.buf);
		printbuf_exit(&buf);
	}

	darray_const_str device_paths = {};
	darray_for_each(devs, dev) darray_push(&device_paths, dev->path);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, copygc_enabled, false);
	opt_set(opts, reconcile_enabled, false);
	opt_set(opts, nostart, true);

	struct bch_fs *c = bch2_fs_open(&device_paths, &opts, NULL);
	if (IS_ERR(c))
		die("error opening %s: %s", device_paths.data[0],
		    bch2_err_str(PTR_ERR(c)));

	unlink(device_paths.data[1]);

	int ret = bch2_fs_start(c);
	if (ret)
		die("starting fs: %s", bch2_err_str(ret));

	struct copy_fs_state s = { .verbosity = verbosity };
	copy_fs(c, src_fd, src_path, &s);

	ret = finish_image(c, keep_alloc, verbosity);
	if (ret)
		die("finish_image error: %s", bch2_err_str(ret));

	bch2_fs_stop(c);
	free((void *)devs.data[1].path);
	darray_exit(&devs);
	darray_exit(&device_paths);
	xclose(src_fd);
}

static void image_update(const char *src_path, const char *dst_image,
			 bool keep_alloc, unsigned verbosity)
{
	int src_fd = xopen(src_path, O_RDONLY);
	struct stat src_stat = xfstat(src_fd);
	if (!S_ISDIR(src_stat.st_mode))
		die("%s is not a directory", src_path);

	u64 input_bytes = count_input_size(src_fd);
	lseek(src_fd, 0, SEEK_SET);

	struct stat dst_stat = xstat(dst_image);
	u64 new_size = dst_stat.st_size + input_bytes * 2;
	int dst_fd = xopen(dst_image, O_RDWR);
	if (ftruncate(dst_fd, new_size))
		die("truncate error: %m");
	xclose(dst_fd);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, copygc_enabled, false);
	opt_set(opts, reconcile_enabled, false);
	opt_set(opts, nostart, true);

	darray_const_str device_paths = {};
	darray_push(&device_paths, dst_image);

	struct bch_fs *c = bch2_fs_open(&device_paths, &opts, NULL);
	if (IS_ERR(c))
		die("error opening %s: %s", dst_image,
		    bch2_err_str(PTR_ERR(c)));

	char *metadata_path = mprintf("%s.metadata", dst_image);
	struct dev_opts dev_opts = dev_opts_default();
	dev_opts.path = metadata_path;

	int ret = open_for_format(&dev_opts, BLK_OPEN_CREAT, false);
	if (ret)
		die("Error opening %s: %s", metadata_path, strerror(-ret));

	/*
	 * Temp device needs enough space for btree nodes AND adequate journal
	 * for the btree migration workload. With small bucket sizes, the
	 * journal gets 1/128th of the device (min 8 buckets), which can be
	 * far too little. 64MB floor ensures reasonable journal capacity.
	 */
	u64 metadata_dev_size =
		max(input_bytes,
		    max((u64)c->opts.btree_node_size * BCH_MIN_NR_NBUCKETS,
			(u64)64 << 20));

	if (ftruncate(dev_opts.bdev->bd_fd, metadata_dev_size))
		die("ftruncate error: %m");

	ret = bch2_format_for_device_add(&dev_opts, c->opts.block_size,
					 c->opts.btree_node_size);
	if (ret)
		die("formatting metadata device: %s", bch2_err_str(ret));

	struct printbuf err = PRINTBUF;
	ret = bch2_dev_add(c, metadata_path, &err);
	if (ret)
		die("error adding metadata device: %s", err.buf);
	printbuf_exit(&err);

	/*
	 * Set data_allowed on both devices for image update:
	 * dev 0 gets user data only, dev 1 gets journal+btree.
	 */
	{
		guard(mutex_noio)(&c->sb_lock);
		struct bch_member *m0 = bch2_members_v2_get_mut(c->disk_sb.sb, 0);
		SET_BCH_MEMBER_DATA_ALLOWED(m0, BIT(BCH_DATA_user));

		struct bch_member *m1 = bch2_members_v2_get_mut(c->disk_sb.sb, 1);
		SET_BCH_MEMBER_DATA_ALLOWED(m1, BIT(BCH_DATA_journal) |
							BIT(BCH_DATA_btree));
		bch2_write_super(c);
	}

	bch2_dev_allocator_set_rw(c, c->devs[0], true);
	bch2_dev_allocator_set_rw(c, c->devs[1], true);

	ret = bch2_fs_start(c);
	if (ret)
		die("starting fs: %s", bch2_err_str(ret));

	unlink(metadata_path);

	if (verbosity > 1)
		printf("Moving btree to temp device\n");

	ret = move_btree(c, true, 1);
	if (ret)
		die("migrating btree to temp device: %s", bch2_err_str(ret));

	if (verbosity > 1)
		printf("Deleting xattrs\n");

	ret = bch2_btree_delete_range(c, BTREE_ID_xattrs, POS_MIN, SPOS_MAX,
				      BTREE_ITER_all_snapshots);
	if (ret)
		die("deleting xattrs: %s", bch2_err_str(ret));

	if (verbosity > 1)
		printf("Syncing data\n");

	struct copy_fs_state s = { .verbosity = verbosity };
	copy_fs(c, src_fd, src_path, &s);

	ret = finish_image(c, keep_alloc, verbosity);
	if (ret)
		die("finish_image error: %s", bch2_err_str(ret));

	bch2_fs_stop(c);
	darray_exit(&device_paths);
	xclose(src_fd);
	free(metadata_path);
}

static void image_create_usage(void)
{
	puts("bcachefs image create - create a minimum size, reproducible filesystem image\n"
	     "Usage: bcachefs image create [OPTION]... <file>\n"
	     "\n"
	     "Options:\n"
	     "      --source=path           Source directory to be used as content for the new image\n"
	     "  -a, --keep-alloc            Include allocation info in the filesystem\n"
	     "                              6.16+ regenerates alloc info on first rw mount\n"
	     "      --replicas=#            Sets both data and metadata replicas\n"
	     "      --encrypted             Enable whole filesystem encryption (chacha20/poly1305)\n"
	     "  -L, --fs_label=label\n"
	     "  -U, --uuid=uuid\n"
	     "      --superblock_size=size\n"
	     "      --bucket_size=size\n"
	     "      --fs_size=size          Expected size of device image will be used on, hint for bucket size\n"
	     "  -l, --label=label          Disk label\n"
	     "      --version=version       Create filesystem with specified on disk format version\n"
	     "  -f, --force\n"
	     "  -q, --quiet                 Only print errors\n"
	     "  -v, --verbose               Verbose filesystem initialization\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int cmd_image_create(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "source", required_argument, NULL, 's' },
		{ "keep-alloc", no_argument, NULL, 'a' },
		{ "replicas", required_argument, NULL, 'r' },
		{ "encrypted", no_argument, NULL, 'e' },
		{ "passphrase_file", required_argument, NULL, 'p' },
		{ "no_passphrase", no_argument, NULL, 'n' },
		{ "fs_label", required_argument, NULL, 'L' },
		{ "label", required_argument, NULL, 'l' },
		{ "fs_size", required_argument, NULL, 1000 },
		{ "uuid", required_argument, NULL, 'U' },
		{ "superblock_size", required_argument, NULL, 'S' },
		{ "version", required_argument, NULL, 'V' },
		{ "force", no_argument, NULL, 'f' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct format_opts opts = format_opts_default();
	struct dev_opts dev_opts = dev_opts_default();
	bool keep_alloc = false, no_passphrase = false;
	unsigned verbosity = 1;
	struct bch_opt_strs fs_opt_strs = {};
	struct bch_opts fs_opts = bch2_opts_empty();

	opts.superblock_size = 128; /* 64k */

	while (true) {
		const struct bch_option *opt = bch2_cmdline_opt_parse(
			argc, argv, OPT_FORMAT | OPT_FS | OPT_DEVICE);
		if (opt) {
			unsigned id = opt - bch2_opt_table;
			u64 v;
			struct printbuf err = PRINTBUF;
			int ret = bch2_opt_parse(NULL, opt, optarg, &v, &err);
			if (ret == -BCH_ERR_option_needs_open_fs) {
				fs_opt_strs.by_id[id] = strdup(optarg);
				continue;
			}
			if (ret)
				die("invalid option: %s", err.buf);

			if (opt->flags & OPT_DEVICE)
				bch2_opt_set_by_id(&dev_opts.opts, id, v);
			else if (opt->flags & OPT_FS)
				bch2_opt_set_by_id(&fs_opts, id, v);
			else
				die("got bch_opt of wrong type %s",
				    opt->attr.name);

			continue;
		}

		int optid = getopt_long(argc, argv, "s:ar:ep:nl:L:U:S:V:fqvh",
					longopts, NULL);
		if (optid == -1)
			break;

		switch (optid) {
		case 's':
			opts.source = optarg;
			break;
		case 'a':
			keep_alloc = true;
			break;
		case 'r': {
			unsigned v;
			if (kstrtouint(optarg, 10, &v) || v < 1 ||
			    v > BCH_REPLICAS_MAX)
				die("invalid replicas");
			opt_set(fs_opts, metadata_replicas, v);
			opt_set(fs_opts, data_replicas, v);
			break;
		}
		case 'e':
			opts.encrypted = true;
			break;
		case 'p':
			opts.passphrase_file = optarg;
			break;
		case 'n':
			no_passphrase = true;
			break;
		case 'L':
			opts.label = optarg;
			break;
		case 'l':
			dev_opt_str_push(&dev_opts, Opt_label, optarg);
			break;
		case 1000:
			if (bch2_strtoull_h(optarg, &dev_opts.fs_size))
				die("invalid filesystem size");
			break;
		case 'U':
			if (uuid_parse(optarg, opts.uuid.b))
				die("Bad uuid");
			break;
		case 'S':
			if (bch2_strtouint_h(optarg, &opts.superblock_size))
				die("invalid superblock size");

			opts.superblock_size >>= 9;
			break;
		case 'V':
			opts.version = version_parse(optarg);
			break;
		case 'f':
			break;
		case 'q':
			verbosity = 0;
			break;
		case 'v':
			verbosity++;
			break;
		case 'h':
			image_create_usage();
			exit(EXIT_SUCCESS);
			break;
		case '?':
			exit(EXIT_FAILURE);
			break;
		default:
			die("getopt ret %i %c", optid, optid);
		}
	}
	args_shift(optind);

	if (argc != 1)
		die("Please supply a filename for the new image");

	if (!opts.source)
		die("--source is required");

	if (opts.passphrase_file && !opts.encrypted)
		die("--passphrase_file requires --encrypted");
	if (opts.passphrase_file && no_passphrase)
		die("--passphrase_file, --no_passphrase are incompatible");

	if (no_passphrase)
		setenv("BCACHEFS_NO_PASSPHRASE", "1", 1);

	dev_opts.path = argv[0];

	image_create(fs_opt_strs, fs_opts, opts, dev_opts, opts.source,
		     keep_alloc, verbosity);
	bch2_opt_strs_free(&fs_opt_strs);
	return 0;
}

static void image_update_usage(void)
{
	puts("bcachefs image update - update a filesystem image from a directory\n"
	     "Usage: bcachefs image update [OPTION]... <file>\n"
	     "\n"
	     "Options:\n"
	     "      --source=path           Source directory to be used as content for the update\n"
	     "  -a, --keep-alloc            Include allocation info in the filesystem\n"
	     "  -q, --quiet                 Only print errors\n"
	     "  -v, --verbose               Verbose output\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int cmd_image_update(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "source", required_argument, NULL, 's' },
		{ "keep-alloc", no_argument, NULL, 'a' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	const char *source = NULL;
	bool keep_alloc = false;
	unsigned verbosity = 1;

	int optid;
	while ((optid = getopt_long(argc, argv, "s:aqvh", longopts, NULL)) !=
	       -1) {
		switch (optid) {
		case 's':
			source = optarg;
			break;
		case 'a':
			keep_alloc = true;
			break;
		case 'q':
			verbosity = 0;
			break;
		case 'v':
			verbosity++;
			break;
		case 'h':
			image_update_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	}
	args_shift(optind);

	if (argc != 1)
		die("Please supply an image file to update");

	if (!source)
		die("--source is required");

	image_update(source, argv[0], keep_alloc, verbosity);
	return 0;
}

static int image_usage(void)
{
	puts("bcachefs image - commands for creating and updating image files\n"
	     "Usage: bcachefs image <CMD> [OPTION]...\n"
	     "\n"
	     "Commands:\n"
	     "  create                  Create a minimally-sized disk image\n"
	     "  update                  Update an existing disk image\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
	return 0;
}

int cmd_image(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);

	if (!cmd)
		return image_usage();
	if (!strcmp(cmd, "create"))
		return cmd_image_create(argc, argv);
	if (!strcmp(cmd, "update"))
		return cmd_image_update(argc, argv);

	image_usage();
	return -EINVAL;
}
