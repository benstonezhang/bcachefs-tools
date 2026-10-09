/*
 * bcachefs top: Show live performance counters.
 *
 * Implements a multi-page TUI (Counters and Devices) for real-time
 * monitoring of filesystem operations and device I/O. Supports both
 * interactive and non-interactive (batch) modes.
 *
 * Ported from src/commands/top.rs.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h>

#include "libbcachefs.h"
#include "sb/counters.h"
#include "cmds.h"

enum page_type { PAGE_COUNTERS, PAGE_DEVICES, PAGE_NR };

static const char *page_labels[] = { "counters", "devices" };

/*
 * Per-device IO from sysfs:
 * io_done is JSON: {"read": {...}, "write": {...}}, values in bytes.
 */
struct dev_io_entry {
	char *label; /* "dev/data_type" */
	u64 read_bytes;
	u64 write_bytes;
};

typedef DARRAY(struct dev_io_entry) dev_io_entries;

struct top_state {
	struct bchfs_handle fs;
	u16 nr_stable;
	struct bch_ioctl_query_counters *mount_vals;
	struct bch_ioctl_query_counters *start_vals;
	struct bch_ioctl_query_counters *prev_vals;
	struct bch_ioctl_query_counters *curr_vals;

	dev_io_entries prev_dev_io;
	dev_io_entries curr_dev_io;

	bool human_readable;
	enum device_name_mode name_mode;
	char *sysfs_path;
	unsigned interval_secs;
	enum page_type page;
	int cursor;
	int scroll_offset;
};

static const u16 counters_to_stable_map[] = {
#define x(n, id, ...) [BCH_COUNTER_##n] = BCH_COUNTER_STABLE_##n,
	BCH_PERSISTENT_COUNTERS()
#undef x
};

static struct bch_ioctl_query_counters *read_counters(struct bchfs_handle fs,
						      u16 flags)
{
	struct bch_ioctl_query_counters *ret = kzalloc(
		sizeof(*ret) + sizeof(ret->d[0]) * BCH_COUNTER_NR, GFP_KERNEL);
	ret->nr = BCH_COUNTER_NR;
	ret->flags = flags;
	if (ioctl(fs.ioctl_fd, BCH_IOCTL_QUERY_COUNTERS, ret))
		die("BCH_IOCTL_QUERY_COUNTERS error: %m");
	return ret;
}

static u64 json_get_val(const char *json, const char *key)
{
	const char *p = strstr(json, key);
	if (!p)
		return 0;
	p = strchr(p, ':');
	if (!p)
		return 0;
	p++;
	while (*p && !isdigit(*p))
		p++;
	return strtoull(p, NULL, 10);
}

static void read_device_io(const char *sysfs_path, enum device_name_mode mode,
			  dev_io_entries *entries)
{
	DIR *dir = opendir(sysfs_path);
	if (!dir)
		return;

	struct dirent *d;
	while ((d = readdir(dir))) {
		if (strncmp(d->d_name, "dev-", 4))
			continue;

		char *dev_sysfs = mprintf("%s/%s", sysfs_path, d->d_name);
		char *dev_name = dev_display_name_from_sysfs(dev_sysfs, mode);
		free(dev_sysfs);

		char *io_done_path =
			mprintf("%s/%s/io_done", sysfs_path, d->d_name);
		char *json = read_file_str(AT_FDCWD, io_done_path);
		free(io_done_path);

		if (json) {
			/*
			 * io_done is: {"read": {"total": N, "user": M, ...}, "write": {...}}
			 * We iterate through known data types.
			 */
			for (unsigned i = 0; i < BCH_DATA_NR; i++) {
				const char *dtype = __bch2_data_types[i];
				u64 r = 0, w = 0;

				char *read_part = strstr(json, "\"read\"");
				if (read_part)
					r = json_get_val(read_part, dtype);

				char *write_part = strstr(json, "\"write\"");
				if (write_part)
					w = json_get_val(write_part, dtype);

				if (r || w) {
					struct dev_io_entry e = {
						.label = mprintf("%s/%s", dev_name, dtype),
						.read_bytes = r,
						.write_bytes = w,
					};
					darray_push(entries, e);
				}
			}
			free(json);
		}
		free(dev_name);
	}
	closedir(dir);
}

static char *fmt_counter(u64 val, unsigned i, bool human_readable)
{
	if (bch2_counter_flags_map[i] == TYPE_SECTORS)
		return human_readable ? fmt_sectors_human(val) :
					      mprintf("%llu", val << 9);
	if (human_readable && val >= 10000)
		return fmt_num_human(val);
	return mprintf("%llu", val);
}

/*
 * Print one frame: counters page (rate / total / mount) or devices
 * page (read/s / read / write/s / write).
 */
static void print_frame(struct top_state *s, bool is_tty)
{
	int term_h = 40;
	if (is_tty) {
		printf("\033[H\033[J");
		struct winsize ws;
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
			term_h = ws.ws_row;
	}

	printf("All counters have a corresponding tracepoint; for more info try perf trace\n");
	printf("q:quit  h:human-readable  Tab:page  1-9:interval\n\n");

	/* Page tabs */
	for (int i = 0; i < PAGE_NR; i++) {
		if (i == s->page)
			printf("\033[7m[%s]\033[0m  ", page_labels[i]);
		else
			printf("[%s]  ", page_labels[i]);
	}
	printf("\n\n");

	if (s->page == PAGE_COUNTERS) {
		printf("%-40s %14s %14s %14s\n", "NAME", "rate/s", "total",
		       "mount");
		for (unsigned i = 0; i < BCH_COUNTER_NR; i++) {
			unsigned stable = counters_to_stable_map[i];
			u64 cv = s->curr_vals->d[stable];
			u64 pv = s->prev_vals->d[stable];
			u64 sv = s->start_vals->d[stable];
			u64 mv = s->mount_vals->d[stable];

			if (cv == mv)
				continue;

			char *rate_s = fmt_counter((cv - pv) / s->interval_secs,
						   i, s->human_readable);
			char *total_s = fmt_counter(cv - sv, i, s->human_readable);
			char *mount_s =
				fmt_counter(cv - mv, i, s->human_readable);

			printf("%-40s %12s/s %14s %14s\n",
			       bch2_counter_names[i], rate_s, total_s, mount_s);
			free(rate_s);
			free(total_s);
			free(mount_s);
		}
	} else {
		printf("%-40s %14s %14s %14s %14s\n", "DEVICE", "read/s",
		       "read", "write/s", "write");
		darray_for_each(s->curr_dev_io, e)
		{
			u64 prev_r = e->read_bytes, prev_w = e->write_bytes;
			darray_for_each(s->prev_dev_io, pe)
			{
				if (!strcmp(pe->label, e->label)) {
					prev_r = pe->read_bytes;
					prev_w = pe->write_bytes;
					break;
				}
			}
			char *rs = fmt_bytes_human((e->read_bytes - prev_r) /
						   s->interval_secs);
			char *r = fmt_bytes_human(e->read_bytes);
			char *ws = fmt_bytes_human((e->write_bytes - prev_w) /
						   s->interval_secs);
			char *w = fmt_bytes_human(e->write_bytes);
			printf("%-40s %12s/s %14s %12s/s %14s\n", e->label, rs,
			       r, ws, w);
			free(rs);
			free(r);
			free(ws);
			free(w);
		}
	}
}

int cmd_fs_top(int argc, char *argv[])
{
	enum { OPT_MAPPER_NAMES = 1000 };
	static const struct option longopts[] = {
		{ "human-readable", no_argument, NULL, 'h' },
		{ "once", no_argument, NULL, '1' },
		{ "count", required_argument, NULL, 'n' },
		{ "delay", required_argument, NULL, 'd' },
		{ "mapper-names", no_argument, NULL, OPT_MAPPER_NAMES },
		{ "help", no_argument, NULL, 'H' },
		{ NULL }
	};
	struct top_state s = {
		.human_readable = false,
		.name_mode = DEVICE_NAME_RAW,
		.interval_secs = 1,
		.page = PAGE_COUNTERS,
	};
	bool once = false;
	int count = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "hn:d:H", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			s.human_readable = true;
			break;
		case '1':
			once = true;
			break;
		case 'n':
			count = atoi(optarg);
			break;
		case 'd':
			s.interval_secs = atoi(optarg);
			if (s.interval_secs < 1)
				s.interval_secs = 1;
			break;
		case OPT_MAPPER_NAMES:
			s.name_mode = DEVICE_NAME_MAPPER;
			break;
		case 'H':
			puts("Usage: bcachefs fs top [OPTION]... <mountpoint>\n"
			     "  -h, --human-readable\n"
			     "      --mapper-names   Show mapper names for dm-multipath\n"
			     "  -1, --once\n"
			     "  -n, --count=N\n"
			     "  -d, --delay=SECS");
			exit(0);
		}
	args_shift(optind);

	char *path = arg_pop() ?: ".";
	s.fs = bcache_fs_open(path);
	char uuid_str[40];
	uuid_unparse(s.fs.uuid.b, uuid_str);
	s.sysfs_path = mprintf(SYSFS_BASE "%s", uuid_str);

	s.mount_vals = read_counters(s.fs, BCH_IOCTL_QUERY_COUNTERS_MOUNT);
	s.start_vals = read_counters(s.fs, 0);
	s.curr_vals = read_counters(s.fs, 0);
	s.prev_vals = read_counters(s.fs, 0);

	if (once)
		count = 1;
	else if (count == 0 && !isatty(STDOUT_FILENO))
		count = 1;

	bool is_tty = !once && count == 0;
	struct termios orig;
	if (is_tty) {
		tcgetattr(STDIN_FILENO, &orig);
		struct termios raw = orig;
		raw.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
		printf("\033[?1049h");
	}

	int frames = 0;
	while (true) {
		free(s.prev_vals);
		s.prev_vals = s.curr_vals;
		s.curr_vals = read_counters(s.fs, 0);

		darray_for_each(s.prev_dev_io, e) free(e->label);
		darray_exit(&s.prev_dev_io);
		s.prev_dev_io = s.curr_dev_io;
		memset(&s.curr_dev_io, 0, sizeof(s.curr_dev_io));
		read_device_io(s.sysfs_path, s.name_mode, &s.curr_dev_io);

		print_frame(&s, is_tty);

		if (once || (count > 0 && ++frames >= count))
			break;

		if (is_tty) {
			struct pollfd fds = { .fd = STDIN_FILENO,
					      .events = POLLIN };
			if (poll(&fds, 1, s.interval_secs * 1000) > 0) {
				char c;
				if (read(STDIN_FILENO, &c, 1) > 0) {
					if (c == 'q')
						break;
					if (c == 'h')
						s.human_readable =
							!s.human_readable;
					if (c == '\t')
						s.page = (s.page + 1) % PAGE_NR;
					if (isdigit(c) && c > '0')
						s.interval_secs = c - '0';
				}
			}
		} else {
			sleep(s.interval_secs);
		}
	}

	if (is_tty) {
		printf("\033[?1049l");
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
	}

	free(s.mount_vals);
	free(s.start_vals);
	free(s.curr_vals);
	darray_for_each(s.curr_dev_io, e) free(e->label);
	darray_exit(&s.curr_dev_io);
	free(s.sysfs_path);
	bcache_fs_close(s.fs);
	return 0;
}
