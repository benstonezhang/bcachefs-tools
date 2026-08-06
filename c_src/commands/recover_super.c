/*
 * recover-super: Attempt to recover an overwritten superblock from backups.
 *
 * Scans the device for backup superblocks and picks the one with the
 * most recent mount time. Can also recover by copying the superblock
 * from another member device in a multi-device filesystem.
 *
 * Ported from src/commands/recover_super.rs.
 *
 * GPLv2
 */

#include <ctype.h>
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
#include "sb/io.h"
#include "sb/members.h"
#include "cmds.h"

typedef DARRAY(struct bch_sb *) probed_sb_list;

struct recover_super_args {
	u64 dev_size;
	u64 offset;
	u64 scan_len;

	const char *src_device;
	int dev_idx;

	bool yes;
	bool verbose;

	const char *dev_path;
};

static u64 bch2_sb_last_mount_time(struct bch_sb *sb)
{
	u64 ret = 0;
	for (unsigned i = 0; i < sb->nr_devices; i++) {
		struct bch_member m = bch2_sb_member_get(sb, i);
		ret = max(ret, le64_to_cpu(m.last_mount));
	}
	return ret;
}

static int bch2_sb_time_cmp(struct bch_sb *l, struct bch_sb *r)
{
	return cmp_int(bch2_sb_last_mount_time(l), bch2_sb_last_mount_time(r));
}

static int validate_sb(struct bch_sb *sb, u64 offset_sectors,
		       struct printbuf *err)
{
	struct bch_opts opts = bch2_opts_empty();
	return bch2_sb_validate(sb, &opts, offset_sectors, 0, err);
}

static void probe_one_super(int dev_fd, unsigned sb_size, u64 offset,
			    probed_sb_list *sbs, bool verbose)
{
	void *sb_buf = malloc(sb_size);
	if (!sb_buf)
		die("malloc error");

	if (pread(dev_fd, sb_buf, sb_size, offset) != (ssize_t)sb_size) {
		free(sb_buf);
		return;
	}

	struct printbuf err = PRINTBUF;
	int ret = validate_sb(sb_buf, offset >> 9, &err);
	printbuf_exit(&err);

	if (!ret) {
		if (verbose) {
			struct printbuf buf = PRINTBUF;
			prt_human_readable_u64(&buf, offset);
			printf("found superblock at %s\n", buf.buf);
			printbuf_exit(&buf);
		}

		darray_push(sbs, sb_buf);
	} else {
		free(sb_buf);
	}
}

static void probe_sb_range(int dev_fd, u64 start_offset, u64 end_offset,
			   probed_sb_list *sbs, bool verbose)
{
	start_offset &= ~((u64)511);
	end_offset &= ~((u64)511);

	size_t buflen = end_offset - start_offset;
	void *buf = malloc(buflen);
	if (!buf)
		die("malloc error");

	if (pread(dev_fd, buf, buflen, start_offset) != (ssize_t)buflen) {
		free(buf);
		return;
	}

	for (u64 offset = 0; offset < buflen; offset += 512) {
		struct bch_sb *sb = buf + offset;

		if (!uuid_equal(&sb->magic, &BCACHE_MAGIC) &&
		    !uuid_equal(&sb->magic, &BCHFS_MAGIC))
			continue;

		size_t bytes = vstruct_bytes(sb);
		if (offset + bytes > buflen) {
			fprintf(stderr,
				"found sb %llu size %zu that overran buffer\n",
				start_offset + offset, bytes);
			continue;
		}

		struct printbuf err = PRINTBUF;
		int ret = validate_sb(sb, (start_offset + offset) >> 9, &err);
		if (ret)
			fprintf(stderr,
				"found sb %llu that failed to validate: %s\n",
				start_offset + offset, err.buf);
		printbuf_exit(&err);

		if (ret)
			continue;

		if (verbose) {
			struct printbuf buf = PRINTBUF;
			prt_human_readable_u64(&buf, start_offset + offset);
			printf("found superblock at %s\n", buf.buf);
			printbuf_exit(&buf);
		}

		void *sb_copy = malloc(bytes);
		if (!sb_copy)
			die("malloc error");
		memcpy(sb_copy, sb, bytes);
		darray_push(sbs, sb_copy);
	}

	free(buf);
}

static struct bch_sb *recover_super_from_scan(struct recover_super_args args,
					      int dev_fd)
{
	probed_sb_list sbs = {};

	if (args.offset) {
		probe_one_super(dev_fd, SUPERBLOCK_SIZE_DEFAULT << 9,
				args.offset, &sbs, args.verbose);
	} else {
		probe_sb_range(dev_fd, 4096, args.scan_len, &sbs, args.verbose);
		probe_sb_range(dev_fd, args.dev_size - args.scan_len,
			       args.dev_size, &sbs, args.verbose);
	}

	if (!sbs.nr) {
		fprintf(stderr, "Found no bcachefs superblocks\n");
		exit(EXIT_FAILURE);
	}

	struct bch_sb *best = NULL;
	darray_for_each(sbs, sb) if (!best || bch2_sb_time_cmp(best, *sb) < 0)
		best = *sb;

	darray_for_each(sbs, sb) if (*sb != best) free(*sb);
	darray_exit(&sbs);
	return best;
}

static struct bch_sb *recover_super_from_member(struct recover_super_args args)
{
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, noexcl, true);
	opt_set(opts, nochanges, true);

	struct bch_sb_handle src_sb;
	int ret = bch2_read_super(args.src_device, &opts, &src_sb);
	if (ret)
		die("Error opening %s: %s", args.src_device, bch2_err_str(ret));

	if (!bch2_member_exists(src_sb.sb, args.dev_idx))
		die("Member %u does not exist in source superblock",
		    args.dev_idx);

	bch2_sb_field_delete(&src_sb, BCH_SB_FIELD_journal);
	bch2_sb_field_delete(&src_sb, BCH_SB_FIELD_journal_v2);
	src_sb.sb->dev_idx = args.dev_idx;

	struct bch_sb *sb = src_sb.sb;
	src_sb.sb = NULL;

	bch2_free_super(&src_sb);

	struct bch_member m = bch2_sb_member_get(sb, args.dev_idx);

	bch2_sb_layout_init(&sb->layout, le16_to_cpu(sb->block_size) << 9,
			    BCH_MEMBER_BUCKET_SIZE(&m) << 9,
			    1U << sb->layout.sb_max_size_bits, BCH_SB_SECTOR,
			    args.dev_size >> 9, false);

	return sb;
}

static void recover_super_usage(void)
{
	puts("bcachefs recover-super - attempt to recover an overwritten superblock from backups\n"
	     "Usage: bcachefs recover-super [OPTION]... <device>\n"
	     "\n"
	     "Options:\n"
	     "  -d, --dev_size size         Size of filesystem on device, in bytes\n"
	     "  -o, --offset offset         Offset to probe, in bytes (must be multiple of 512)\n"
	     "  -l, --scan_len len          Length in bytes to scan from start and end of device\n"
	     "                              Default: 16M\n"
	     "  -s, --src_device dev        Member device to recover from, in a multi-device fs\n"
	     "  -i, --dev_idx idx           Index of this device, if recovering from another device\n"
	     "  -y, --yes                   Recover without prompting\n"
	     "  -v, --verbose               Increase logging level\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_recover_super(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "dev_size", required_argument, NULL, 'd' },
		{ "offset", required_argument, NULL, 'o' },
		{ "scan_len", required_argument, NULL, 'l' },
		{ "src_device", required_argument, NULL, 's' },
		{ "dev_idx", required_argument, NULL, 'i' },
		{ "yes", no_argument, NULL, 'y' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct recover_super_args args = {
		.scan_len = 16 << 20,
		.dev_idx = -1,
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "d:o:l:s:i:yvh", longopts,
				  NULL)) != -1)
		switch (opt) {
		case 'd':
			if (bch2_strtoull_h(optarg, &args.dev_size))
				die("invalid dev_size %s", optarg);
			break;
		case 'o':
			if (bch2_strtoull_h(optarg, &args.offset))
				die("invalid offset %s", optarg);
			if (args.offset & 511)
				die("offset must be a multiple of 512");
			break;
		case 'l':
			if (bch2_strtoull_h(optarg, &args.scan_len))
				die("invalid scan_len %s", optarg);
			break;
		case 's':
			args.src_device = optarg;
			break;
		case 'i':
			if (kstrtoint(optarg, 10, &args.dev_idx) ||
			    args.dev_idx < 0)
				die("invalid dev_idx %s", optarg);
			break;
		case 'y':
			args.yes = true;
			break;
		case 'v':
			args.verbose = true;
			break;
		case 'h':
			recover_super_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (args.src_device && args.dev_idx == -1)
		die("--src_device requires --dev_idx");
	if (args.dev_idx >= 0 && !args.src_device)
		die("--dev_idx requires --src_device");

	if (!argc)
		die("please supply a device");
	if (argc > 1)
		die("too many arguments");

	const char *dev_path = argv[0];
	int dev_fd = xopen(dev_path, O_RDWR);

	if (!args.dev_size)
		args.dev_size = get_size(dev_fd);

	struct bch_sb *sb = !args.src_device ?
				    recover_super_from_scan(args, dev_fd) :
					  recover_super_from_member(args);

	struct printbuf buf = PRINTBUF;
	bch2_sb_to_text(&buf, NULL, sb, true, 1u << BCH_SB_FIELD_members_v2);
	printf("Found superblock:\n%s\n", buf.buf);

	bool do_recover = args.yes;
	if (!do_recover) {
		printf("Recover? ");
		do_recover = ask_yn();
	}

	if (do_recover) {
		bch2_super_write(dev_fd, sb);

		/* Run udevadm trigger like Rust version */
		char *cmd = mprintf("udevadm trigger --settle %s", dev_path);
		if (system(cmd)) {
			/* ignore error */
		}
		free(cmd);
	}

	if (args.src_device)
		printf("Recovered device will no longer have a journal, please run fsck\n");

	printbuf_exit(&buf);
	free(sb);
	xclose(dev_fd);
	return 0;
}
