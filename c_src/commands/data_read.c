/*
 * bcachefs data-read: read file data with extended error reporting.
 *
 * Performs O_DIRECT reads with detailed error reporting. Identifies checksum,
 * IO, decompression, and Erasure Coding (EC) errors via a bitmask and kernel
 * log messages. Using --no-promote ensures the read is satisfied directly
 * from the target device without triggering background promotion I/O.
 *
 * Ported from src/commands/data_read.rs.
 */

#include <ctype.h>
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

static void data_read_usage(void)
{
	puts("bcachefs data-read - read file data with extended error reporting\n"
	     "Usage: bcachefs data-read [OPTION]... <file>\n"
	     "\n"
	     "Options:\n"
	     "  --offset=OFFSET           Offset in bytes (must be sector-aligned)\n"
	     "  --len=LEN                 Length in bytes (must be sector-aligned)\n"
	     "  --output=PATH             Write raw data to this file instead of hex dump\n"
	     "  --no-poison-check         Bypass poison checks (read even from poisoned extents)\n"
	     "  --width=WIDTH             Hex dump width (default 16)\n"
	     "  -h, --help                Display this help and exit\n");
}

int cmd_data_read(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "offset", required_argument, NULL, 'o' },
		{ "len", required_argument, NULL, 'l' },
		{ "output", required_argument, NULL, 'O' },
		{ "no-poison-check", no_argument, NULL, 'p' },
		{ "width", required_argument, NULL, 'w' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	u64 offset = 0, len = 4096;
	char *output_path = NULL;
	bool no_poison_check = false;
	unsigned width = 16;
	int opt;

	while ((opt = getopt_long(argc, argv, "o:l:O:pw:h", longopts, NULL)) !=
	       -1)
		switch (opt) {
		case 'o':
			offset = strtoull(optarg, NULL, 10);
			break;
		case 'l':
			len = strtoull(optarg, NULL, 10);
			break;
		case 'O':
			output_path = optarg;
			break;
		case 'p':
			no_poison_check = true;
			break;
		case 'w':
			width = atoi(optarg);
			break;
		case 'h':
			data_read_usage();
			return 0;
		default:
			data_read_usage();
			return -EINVAL;
		}
	args_shift(optind);

	char *file_path = arg_pop();
	if (!file_path) {
		data_read_usage();
		return -EINVAL;
	}

	if ((offset | len) & (SECTOR_SIZE - 1))
		die("offset and len must be sector-aligned (%u bytes)",
		    SECTOR_SIZE);

	int fd = open(file_path, O_RDONLY | O_DIRECT);
	if (fd < 0)
		die("error opening %s: %m", file_path);

	void *buf = xaligned_alloc(SECTOR_SIZE, len);

	char err_msg_buf[4096] = { 0 };
	struct bch_ioctl_pread_raw arg = {
		.offset = offset,
		.len = len,
		.buf = (unsigned long) buf,
		.flags = no_poison_check ? BCH_PREAD_RAW_no_poison_check : 0,
		.err = {
			.msg_ptr = (unsigned long) err_msg_buf,
			.msg_len = sizeof(err_msg_buf),
		},
	};

	int ret = ioctl(fd, BCHFS_IOC_PREAD_RAW, &arg);

	if (arg.errors) {
		fprintf(stderr, "errors:");
		if (arg.errors & BCH_PREAD_RAW_ERR_checksum)
			fprintf(stderr, " checksum");
		if (arg.errors & BCH_PREAD_RAW_ERR_io)
			fprintf(stderr, " io");
		if (arg.errors & BCH_PREAD_RAW_ERR_decompression)
			fprintf(stderr, " decompression");
		if (arg.errors & BCH_PREAD_RAW_ERR_ec_reconstruct)
			fprintf(stderr, " ec_reconstruct");
		fprintf(stderr, "\n");
	}

	if (err_msg_buf[0])
		fprintf(stderr, "kernel: %s\n", err_msg_buf);

	if (ret < 0)
		fprintf(stderr, "ioctl BCHFS_IOC_PREAD_RAW error: %m\n");

	if (output_path) {
		int out_fd =
			open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (out_fd < 0)
			die("error opening output file %s: %m", output_path);
		if (write(out_fd, buf, len) != (ssize_t)len)
			die("error writing to output file: %m");
		close(out_fd);
		printf("wrote %llu bytes to %s\n", len, output_path);
	} else {
		unsigned char *p = buf;
		for (u64 i = 0; i < len; i += width) {
			printf("%08llx  ", offset + i);
			for (unsigned j = 0; j < width; j++) {
				if (i + j < len)
					printf("%02x ", p[i + j]);
				else
					printf("   ");
				if (j == 7)
					printf(" ");
			}
			printf(" |");
			for (unsigned j = 0; j < width && i + j < len; j++) {
				unsigned char c = p[i + j];
				printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
			}
			printf("|\n");
		}
	}

	free(buf);
	close(fd);
	return ret < 0 ? 1 : 0;
}
