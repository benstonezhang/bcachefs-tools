/*
 * reconcile: Re-verify/fix data layout and replicas.
 *
 * Key logic: background processes like copygc and rebalance must be disabled
 * while reconciliation is active to prevent race conditions.
 *
 * Ported from src/commands/reconcile.rs.
 */

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"

static const char *reconcile_types[] = {
	"replicas",	 "checksum", "erasure_code", "compression", "target",
	"high_priority", "pending",  "stripes",	     NULL
};

static int parse_reconcile_type(const char *name)
{
	for (int i = 0; reconcile_types[i]; i++) {
		if (!strcmp(name, reconcile_types[i]))
			return i;
	}
	return -1;
}

static unsigned *parse_ordered_types(const char *optarg)
{
	char *s, *orig = strdup(optarg);
	char *p = orig;
	unsigned *types = NULL;
	unsigned types_nr = 0;

	while ((s = strsep(&p, ","))) {
		if (!*s)
			continue;
		int idx = parse_reconcile_type(s);
		if (idx < 0)
			die("Bad reconcile type %s", s);
		types = realloc(types, (types_nr + 1) * sizeof(unsigned));
		types[types_nr++] = idx;
	}

	free(orig);
	if (!types_nr)
		return NULL;

	types = realloc(types, (types_nr + 1) * sizeof(unsigned));
	types[types_nr] = BCH_RECONCILE_ACCOUNTING_NR; // sentinel
	return types;
}

static void free_ordered_types(unsigned *types)
{
	free(types);
}

static void reconcile_status_usage(void)
{
	puts("bcachefs reconcile status - show reconcile status\n"
	     "Usage: bcachefs reconcile status [OPTION]... <mountpoint>\n"
	     "\n"
	     "Options:\n"
	     "  -t, --types=TYPES           Comma-separated list of types to show\n"
	     "  -h, --help                  Display this help and exit\n");
}

static void reconcile_wait_usage(void)
{
	puts("bcachefs reconcile wait - wait for reconcile to complete\n"
	     "Usage: bcachefs reconcile wait [OPTION]... <mountpoint>\n"
	     "\n"
	     "Options:\n"
	     "  -t, --types=TYPES           Comma-separated list of types to wait on\n"
	     "  -h, --help                  Display this help and exit\n");
}

static bool show_reconcile_status(struct bchfs_handle fs, unsigned *types,
				  bool print_status)
{
	struct accounting_result res = bchu_fs_accounting_query(
		fs, 1 << BCH_DISK_ACCOUNTING_reconcile_work);

	u64 scan_pending = read_file_u64(fs.sysfs_fd, "reconcile_scan_pending");
	if (print_status)
		printf("Scan pending: %llu\n", scan_pending);

	u64 v[BCH_RECONCILE_ACCOUNTING_NR][2];
	memset(v, 0, sizeof(v));

	darray_for_each(res.entries, i)
	{
		struct accounting_entry *e = *i;
		if (e->pos.type == BCH_DISK_ACCOUNTING_reconcile_work) {
			unsigned idx = e->pos.reconcile_work.type;
			if (idx < BCH_RECONCILE_ACCOUNTING_NR) {
				v[idx][0] = e->counters[0];
				v[idx][1] = e->counters[1];
			}
		}
	}

	bool have_pending = scan_pending != 0;
	struct printbuf out = PRINTBUF;
	out.human_readable_units = true;

	if (print_status)
		prt_printf(&out, "\tdata\rmetadata\r\n");

	for (unsigned *t = types; *t != BCH_RECONCILE_ACCOUNTING_NR; t++) {
		unsigned i = *t;
		if (i >= BCH_RECONCILE_ACCOUNTING_NR)
			continue;

		if (print_status)
			prt_printf(&out, "  ");
		if (print_status)
			bch2_prt_reconcile_accounting_type(&out, i);
		if (print_status)
			prt_printf(&out, ":\t");

		if (print_status)
			prt_units_u64(&out, v[i][0] << 9);
		if (print_status)
			prt_printf(&out, "\r");

		if (print_status)
			prt_units_u64(&out, v[i][1] << 9);
		if (print_status)
			prt_printf(&out, "\r\n");

		if (v[i][0] || v[i][1])
			have_pending = true;
	}

	if (print_status) {
		bch2_printbuf_tabstop_align(&out);
		printf("%s", out.buf);
	}

	printbuf_exit(&out);
	bchu_accounting_result_free(&res);

	if (print_status) {
		char *status = read_file_str(fs.sysfs_fd, "reconcile_status");
		if (status) {
			printf("\n%s\n", status);
			free(status);
		}
	}

	return have_pending;
}

static int cmd_reconcile_status(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "types", required_argument, NULL, 't' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	unsigned *types = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "t:h", longopts, NULL)) != -1)
		switch (opt) {
		case 't':
			types = parse_ordered_types(optarg);
			break;
		case 'h':
			reconcile_status_usage();
			return 0;
		default:
			reconcile_status_usage();
			return -EINVAL;
		}
	args_shift(optind);

	if (!types) {
		/* Default: all types in enum order */
		types = malloc((BCH_RECONCILE_ACCOUNTING_NR + 1) * sizeof(unsigned));
		for (unsigned i = 0; i < BCH_RECONCILE_ACCOUNTING_NR; i++)
			types[i] = i;
		types[BCH_RECONCILE_ACCOUNTING_NR] = BCH_RECONCILE_ACCOUNTING_NR;
	}

	char *fs_path = arg_pop() ?: ".";
	struct bchfs_handle fs = bcache_fs_open(fs_path);

	show_reconcile_status(fs, types, true);

	free_ordered_types(types);
	bcache_fs_close(fs);
	return 0;
}

static int cmd_reconcile_wait(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "types", required_argument, NULL, 't' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	unsigned *types = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "t:h", longopts, NULL)) != -1)
		switch (opt) {
		case 't':
			types = parse_ordered_types(optarg);
			break;
		case 'h':
			reconcile_wait_usage();
			return 0;
		default:
			reconcile_wait_usage();
			return -EINVAL;
		}
	args_shift(optind);

	if (!types) {
		/* Default: all types except pending in enum order */
		types = malloc((BCH_RECONCILE_ACCOUNTING_NR) * sizeof(unsigned));
		unsigned n = 0;
		for (unsigned i = 0; i < BCH_RECONCILE_ACCOUNTING_NR; i++) {
			if (i != BCH_RECONCILE_ACCOUNTING_pending)
				types[n++] = i;
		}
		types = realloc(types, (n + 1) * sizeof(unsigned));
		types[n] = BCH_RECONCILE_ACCOUNTING_NR;
	}

	char *fs_path = arg_pop() ?: ".";
	struct bchfs_handle fs = bcache_fs_open(fs_path);

	write_file_str(fs.sysfs_fd, "internal/trigger_reconcile_wakeup", "1");

	bool is_tty = isatty(STDOUT_FILENO);

	while (true) {
		if (is_tty)
			printf("\033[H\033[J");

		if (!show_reconcile_status(fs, types, is_tty))
			break;

		if (is_tty) {
			struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
			if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
				char ch;
				if (read(STDIN_FILENO, &ch, 1) == 1) {
					if (ch == 'q' || ch == 27 || ch == 3)
						break;
				}
			}
		} else {
			sleep(1);
		}
	}

	free_ordered_types(types);
	bcache_fs_close(fs);
	return 0;
}

int cmd_reconcile(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);

	if (!cmd) {
		reconcile_status_usage();
		return -EINVAL;
	}

	if (!strcmp(cmd, "status"))
		return cmd_reconcile_status(argc, argv);
	if (!strcmp(cmd, "wait"))
		return cmd_reconcile_wait(argc, argv);

	printf("Unknown reconcile command: %s\n", cmd);
	return -EINVAL;
}
