/*
 * recovery-pass: List and manage scheduled recovery passes.
 *
 * Allows scheduling or descheduling specific recovery passes to be run
 * on next mount.
 *
 * Ported from src/commands/recovery_pass.rs.
 *
 * GPLv2
 */

#include <getopt.h>
#include <string.h>

#include "libbcachefs.h"
#include "init/fs.h"
#include "init/passes.h"
#include "sb/io.h"
#include "cmds.h"

static void recovery_pass_usage(void)
{
	puts("bcachefs recovery-pass - List and manage scheduled recovery passes\n"
	     "Usage: bcachefs recovery-pass [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -s, --set=pass          Schedule a recovery pass\n"
	     "  -u, --unset=pass        Deschedule a recovery pass\n"
	     "  -h, --help              Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_recovery_pass(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "set", required_argument, NULL, 's' },
		{ "unset", required_argument, NULL, 'u' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	u64 passes_to_set = 0;
	u64 passes_to_unset = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "s:u:h", longopts, NULL)) != -1)
		switch (opt) {
		case 's':
			passes_to_set |= read_flag_list_or_die(
				optarg, bch2_recovery_passes, "recovery pass");
			break;
		case 'u':
			passes_to_unset |= read_flag_list_or_die(
				optarg, bch2_recovery_passes, "recovery pass");
			break;
		case 'h':
			recovery_pass_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply device(s)");

	passes_to_set = bch2_recovery_passes_to_stable(passes_to_set);
	passes_to_unset = bch2_recovery_passes_to_stable(passes_to_unset);

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_opts bch_opts = bch2_opts_empty();
	opt_set(bch_opts, nostart, true);

	struct bch_fs *c = bch2_fs_open(&devs, &bch_opts);
	if (IS_ERR(c))
		die("error opening %s: %s", devs.data[0],
		    bch2_err_str(PTR_ERR(c)));

	u64 scheduled;
	{
		guard(mutex_noio)(&c->sb_lock);

		unsigned ext_u64s = DIV_ROUND_UP(
			sizeof(struct bch_sb_field_ext), sizeof(u64));
		struct bch_sb_field_ext *ext =
			bch2_sb_field_get_minsize(&c->disk_sb, ext, ext_u64s);
		if (!ext)
			die("Error getting sb_field_ext");

		scheduled = le64_to_cpu(ext->recovery_passes_required[0]);

		if (passes_to_set || passes_to_unset) {
			ext->recovery_passes_required[0] &=
				cpu_to_le64(~passes_to_unset);
			ext->recovery_passes_required[0] |=
				cpu_to_le64(passes_to_set);
			scheduled =
				le64_to_cpu(ext->recovery_passes_required[0]);
			bch2_write_super(c);
		}
	}

	struct printbuf buf = PRINTBUF;
	prt_printf(&buf, "Scheduled recovery passes: ");

	if (scheduled) {
		prt_bitflags(&buf, bch2_recovery_passes,
			     bch2_recovery_passes_from_stable(scheduled));
	} else {
		prt_printf(&buf, "(none)");
	}

	printf("%s\n", buf.buf);
	printbuf_exit(&buf);

	bch2_fs_stop(c);
	darray_exit(&devs);

	return 0;
}
