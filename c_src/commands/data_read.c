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

	/*
	 * Per-call read size, matching Rust's CHUNK: the ioctl reports how
	 * much it read through an int return, so a single call cannot ask
	 * for (or honestly report) more than INT_MAX - and chunking keeps
	 * the buffer allocation bounded too.
	 */
	const u64 chunk = 64ULL << 20;
	u64 read_len = min_t(u64, len, chunk);

	void *buf = xaligned_alloc(SECTOR_SIZE, read_len);
	/* Rust allocates zeroed; error paths must not leak stale memory */
	memset(buf, 0, read_len);

	char err_msg_buf[4096];
	u64 pos = 0;
	u32 errors = 0;
	bool failed = false;

	/* Opened before reading, like Rust's File::create: a bad output
	 * path fails up front (and truncates, as it always did). */
	int out_fd = -1;
	if (output_path) {
		out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (out_fd < 0)
			die("error opening output file %s: %m", output_path);
	}

	while (pos < len) {
		u64 want = min_t(u64, len - pos, chunk);

		/* A message left over from the previous chunk would be read
		 * back as this chunk's. */
		memset(err_msg_buf, 0, sizeof(err_msg_buf));

		struct bch_ioctl_pread_raw arg = {
			.offset = offset + pos,
			.len = want,
			.buf = (unsigned long) buf,
			.flags = no_poison_check ?
				BCH_PREAD_RAW_no_poison_check : 0,
			.err = {
				.msg_ptr = (unsigned long) err_msg_buf,
				.msg_len = sizeof(err_msg_buf),
			},
		};

		int ret = ioctl(fd, BCHFS_IOC_PREAD_RAW, &arg);
		errors |= arg.errors;

		if (err_msg_buf[0])
			fprintf(stderr, "kernel: %s\n", err_msg_buf);

		u64 got;
		if (ret < 0) {
			fprintf(stderr, "read failed at offset %llu: %s\n",
				offset + pos, strerror(errno));
			if (pos)
				fprintf(stderr,
					"  %llu bytes before it were read; resume past the bad range with --offset\n",
					pos);
			failed = true;
			break;
		}
		got = ret;

		if (output_path) {
			if (write(out_fd, buf, got) != (ssize_t)got)
				die("error writing to output file: %m");
		} else {
			unsigned char *p = buf;
			for (u64 i = 0; i < got; i += width) {
				printf("%08llx  ", offset + pos + i);
				for (unsigned j = 0; j < width; j++) {
					if (i + j < got)
						printf("%02x ", p[i + j]);
					else
						printf("   ");
					if (j == 7)
						printf(" ");
				}
				printf(" |");
				for (unsigned j = 0; j < width && i + j < got;
				     j++) {
					unsigned char c = p[i + j];
					printf("%c",
					       (c >= 0x20 && c < 0x7f) ? c :
									 '.');
				}
				printf("|\n");
			}
		}

		pos += got;

		/* Short of what was asked for, without an error: end of
		 * file. */
		if (got < want)
			break;
	}

	if (errors) {
		bool first = true;

		fprintf(stderr, "errors:");
		if (errors & BCH_PREAD_RAW_ERR_checksum) {
			fprintf(stderr, "%s checksum", first ? "" : ",");
			first = false;
		}
		if (errors & BCH_PREAD_RAW_ERR_io) {
			fprintf(stderr, "%s io", first ? "" : ",");
			first = false;
		}
		if (errors & BCH_PREAD_RAW_ERR_decompression) {
			fprintf(stderr, "%s decompression", first ? "" : ",");
			first = false;
		}
		if (errors & BCH_PREAD_RAW_ERR_ec_reconstruct) {
			fprintf(stderr, "%s ec_reconstruct", first ? "" : ",");
			first = false;
		}
		fprintf(stderr, "\n");
	}

	if (!failed && pos < len)
		fprintf(stderr,
			"short read: %llu of %llu bytes requested (end of file?)\n",
			pos, len);

	if (output_path) {
		close(out_fd);
		printf("wrote %llu bytes to %s\n", pos, output_path);
	}

	free(buf);
	close(fd);
	return failed ? 1 : 0;
}
