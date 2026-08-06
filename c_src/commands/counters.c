#include <ctype.h>
#include <getopt.h>
#include <string.h>

#include "libbcachefs.h"
#include "init/fs.h"
#include "sb/io.h"
#include "sb/counters.h"
#include "sb/counters_format.h"
#include "cmds.h"

static void reset_counters_usage(void)
{
	puts("bcachefs reset-counters - Reset all counters on an unmounted device\n"
	     "Usage: bcachefs reset-counters [OPTION]... device\n"
	     "\n"
	     "Options:\n"
	     "  -c, --counters, --counter NAMES\n"
	     "                              Reset specific counters (comma-separated), not all\n"
	     "  -h, --help                  Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
	exit(EXIT_SUCCESS);
}

static int match_counter(const char *name)
{
	for (int i = 0; i < BCH_COUNTER_NR; i++) {
		if (bch2_counter_names[i] &&
		    !strcmp(bch2_counter_names[i], name))
			return i;
	}
	return -1;
}

static char *trim(char *s)
{
	char *end;
	while (isspace((unsigned char)*s))
		s++;
	if (*s == 0)
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		end--;
	end[1] = '\0';
	return s;
}

int cmd_reset_counters(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "counters", required_argument, NULL, 'c' },
		{ "help", 0, NULL, 'h' },
		{ NULL }
	};
	char *counters_arg = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:h", longopts, NULL)) != -1)
		switch (opt) {
		case 'c':
			counters_arg = optarg;
			break;
		case 'h':
			reset_counters_usage();
			break;
		}
	args_shift(optind);

	char *dev_path = arg_pop();
	if (!dev_path)
		die("please supply a device");
	if (argc)
		die("too many arguments");

	darray_s32 to_reset = { 0 };
	if (counters_arg) {
		char *s, *orig = strdup(counters_arg);
		char *p = orig;
		while ((s = strsep(&p, ","))) {
			s = trim(s);
			if (!*s)
				continue;
			int idx = match_counter(s);
			if (idx < 0)
				die("invalid counter '%s'", s);
			darray_push(&to_reset, idx);
		}
		free(orig);
	}

	char *devs_str = bch2_scan_devices(dev_path);
	if (!devs_str)
		die("no devices found for %s", dev_path);

	darray_const_str devs = { 0 };
	char *p = devs_str, *s;
	while ((s = strsep(&p, ":")))
		darray_push(&devs, s);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);
	opt_set(opts, degraded, BCH_DEGRADED_very);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	if (IS_ERR(c))
		die("Error opening %s: %s", dev_path, bch2_err_str(PTR_ERR(c)));

	if (to_reset.nr == 0) {
		for (int i = 0; i < BCH_COUNTER_NR; i++)
			bch2_counter_reset(c, i);
	} else {
		darray_for_each(to_reset, i) bch2_counter_reset(c, *i);
	}

	{
		guard(mutex_noio)(&c->sb_lock);
		bch2_write_super(c);
	}

	bch2_fs_stop(c);

	darray_exit(&to_reset);
	free(devs_str);
	darray_exit(&devs);
	return 0;
}
