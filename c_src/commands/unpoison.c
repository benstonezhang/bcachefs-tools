/*
 * bcachefs unpoison: clear poison flags on file extents.
 *
 * Extents are marked as poisoned following checksum errors to ensure that
 * subsequent reads fail immediately without re-accessing potentially bad
 * data on disk. This tool clears that flag.
 *
 * WARNING: Unpoisoning data means subsequent reads will return data from
 * disk even if it remains corrupted.
 *
 * Ported from src/commands/unpoison.rs.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"

static void unpoison_usage(void)
{
	puts("bcachefs unpoison - clear poison flags on file extents\n"
	     "Usage: bcachefs unpoison [OPTION]... <file>\n"
	     "\n"
	     "Options:\n"
	     "  --offset=OFFSET           Start offset in bytes (must be sector-aligned)\n"
	     "  --len=LEN                 Length in bytes (must be sector-aligned, 0 = entire file)\n"
	     "  --yes-i-understand        Required to confirm you understand the risks\n"
	     "  -h, --help                Display this help and exit\n");
}

int cmd_unpoison(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "offset", required_argument, NULL, 'o' },
		{ "len", required_argument, NULL, 'l' },
		{ "yes-i-understand", no_argument, NULL, 'y' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	u64 offset = 0, len = 0;
	bool understand = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "o:l:yh", longopts, NULL)) != -1)
		switch (opt) {
		case 'o':
			offset = strtoull(optarg, NULL, 10);
			break;
		case 'l':
			len = strtoull(optarg, NULL, 10);
			break;
		case 'y':
			understand = true;
			break;
		case 'h':
			unpoison_usage();
			return 0;
		default:
			unpoison_usage();
			return -EINVAL;
		}
	args_shift(optind);

	char *file_path = arg_pop();
	if (!file_path) {
		unpoison_usage();
		return -EINVAL;
	}

	if (!understand) {
		fprintf(stderr,
			"WARNING: Unpoisoning makes corruption invisible.\n\n");
		fprintf(stderr,
			"Poisoned extents failed checksum verification. After data moves,\n");
		fprintf(stderr,
			"the corrupt data gets a valid checksum. Unpoisoning removes the\n");
		fprintf(stderr,
			"the only remaining indication that the data is bad.\n");
		fprintf(stderr,
			"Use `bcachefs data-read --no-poison-check` to inspect the data first.\n");
		fprintf(stderr, "\n");
		fprintf(stderr, "Pass --yes-i-understand to proceed.\n");
		return 1;
	}

	int fd = open(file_path, O_RDWR);
	if (fd < 0)
		die("error opening %s: %m", file_path);

	if (len == 0) {
		struct stat statbuf = xfstat(fd);
		len = (statbuf.st_size + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	}

	if ((offset | len) & (SECTOR_SIZE - 1))
		die("offset and len must be sector-aligned (%u bytes)",
		    SECTOR_SIZE);

	struct bch_ioctl_unpoison arg = {
		.offset = offset,
		.len = len,
		.flags = 0,
	};

	if (ioctl(fd, BCHFS_IOC_UNPOISON, &arg) < 0)
		die("ioctl BCHFS_IOC_UNPOISON error: %m");

	printf("unpoisoned %llu bytes at offset %llu\n", len, offset);

	close(fd);
	return 0;
}
