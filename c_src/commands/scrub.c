/*
 * scrub: Verify checksums and correct errors, if possible.
 *
 * Implements the scrub command which verifies data integrity across
 * filesystem devices. Uses ioctl-based progress reporting and ANSI-based
 * TUI for real-time status.
 *
 * Ported from src/commands/scrub.rs.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"

static volatile sig_atomic_t interrupted = 0;

static void sigint_handler(int sig)
{
	interrupted = 1;
}

/*
 * bch_ioctl_data_event is blocklisted from bindgen (packed+aligned conflict),
 * so we read raw bytes and extract fields manually.
 * Layout: u8 type, u8 ret, u8 pad[6], bch_ioctl_data_progress, padding to 128.
 */
#define DATA_EVENT_SIZE 128

struct scrub_dev {
	char *name;
	int progress_fd;
	u64 done;
	u64 corrected;
	u64 uncorrected;
	u64 total;
	u8 ret_status;
};

typedef DARRAY(struct scrub_dev) scrub_devs;

static int read_data_event(int fd, u8 *type, u8 *ret,
			   struct bch_ioctl_data_progress *p)
{
	char buf[DATA_EVENT_SIZE];
	ssize_t n = read(fd, buf, DATA_EVENT_SIZE);
	if (n != DATA_EVENT_SIZE)
		return -1;

	*type = buf[0];
	*ret = buf[1];
	memcpy(p, buf + 8, sizeof(*p));
	return 0;
}

static int start_scrub(int ioctl_fd, u32 dev_idx, u32 data_types)
{
	struct bch_ioctl_data cmd = {
		.op = BCH_DATA_OP_scrub,
	};
	cmd.scrub.dev = dev_idx;
	cmd.scrub.data_types = data_types;

	return ioctl(ioctl_fd, BCH_IOCTL_DATA, &cmd);
}

static void print_scrub_line(struct scrub_dev *d, u64 rate)
{
	unsigned pct = d->total ? (d->done * 100 / d->total) : 0;
	char *done_s = fmt_sectors_human(d->done);
	char *corr_s = fmt_sectors_human(d->corrected);
	char *uncorr_s = fmt_sectors_human(d->uncorrected);
	char *total_s = fmt_sectors_human(d->total);
	char *rate_s = fmt_bytes_human(rate);

	printf("%-16s %12s %12s %12s %12s %5u%%  ", d->name, done_s, corr_s,
	       uncorr_s, total_s, pct);

	if (d->progress_fd >= 0)
		printf("%s/sec\n", rate_s);
	else if (d->ret_status == BCH_IOCTL_DATA_EVENT_RET_device_offline)
		printf("offline\n");
	else
		printf("complete\n");

	free(done_s);
	free(corr_s);
	free(uncorr_s);
	free(total_s);
	free(rate_s);
}

static void scrub_usage(void)
{
	puts("bcachefs scrub - verify data checksums and correct errors\n"
	     "Usage: bcachefs scrub [OPTION]... <mountpoint>\n"
	     "\n"
	     "Options:\n"
	     "  -m, --metadata          Check metadata only\n"
	     "      --mapper-names      Show mapper names for dm-multipath devices\n"
	     "  -h, --help              Display this help and exit\n");
}

int cmd_scrub(int argc, char *argv[])
{
	enum { OPT_MAPPER_NAMES = 1000 };
	static const struct option longopts[] = {
		{ "metadata", no_argument, NULL, 'm' },
		{ "mapper-names", no_argument, NULL, OPT_MAPPER_NAMES },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	u32 data_types = ~0U;
	enum device_name_mode name_mode = DEVICE_NAME_RAW;
	int opt;

	while ((opt = getopt_long(argc, argv, "mh", longopts, NULL)) != -1)
		switch (opt) {
		case 'm':
			data_types = 1 << BCH_DATA_btree;
			break;
		case OPT_MAPPER_NAMES:
			name_mode = DEVICE_NAME_MAPPER;
			break;
		case 'h':
			scrub_usage();
			return 0;
		default:
			scrub_usage();
			return -EINVAL;
		}
	args_shift(optind);

	char *fs_path = arg_pop();
	if (!fs_path) {
		scrub_usage();
		return -EINVAL;
	}

	signal(SIGINT, sigint_handler);

	struct bchfs_handle fs = bcache_fs_open(fs_path);
	dev_names devs = bchu_fs_get_devices_mode(fs, name_mode);
	scrub_devs sdevs = {};

	int dev_idx = fs.dev_idx;

	if (dev_idx >= 0) {
		struct dev_name *d = dev_idx_to_name(&devs, dev_idx);
		int fd = start_scrub(fs.ioctl_fd, dev_idx, data_types);
		if (fd < 0)
			die("error starting scrub: %m");

		struct scrub_dev sd = {
			.name = strdup(d ? d->dev : "unknown"),
			.progress_fd = fd,
		};
		darray_push(&sdevs, sd);
	} else {
		darray_for_each(devs, d)
		{
			int fd = start_scrub(fs.ioctl_fd, d->idx, data_types);
			if (fd < 0)
				continue;

			struct scrub_dev sd = {
				.name = strdup(d->dev),
				.progress_fd = fd,
			};
			darray_push(&sdevs, sd);
		}
	}

	if (!sdevs.nr)
		die("no devices to scrub");

	printf("Starting scrub on %zu devices\n", sdevs.nr);
	printf("%-16s %12s %12s %12s %12s %6s\n", "device", "checked",
	       "corrected", "uncorrected", "total", "");

	int exit_code = 0;
	struct timespec last_time, now_time;
	clock_gettime(CLOCK_MONOTONIC, &last_time);
	bool first = true;

	while (true) {
		clock_gettime(CLOCK_MONOTONIC, &now_time);
		u64 ns_elapsed =
			first ? 0 :
				      (now_time.tv_sec - last_time.tv_sec) *
						1000000000ULL +
					(now_time.tv_nsec - last_time.tv_nsec);

		bool all_done = true;

		bool live_output = isatty(STDOUT_FILENO);

		if (live_output && !first) {
			for (size_t i = 0; i < sdevs.nr; i++) {
				if (i > 0)
					printf("\x1b[1A");
				printf("\x1b[2K\r");
			}
		}

		darray_for_each(sdevs, d)
		{
			u64 rate = 0;

			if (d->progress_fd >= 0) {
				u8 type, ret;
				struct bch_ioctl_data_progress p;
				if (read_data_event(d->progress_fd, &type, &ret,
						    &p) == 0) {
				if (type == 0) {
					if (ns_elapsed > 0) {
						/*
						 * Use checked arithmetic to
						 * match Rust's wrapping_sub
						 * + checked_shl +
						 * saturating_mul
						 */
						u64 delta = p.sectors_done -
							    d->done;

						if (delta <= U64_MAX >> 9)
							rate = delta << 9;
						else
							rate = U64_MAX;

						if (rate <=
						    U64_MAX / 1000000000ULL)
							rate = rate *
							       1000000000ULL /
							       ns_elapsed;
						else
							rate = U64_MAX /
							       ns_elapsed;
					}

						d->done = p.sectors_done;
						d->corrected =
							p.sectors_error_corrected;
						d->uncorrected =
							p.sectors_error_uncorrected;
						d->total = p.sectors_total;

						if (d->corrected)
							exit_code |= 2;
						if (d->uncorrected)
							exit_code |= 4;

						if (ret) {
							d->ret_status = ret;
							close(d->progress_fd);
							d->progress_fd = -1;
						}
					}
				} else {
					close(d->progress_fd);
					d->progress_fd = -1;
				}
			}

			print_scrub_line(d, rate);
			if (d->progress_fd >= 0)
				all_done = false;
		}

		if (all_done)
			break;

		if (interrupted) {
			printf("\nInterrupted\n");
			exit_code |= 1;
			break;
		}

		last_time = now_time;
		first = false;
		usleep(1000000);
	}

	darray_for_each(sdevs, d)
	{
		if (d->progress_fd >= 0)
			close(d->progress_fd);
		free(d->name);
	}
	darray_exit(&sdevs);
	darray_exit(&devs);
	bcache_fs_close(fs);

	return exit_code;
}
