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

static bool show_reconcile_status(struct bchfs_handle fs, unsigned types_mask)
{
	struct accounting_result res = bchu_fs_accounting_query(
		fs, 1 << BCH_DISK_ACCOUNTING_reconcile_work);

	u64 scan_pending = read_file_u64(fs.sysfs_fd, "reconcile_scan_pending");
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

	prt_printf(&out, "\tdata\rmetadata\r\n");

	for (unsigned i = 0; i < BCH_RECONCILE_ACCOUNTING_NR; i++) {
		if (!(types_mask & (1 << i)))
			continue;

		prt_printf(&out, "  ");
		bch2_prt_reconcile_accounting_type(&out, i);
		prt_printf(&out, ":\t");

		prt_units_u64(&out, v[i][0] << 9);
		prt_printf(&out, "\r");

		prt_units_u64(&out, v[i][1] << 9);
		prt_printf(&out, "\r\n");

		if (v[i][0] || v[i][1])
			have_pending = true;
	}

	bch2_printbuf_tabstop_align(&out);
	printf("%s", out.buf);

	printbuf_exit(&out);
	bchu_accounting_result_free(&res);

	char *status = read_file_str(fs.sysfs_fd, "reconcile_status");
	if (status) {
		printf("\n%s\n", status);
		free(status);
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
	unsigned types_mask = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "t:h", longopts, NULL)) != -1)
		switch (opt) {
		case 't':
			types_mask = read_flag_list_or_die(
				optarg, reconcile_types, "reconcile type");
			break;
		case 'h':
			reconcile_status_usage();
			return 0;
		default:
			reconcile_status_usage();
			return -EINVAL;
		}
	args_shift(optind);

	if (!types_mask)
		types_mask = (1 << BCH_RECONCILE_ACCOUNTING_NR) - 1;

	char *fs_path = arg_pop() ?: ".";
	struct bchfs_handle fs = bcache_fs_open(fs_path);

	show_reconcile_status(fs, types_mask);

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
	unsigned types_mask = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "t:h", longopts, NULL)) != -1)
		switch (opt) {
		case 't':
			types_mask = read_flag_list_or_die(
				optarg, reconcile_types, "reconcile type");
			break;
		case 'h':
			reconcile_wait_usage();
			return 0;
		default:
			reconcile_wait_usage();
			return -EINVAL;
		}
	args_shift(optind);

	if (!types_mask)
		types_mask = ((1 << BCH_RECONCILE_ACCOUNTING_NR) - 1) &
			     ~(1 << BCH_RECONCILE_ACCOUNTING_pending);

	char *fs_path = arg_pop() ?: ".";
	struct bchfs_handle fs = bcache_fs_open(fs_path);

	write_file_str(fs.sysfs_fd, "internal/trigger_reconcile_wakeup", "1");

	bool is_tty = isatty(STDOUT_FILENO);

	while (true) {
		if (is_tty)
			printf("\033[H\033[J");

		if (!show_reconcile_status(fs, types_mask))
			break;

		sleep(1);
	}

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
