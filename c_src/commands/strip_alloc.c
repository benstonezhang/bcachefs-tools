/*
 * GPLv2
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "init/fs.h"
#include "cmds.h"

static void strip_alloc_usage(void)
{
	puts("bcachefs strip-alloc - remove alloc info and journal from a filesystem\n"
	     "Removes metadata unneeded for running in read-only mode\n"
	     "Alloc info and journal will be recreated on first RW mount\n"
	     "Usage: bcachefs strip_alloc [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help              Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_strip_alloc(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			strip_alloc_usage();
			exit(16);
		}
	args_shift(optind);

	if (!argc) {
		fprintf(stderr, "Please supply device(s)\n");
		exit(8);
	}

	darray_const_str devs = get_or_split_cmdline_devs(argc, argv);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);
reopen:
	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	int ret = PTR_ERR_OR_ZERO(c);
	if (ret)
		die("Error opening filesystem: %s", bch2_err_str(ret));

	if (!c->sb.clean) {
		printf("Filesystem not clean, running recovery");
		ret = bch2_fs_start(c);
		if (ret) {
			fprintf(stderr, "Error starting filesystem: %s\n",
				bch2_err_str(ret));
			goto err_stop;
		}
		bch2_fs_stop(c);
		goto reopen;
	}

	u64 capacity = 0;
	for_each_member_device(c, ca)
		capacity += ca->mi.nbuckets * (ca->mi.bucket_size << 9);

	if (capacity > 1ULL << 40) {
		fprintf(stderr,
			"capacity too large for alloc info reconstruction, exiting\n");
		goto err_stop;
	}

	printf("Stripping alloc info from %s\n", argv[0]);

	mutex_lock(&c->sb_lock);
	strip_fs_alloc(c);
	bch2_write_super(c);
	mutex_unlock(&c->sb_lock);
err_stop:
	bch2_fs_stop(c);
	darray_exit(&devs);
	return ret;
}
