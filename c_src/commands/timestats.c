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
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"

struct duration_stats {
	u64 min, max, total, mean, stddev;
};

struct ewma_stats {
	u64 mean, stddev;
};

struct time_stats {
	u64 count;
	struct duration_stats duration_ns;
	struct ewma_stats duration_ewma_ns;
};

struct stat_entry {
	char *name;
	struct time_stats stats;
};

typedef DARRAY(struct stat_entry) stat_entries;

static u64 json_get_u64(const char *json, const char *key)
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

static void parse_time_stats(const char *json, struct time_stats *s)
{
	s->count = json_get_u64(json, "\"count\"");

	const char *duration = strstr(json, "\"duration_ns\"");
	if (duration) {
		s->duration_ns.min = json_get_u64(duration, "\"min\"");
		s->duration_ns.max = json_get_u64(duration, "\"max\"");
		s->duration_ns.total = json_get_u64(duration, "\"total\"");
		s->duration_ns.mean = json_get_u64(duration, "\"mean\"");
		s->duration_ns.stddev = json_get_u64(duration, "\"stddev\"");
	}

	const char *ewma = strstr(json, "\"duration_ewma_ns\"");
	if (ewma) {
		s->duration_ewma_ns.mean = json_get_u64(ewma, "\"mean\"");
		s->duration_ewma_ns.stddev = json_get_u64(ewma, "\"stddev\"");
	}
}

static char *fmt_duration_adaptive(u64 ns, u64 max_ns)
{
	if (ns == 0)
		return strdup("-");

	/* Auto-select unit based on the maximum value in the set to keep columns aligned */
	if (max_ns > 1000000000ULL)
		return mprintf("%.2f s ", (double)ns / 1000000000.0);
	if (max_ns > 1000000ULL)
		return mprintf("%.2f ms", (double)ns / 1000000.0);
	if (max_ns > 1000ULL)
		return mprintf("%.2f us", (double)ns / 1000.0);
	return mprintf("%llu ns", ns);
}

static int stat_entry_cmp(const void *_a, const void *_b)
{
	const struct stat_entry *a = _a;
	const struct stat_entry *b = _b;
	return strcmp(a->name, b->name);
}

static void collect_time_stats(const char *sysfs_path, stat_entries *entries)
{
	char *json_dir_path = mprintf("%s/time_stats_json", sysfs_path);
	DIR *dir = opendir(json_dir_path);
	if (!dir) {
		free(json_dir_path);
		return;
	}

	struct dirent *d;
	while ((d = readdir(dir))) {
		if (d->d_name[0] == '.')
			continue;

		char *file_path = mprintf("%s/%s", json_dir_path, d->d_name);
		char *json = read_file_str(AT_FDCWD, file_path);

		if (json) {
			struct stat_entry e = { .name = strdup(d->d_name) };
			parse_time_stats(json, &e.stats);
			darray_push(entries, e);
			free(json);
		} else {
			fprintf(stderr, "warning: failed to parse %s\n", file_path);
		}

		free(file_path);
	}

	closedir(dir);
	free(json_dir_path);
}

static void collect_device_latency_stats(const char *sysfs_path,
					 enum device_name_mode mode,
					 stat_entries *entries)
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
		const char *suffixes[] = { "io_latency_stats_read_json",
					   "io_latency_stats_write_json" };
		const char *labels[] = { "read", "write" };

		for (int i = 0; i < 2; i++) {
			char *file_path = mprintf("%s/%s/%s", sysfs_path,
						  d->d_name, suffixes[i]);
			char *json = read_file_str(AT_FDCWD, file_path);

			if (json) {
				struct stat_entry e = {
					.name = mprintf("%s/%s", dev_name, labels[i])
				};
				parse_time_stats(json, &e.stats);
				darray_push(entries, e);
				free(json);
			} else {
				fprintf(stderr, "warning: failed to parse %s\n", file_path);
			}

			free(file_path);
		}
		free(dev_name);
	}
	closedir(dir);
}

static void timestats_usage(void)
{
	puts("bcachefs timestats - display operation latency statistics\n"
	     "Usage: bcachefs timestats [OPTION]... [mountpoint]\n"
	     "\n"
	     "Options:\n"
	     "  -a, --all                   Show stats with zero count\n"
	     "  -i, --interval=INTERVAL     Refresh interval (interactive mode)\n"
	     "      --mapper-names          Show mapper names for dm-multipath devices\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Interactivity:\n"
	     "  q                           Quit");
}

#ifndef BCACHEFS_NCURSES

static void print_stats_header(void)
{
	printf("\033[1m%-40s %10s %12s %12s %12s %12s\033[0m\n", "NAME",
	       "COUNT", "MEAN", "MAX", "MEAN_REC", "STDDEV");
}

static void print_stats_row(const struct stat_entry *e, u64 max_ns)
{
	char *max = fmt_duration_adaptive(e->stats.duration_ns.max, max_ns);
	char *mean = fmt_duration_adaptive(e->stats.duration_ns.mean, max_ns);
	char *mean_recent =
		fmt_duration_adaptive(e->stats.duration_ewma_ns.mean, max_ns);
	char *stddev =
		fmt_duration_adaptive(e->stats.duration_ns.stddev, max_ns);

	printf("%-40s %10llu %12s %12s %12s %12s\n", e->name, e->stats.count,
	       mean, max, mean_recent, stddev);

	free(max);
	free(mean);
	free(mean_recent);
	free(stddev);
}

#else
/* To be implemented in Phase 2 */
#endif

int cmd_timestats(int argc, char *argv[])
{
	enum { OPT_MAPPER_NAMES = 1000 };
	static const struct option longopts[] = {
		{ "all", no_argument, NULL, 'a' },
		{ "interval", required_argument, NULL, 'i' },
		{ "mapper-names", no_argument, NULL, OPT_MAPPER_NAMES },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	bool show_all = false;
	double interval = 1.0;
	enum device_name_mode mode = DEVICE_NAME_RAW;
	int opt;

	while ((opt = getopt_long(argc, argv, "ai:h", longopts, NULL)) != -1)
		switch (opt) {
		case 'a':
			show_all = true;
			break;
		case 'i':
			interval = atof(optarg);
			break;
		case OPT_MAPPER_NAMES:
			mode = DEVICE_NAME_MAPPER;
			break;
		case 'h':
			timestats_usage();
			return 0;
		default:
			timestats_usage();
			return -EINVAL;
		}
	args_shift(optind);

	char *fs_path = arg_pop();
	struct bchfs_handle fs;
	char *sysfs_path = NULL;
	DARRAY(char *) sysfs_paths = {};

	if (fs_path) {
		fs = bcache_fs_open(fs_path);
		char uuid_str[40];
		uuid_unparse(fs.uuid.b, uuid_str);
		sysfs_path = mprintf(SYSFS_BASE "%s", uuid_str);
		darray_push(&sysfs_paths, sysfs_path);
		bcache_fs_close(fs);
	} else {
		DIR *dir = opendir(SYSFS_BASE);
		if (!dir)
			die("No bcachefs filesystems found (%s/ does not exist)", SYSFS_BASE);
		struct dirent *d;
		while ((d = readdir(dir))) {
			if (d->d_name[0] == '.')
				continue;
			char *p = mprintf(SYSFS_BASE "%s", d->d_name);
			darray_push(&sysfs_paths, p);
		}
		closedir(dir);
	}

	if (!sysfs_paths.nr)
		die("No mounted bcachefs filesystems found");

#ifndef BCACHEFS_NCURSES
	bool is_tty = isatty(STDOUT_FILENO);
	struct termios orig_termios;

	if (is_tty) {
		tcgetattr(STDIN_FILENO, &orig_termios);
		struct termios raw = orig_termios;
		raw.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
		printf("\033[?1049h"); /* Use alternate screen buffer */
	}

	enum { PAGE_BASE, PAGE_DEVICES } page = PAGE_BASE;

	while (true) {
		for (size_t fs_idx = 0; fs_idx < sysfs_paths.nr; fs_idx++) {
			sysfs_path = sysfs_paths.data[fs_idx];
			stat_entries entries = { 0 };
			if (page == PAGE_BASE)
				collect_time_stats(sysfs_path, &entries);
			else
				collect_device_latency_stats(sysfs_path, mode, &entries);

			qsort(entries.data, entries.nr, sizeof(entries.data[0]),
			      stat_entry_cmp);

			u64 overall_max_ns = 0;
			darray_for_each(entries, e)
			{
				if (e->stats.duration_ns.max > overall_max_ns)
					overall_max_ns = e->stats.duration_ns.max;
			}

			if (is_tty)
				printf("\033[H\033[J");

			if (sysfs_paths.nr > 1)
				printf("Filesystem: %s\n", sysfs_path);

			printf("bcachefs timestats (%s) [%s] - Interval: %.1fs\n",
			       sysfs_path, page == PAGE_BASE ? "base" : "devices",
			       interval);
			printf("Press 'q' to exit, 'Tab' to switch page\n\n");
			print_stats_header();

			darray_for_each(entries, e)
			{
				if (show_all || e->stats.count > 0)
					print_stats_row(e, overall_max_ns);
				free(e->name);
			}
			darray_exit(&entries);

			if (!is_tty && fs_idx + 1 < sysfs_paths.nr)
				printf("\n");
		}

		if (!is_tty)
			break;

		struct pollfd fds = { .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(&fds, 1, interval * 1000) > 0) {
			char c;
			if (read(STDIN_FILENO, &c, 1) > 0) {
				if (c == 'q')
					break;
				if (c == '\t')
					page = (page + 1) % 2;
			}
		}
	}

	if (is_tty) {
		printf("\033[?1049l"); /* Restore original screen buffer */
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
	}
#else
		/* To be implemented in Phase 2 */
#endif

	darray_for_each(sysfs_paths, p) free(*p);
	darray_exit(&sysfs_paths);
	return 0;
}
