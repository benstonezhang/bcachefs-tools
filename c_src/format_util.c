/*
 * format: Create a new bcachefs filesystem.
 *
 * This uses manual argument parsing rather than standard library/clap because:
 * - Per-device options (--label, --discard, etc.) must apply to the next
 *   device on the command line, not globally.
 * - The C opts table (bch_opts) is dynamic and parsed via bch2_parse_one_opt.
 * - Several options (--encrypted, --replicas, --label) have special handling
 *   that maps to multiple underlying C opts.
 *
 * The format flow: parse args → build bch_opt_strs → call bch2_format() →
 * optionally write encryption key → print superblock.
 *
 * Ported from src/commands/format.rs.
 */

/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * C implementation of bch2_format and bch2_format_for_device_add.
 * Ported from src/commands/format_util.rs.
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <uuid/uuid.h>
#include <blkid/blkid.h>

#include "libbcachefs.h"
#include "alloc/disk_groups.h"
#include "sb/io.h"
#include "crypto.h"

#include <sys/sysinfo.h>

#define TARGET_DEV_START 1
#define TARGET_GROUP_START (256 + TARGET_DEV_START)

struct format_opts format_opts_default()
{
	/*
	 * Ensure bcachefs module is loaded so we know the supported on disk
	 * format version:
	 */
	(void)!system("modprobe bcachefs > /dev/null 2>&1");

	unsigned kernel_version = bcachefs_kernel_version();
	unsigned current_version = bcachefs_metadata_version_current;

	return (struct format_opts){
		.version = kernel_version > 0 ?
				min(current_version, kernel_version) :
				current_version,
		.superblock_size = SUPERBLOCK_SIZE_DEFAULT,
	};
}

/*
 * Open the device for formatting, with blkid checks.
 *
 * Adds READ|WRITE|EXCL|BUFFERED to the given extra flags
 */
int open_for_format(struct dev_opts *dev, blk_mode_t mode, bool force)
{
	int blkid_version_code = blkid_get_library_version(NULL, NULL);
	if (blkid_version_code < 2401) {
		if (force) {
			fprintf(stderr,
				"Continuing with out of date libblkid %s because --force was passed.\n",
				BLKID_VERSION);
		} else {
			die("Refusing to format when using libblkid %s\n"
			    "libblkid >= 2.40.1 is required to check for existing filesystems\n"
			    "Earlier versions may not recognize some bcachefs filesystems.\n",
			    BLKID_VERSION);
		}
	}

	blkid_probe pr;
	const char *fs_type = NULL, *fs_label = NULL;
	size_t fs_type_len, fs_label_len;

	dev->file = bdev_file_open_by_path(dev->path,
					   BLK_OPEN_READ | BLK_OPEN_WRITE |
						   BLK_OPEN_EXCL |
						   BLK_OPEN_BUFFERED | mode,
					   dev, NULL);
	int ret = PTR_ERR_OR_ZERO(dev->file);
	if (ret < 0)
		die("Error opening device to format %s: %s", dev->path,
		    strerror(-ret));
	dev->bdev = file_bdev(dev->file);

	if (!(pr = blkid_new_probe()))
		die("blkid error 1");
	if (blkid_probe_set_device(pr, dev->bdev->bd_fd, 0, 0))
		die("blkid error 2");
	if (blkid_probe_enable_partitions(pr, true) ||
	    blkid_probe_enable_superblocks(pr, true) ||
	    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_LABEL |
							  BLKID_SUBLKS_TYPE |
							  BLKID_SUBLKS_MAGIC))
		die("blkid error 3");
	if (blkid_do_fullprobe(pr) < 0)
		die("blkid error 4");

	blkid_probe_lookup_value(pr, "TYPE", &fs_type, &fs_type_len);
	blkid_probe_lookup_value(pr, "LABEL", &fs_label, &fs_label_len);

	if (fs_type) {
		if (fs_label)
			printf("%s contains a %s filesystem labelled '%s'\n",
			       dev->path, fs_type, fs_label);
		else
			printf("%s contains a %s filesystem\n", dev->path,
			       fs_type);
		if (!force) {
			fputs("Proceed anyway?", stdout);
			if (!ask_yn())
				exit(EXIT_FAILURE);
		}
		while (blkid_do_probe(pr) == 0) {
			if (blkid_do_wipe(pr, 0))
				die("Failed to wipe preexisting metadata.");
		}
	}

	blkid_free_probe(pr);
	return ret;
}

static unsigned parse_target(struct bch_sb_handle *sb, dev_opts_list devs,
			     const char *s)
{
	if (!s)
		return 0;

	darray_for_each(devs, i)
	{
		if (!strcmp(s, i->path))
			return dev_to_target(i - devs.data);
	}

	int idx = bch2_disk_path_find(sb, s);
	if (idx >= 0)
		return group_to_target(idx);

	die("Invalid target %s", s);
	return 0;
}

static void bch2_opt_set_sb_all(struct bch_sb *sb, int dev_idx,
				struct bch_opts *opts)
{
	for (unsigned id = 0; id < bch2_opts_nr; id++) {
		u64 v = bch2_opt_defined_by_id(opts, id) ?
				bch2_opt_get_by_id(opts, id) :
				      bch2_opt_get_by_id(&bch2_opts_default, id);

		__bch2_opt_set_sb(sb, dev_idx, &bch2_opt_table[id], v, NULL);
	}
}

static u64 total_system_ram(void)
{
	struct sysinfo info;
	if (sysinfo(&info))
		die("sysinfo() failed: %m");
	return (u64)info.totalram * info.mem_unit;
}

static u64 dev_bucket_size_clamp(struct bch_opts fs_opts, u64 dev_size,
				 u64 fs_bucket_size)
{
	u64 min_nr_nbuckets = BCH_MIN_NR_NBUCKETS;

	/* Largest bucket size that still gives >= 2048 buckets */
	u64 max_size = rounddown_pow_of_two(dev_size / (min_nr_nbuckets * 4));
	if (opt_defined(fs_opts, btree_node_size))
		max_size = max(max_size, (u64)fs_opts.btree_node_size);

	if (max_size * min_nr_nbuckets > dev_size)
		die("bucket size %llu too big for device size", max_size);

	u64 dev_bucket_size = min(max_size, fs_bucket_size);

	/* Buckets >= encoded_extent_max avoid fragmenting encoded extents */
	u64 extent_min = opt_get(fs_opts, encoded_extent_max);
	while (dev_bucket_size < extent_min && dev_bucket_size < max_size)
		dev_bucket_size *= 2;

	return dev_bucket_size;
}

u64 bch2_pick_bucket_size(struct bch_opts opts, dev_opts_list devs)
{
	/* Hard minimum: bucket must hold a btree node */
	u64 bucket_size = opts.block_size;
	if (opt_defined(opts, btree_node_size))
		bucket_size = max(bucket_size, (u64)opts.btree_node_size);

	u64 min_dev_size = BCH_MIN_NR_NBUCKETS * bucket_size;
	darray_for_each(devs, i)
	{
		if (i->fs_size < min_dev_size)
			die("cannot format %s, too small (%llu bytes, min %llu)",
			    i->path, i->fs_size, min_dev_size);
	}

	u64 total_fs_size = 0;
	darray_for_each(devs, i) total_fs_size += i->fs_size;

	bucket_size = max(bucket_size, 256ULL << 10);
	bucket_size =
		max(bucket_size, (u64)opt_get(opts, encoded_extent_max) * 4);

	u64 perf_lower_bound = min(2ULL << 20, total_fs_size / (1ULL << 20));
	bucket_size = max(bucket_size, perf_lower_bound);

	u64 total_ram = total_system_ram();
	u64 mem_available_for_fsck = total_ram / 8;
	u64 bucket_struct_size = sizeof(struct bucket);
	u64 buckets_can_fsck =
		mem_available_for_fsck / (bucket_struct_size * 3 / 2);
	u64 mem_lower_bound = total_fs_size / buckets_can_fsck;
	bucket_size = max(bucket_size, roundup_pow_of_two(mem_lower_bound));

	return min(1ULL << 31, roundup_pow_of_two(bucket_size));
}

u32 bch2_pick_block_size(struct bch_opts opts, dev_opts_list devs)
{
	u64 total_size = 0;
	darray_for_each(devs, i) total_size += i->fs_size;

	u32 block_size = 512;

	if (total_size >= 1ULL << 30) {
		block_size = 4096;
		darray_for_each(devs, i) block_size =
			max(block_size, get_blocksize(i->bdev->bd_fd));
	}

	return min(block_size, (u32)(1 << 15));
}

void bch2_check_bucket_size(struct bch_opts opts, struct dev_opts *dev)
{
	if (dev->opts.bucket_size < opts.block_size)
		die("Bucket size (%u) cannot be smaller than block size (%u)",
		    dev->opts.bucket_size, opts.block_size);

	if (opt_defined(opts, btree_node_size) &&
	    dev->opts.bucket_size < opts.btree_node_size)
		die("Bucket size (%u) cannot be smaller than btree node size (%u)",
		    dev->opts.bucket_size, opts.btree_node_size);

	if (dev->nbuckets < BCH_MIN_NR_NBUCKETS)
		die("Not enough buckets: %llu, need %u (bucket size %u)",
		    dev->nbuckets, BCH_MIN_NR_NBUCKETS, dev->opts.bucket_size);
}

struct bch_sb *bch2_format(struct bch_opt_strs fs_opt_strs,
			   struct bch_opts fs_opts, struct format_opts opts,
			   dev_opts_list devs)
{
	struct bch_sb_handle sb = { NULL };

	darray_for_each(devs, i)
	{
		if (!i->fs_size)
			i->fs_size = get_size(i->bdev->bd_fd);
	}

	if (!opt_defined(fs_opts, block_size))
		opt_set(fs_opts, block_size,
			bch2_pick_block_size(fs_opts, devs));

	if (fs_opts.block_size < 512)
		die("blocksize too small: %u, must be greater than one sector (512 bytes)",
		    fs_opts.block_size);

	u64 fs_bucket_size = bch2_pick_bucket_size(fs_opts, devs);

	darray_for_each(devs, i)
	{
		if (!opt_defined(i->opts, bucket_size))
			opt_set(i->opts, bucket_size,
				dev_bucket_size_clamp(fs_opts, i->fs_size,
						      fs_bucket_size));
	}

	darray_for_each(devs, i)
	{
		i->nbuckets = i->fs_size / i->opts.bucket_size;
		bch2_check_bucket_size(fs_opts, i);
	}

	if (!opt_defined(fs_opts, btree_node_size)) {
		unsigned s = bch2_opts_default.btree_node_size;
		darray_for_each(devs, i)
			s = min(s, (unsigned)i->opts.bucket_size);
		opt_set(fs_opts, btree_node_size, s);
	}

	if (fs_opts.btree_node_size <= fs_opts.block_size)
		die("btree node size (%u) must be greater than block size (%u)",
		    fs_opts.btree_node_size, fs_opts.block_size);

	if (uuid_is_null(opts.uuid.b))
		uuid_generate(opts.uuid.b);

	if (bch2_sb_realloc(&sb, 0))
		die("insufficient memory");

	sb.sb->version = cpu_to_le16(opts.version);
	sb.sb->version_min = cpu_to_le16(opts.version);
	sb.sb->magic = BCHFS_MAGIC;
	sb.sb->user_uuid = opts.uuid;
	sb.sb->nr_devices = devs.nr;

	SET_BCH_SB_VERSION_INCOMPAT_ALLOWED(sb.sb, opts.version);
	SET_BCH_SB_META_REPLICAS_REQ(sb.sb, 1);
	SET_BCH_SB_DATA_REPLICAS_REQ(sb.sb, 1);
	SET_BCH_SB_EXTENT_BP_SHIFT(sb.sb, 16);

	if (opts.version > bcachefs_metadata_version_disk_accounting_big_endian)
		sb.sb->features[0] |= cpu_to_le64(BCH_SB_FEATURES_ALL);

	uuid_generate(sb.sb->uuid.b);

	if (opts.label) {
		size_t len = strlen(opts.label);
		if (len >= sizeof(sb.sb->label))
			die("filesystem label too long (max %zu characters)",
			    sizeof(sb.sb->label) - 1);
		memcpy(sb.sb->label, opts.label, len);
		sb.sb->label[len] = '\0';
	} else
		sb.sb->label[0] = '\0';

	bch2_sb_field_resize(&sb, ext,
			     DIV_ROUND_UP(sizeof(struct bch_sb_field_ext),
					  sizeof(u64)));
	bch2_opt_set_sb_all(sb.sb, -1, &fs_opts);

	struct timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now))
		die("error getting current time: %m");

	sb.sb->time_base_lo =
		cpu_to_le64(now.tv_sec * NSEC_PER_SEC + now.tv_nsec);
	sb.sb->time_precision = cpu_to_le32(1);

	struct bch_sb_field_members_v2 *mi = bch2_sb_field_resize(
		&sb, members_v2,
		DIV_ROUND_UP(sizeof(*mi) + sizeof(struct bch_member) * devs.nr,
			     sizeof(u64)));

	mi->member_bytes = cpu_to_le16(sizeof(struct bch_member));
	darray_for_each(devs, i)
	{
		unsigned idx = i - devs.data;
		struct bch_member *m = bch2_members_v2_get_mut(sb.sb, idx);

		uuid_generate(m->uuid.b);
		m->nbuckets = cpu_to_le64(i->nbuckets);
		m->first_bucket = 0;

		if (!opt_defined(i->opts, rotational))
			opt_set(i->opts, rotational, bdev_rot(i->bdev));

		bch2_opt_set_sb_all(sb.sb, idx, &i->opts);
		SET_BCH_MEMBER_ROTATIONAL_SET(
			bch2_members_v2_get_mut(sb.sb, idx), 1);
	}

	darray_for_each(devs, i)
	{
		unsigned idx = i - devs.data;
		darray_for_each(i->opt_strs, e)
		{
			struct bch_member *m = bch2_members_v2_get_mut(sb.sb, idx);

			switch (e->id) {
			case Opt_label: {
				int path_idx = bch2_disk_path_find_or_create(&sb, e->str);
				if (path_idx < 0)
					die("error creating disk path: %s",
					    strerror(-path_idx));
				SET_BCH_MEMBER_GROUP(m, path_idx + 1);
				break;
			}
			case Opt_failure_domain: {
				size_t len = strlen(e->str);
				if (len >= sizeof(m->failure_domain))
					die("failure domain name too long (max %zu bytes)",
					    sizeof(m->failure_domain));
				memset(m->failure_domain, 0,
				       sizeof(m->failure_domain));
				memcpy(m->failure_domain, e->str, len);
				break;
			}
			default:
				die("can't resolve option %s at format time",
				    bch2_opt_table[e->id].attr.name);
			}
		}
	}

	SET_BCH_SB_FOREGROUND_TARGET(
		sb.sb, parse_target(&sb, devs, fs_opt_strs.foreground_target));
	SET_BCH_SB_BACKGROUND_TARGET(
		sb.sb, parse_target(&sb, devs, fs_opt_strs.background_target));
	SET_BCH_SB_PROMOTE_TARGET(
		sb.sb, parse_target(&sb, devs, fs_opt_strs.promote_target));
	SET_BCH_SB_METADATA_TARGET(
		sb.sb, parse_target(&sb, devs, fs_opt_strs.metadata_target));

	if (opts.encrypted) {
		struct bch_sb_field_crypt *crypt = bch2_sb_field_resize(
			&sb, crypt, DIV_ROUND_UP(sizeof(*crypt), sizeof(u64)));
		bch_sb_crypt_init(sb.sb, crypt, opts.passphrase);
		SET_BCH_SB_ENCRYPTION_TYPE(sb.sb, 1);
	}

	bch2_sb_members_cpy_v2_v1(&sb);

	darray_for_each(devs, i)
	{
		sb.sb->dev_idx = i - devs.data;
		if (!i->sb_offset) {
			i->sb_offset = BCH_SB_SECTOR;
			i->sb_end = i->fs_size >> 9;
		}

		bch2_sb_layout_init(&sb.sb->layout, fs_opts.block_size,
				    i->opts.bucket_size, opts.superblock_size,
				    i->sb_offset, i->sb_end, opts.no_sb_at_end);

		if (i->sb_offset == BCH_SB_SECTOR) {
			static const char zeroes[BCH_SB_SECTOR << 9];
			xpwrite(i->bdev->bd_fd, zeroes, sizeof(zeroes), 0,
				"zeroing start of disk");
		}

		bch2_super_write(i->bdev->bd_fd, sb.sb);
	}

	/* udevadm trigger --settle <devices> */
	struct printbuf cmd = PRINTBUF;
	prt_printf(&cmd, "udevadm trigger --settle");
	darray_for_each(devs, i) prt_printf(&cmd, " %s", i->path);
	if (system(cmd.buf)) {
	}
	printbuf_exit(&cmd);

	return sb.sb;
}

int bch2_format_for_device_add(struct dev_opts *dev, unsigned block_size,
			       unsigned btree_node_size)
{
	struct bch_opt_strs fs_opt_strs;
	memset(&fs_opt_strs, 0, sizeof(fs_opt_strs));

	struct bch_opts fs_opts = bch2_parse_opts(fs_opt_strs);
	opt_set(fs_opts, block_size, block_size);
	opt_set(fs_opts, btree_node_size, btree_node_size);

	dev_opts_list devs = {};
	darray_push(&devs, *dev);

	struct format_opts format_opts = format_opts_default();
	struct bch_sb *sb =
		bch2_format(fs_opt_strs, fs_opts, format_opts, devs);
	darray_exit(&devs);
	free(sb);

	return 0;
}
