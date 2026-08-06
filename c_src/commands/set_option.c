/*
 * set-fs-option: Set a filesystem or device option.
 *
 * Changes are persisted to the superblock. If the filesystem is currently
 * mounted, the change will also be applied via sysfs.
 *
 * Ported from src/commands/set_option.rs.
 *
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
#include "sb/io.h"
#include "sb/members.h"
#include "cmds.h"

static void set_option_usage(void)
{
	puts("bcachefs set-fs-option - set a filesystem option\n"
	     "Usage: bcachefs set-fs-option [OPTION]... <devices>\n"
	     "\n"
	     "Options:");
	bch2_opts_usage(OPT_FS | OPT_DEVICE, 0);
	puts("  -d, --dev-idx=index         Device index for device-specific options\n"
	     "  -v, --verbose               Verbose mode\n"
	     "  -h, --help                  Display this help and exit\n"
	     "\n"
	     "Changes are persisted to the superblock. If the filesystem is currently\n"
	     "mounted, the change will also be applied via sysfs.\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int name_to_dev_idx(struct bch_fs *c, const char *dev)
{
	int ret = -1;

	rcu_read_lock();
	for_each_member_device_rcu(c, ca, NULL)
	{
		if (!strcmp(ca->name, dev)) {
			ret = ca->dev_idx;
			break;
		}
	}
	rcu_read_unlock();

	return ret;
}

int cmd_set_option(int argc, char *argv[])
{
	struct bch_opt_strs new_opt_strs =
		bch2_cmdline_opts_get(&argc, argv, OPT_FS | OPT_DEVICE);
	DARRAY(unsigned) dev_idxs = {};
	unsigned verbosity = 0;
	int opt;

	static const struct option longopts[] = {
		{ "dev-idx", required_argument, NULL, 'd' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};

	while ((opt = getopt_long(argc, argv, "d:vh", longopts, NULL)) != -1)
		switch (opt) {
		case 'd': {
			unsigned dev_idx;
			if (kstrtouint(optarg, 10, &dev_idx))
				die("error parsing %s", optarg);
			darray_push(&dev_idxs, dev_idx);
			break;
		}
		case 'v':
			verbosity++;
			break;
		case 'h':
			set_option_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply device(s)");

	bool any_defined = false;
	for (unsigned i = 0; i < bch2_opts_nr; i++)
		if (new_opt_strs.by_id[i]) {
			any_defined = true;
			break;
		}

	if (!any_defined)
		die("No options specified");

	bool online = false;
	for (int i = 0; i < argc; i++) {
		if (dev_mounted(argv[i])) {
			online = true;
			break;
		}
	}

	if (online) {
		int dev_idx;
		struct bchfs_handle fs = bchu_fs_open_by_dev(argv[0], &dev_idx);

		for (int i = 1; i < argc; i++) {
			struct bchfs_handle fs2 =
				bchu_fs_open_by_dev(argv[i], &dev_idx);
			if (memcmp(&fs.uuid, &fs2.uuid, sizeof(fs.uuid)))
				die("Filesystem mounted, but not all devices are members");
			bcache_fs_close(fs2);
		}

		for (unsigned i = 0; i < bch2_opts_nr; i++) {
			if (!new_opt_strs.by_id[i])
				continue;

			const struct bch_option *opt = bch2_opt_table + i;
			if (!(opt->flags & (OPT_FS | OPT_DEVICE))) {
				fprintf(stderr, "Can't set option %s\n", opt->attr.name);
				continue;
			}

			if ((opt->flags & OPT_FS) && !(opt->flags & OPT_DEVICE)) {
				char *path = mprintf("options/%s", opt->attr.name);
				write_file_str(fs.sysfs_fd, path, new_opt_strs.by_id[i]);
				free(path);
			}

			if (opt->flags & OPT_DEVICE) {
				if (dev_idxs.nr) {
					darray_for_each(dev_idxs, d)
					{
						char *path = mprintf("dev-%u/%s", *d, opt->attr.name);
						write_file_str(fs.sysfs_fd, path, new_opt_strs.by_id[i]);
						free(path);
					}
				} else {
					for (int j = 0; j < argc; j++) {
						struct bchfs_handle fs2 = bchu_fs_open_by_dev(argv[j], &dev_idx);
						if (dev_idx < 0) {
							fprintf(stderr,
								"Couldn't determine device index for %s; use --dev-idx\n",
								argv[j]);
							bcache_fs_close(fs2);
							continue;
						}
						char *path = mprintf("dev-%u/%s", dev_idx, opt->attr.name);
						write_file_str(fs.sysfs_fd, path, new_opt_strs.by_id[i]);
						free(path);
						bcache_fs_close(fs2);
					}
				}
			}
		}
		bcache_fs_close(fs);
	} else {
		/* Update the superblock manually: */
		darray_const_str devs = get_or_split_cmdline_devs(argc, argv);
		struct bch_opts open_opts = bch2_opts_empty();
		opt_set(open_opts, nostart, true);
		if (verbosity)
			opt_set(open_opts, verbose, true);

		struct bch_fs *c = bch2_fs_open(&devs, &open_opts);
		if (IS_ERR(c))
			die("error opening %s: %s", argv[0], bch2_err_str(PTR_ERR(c)));

		bool modified = false;

		for (unsigned i = 0; i < bch2_opts_nr; i++) {
			if (!new_opt_strs.by_id[i])
				continue;

			const struct bch_option *opt = bch2_opt_table + i;
			if (!(opt->flags & (OPT_FS | OPT_DEVICE))) {
				fprintf(stderr, "Can't set option %s\n", opt->attr.name);
				continue;
			}

			struct printbuf err = PRINTBUF;
			u64 v;
			int ret = bch2_opt_parse(c, opt, new_opt_strs.by_id[i],
						 &v, &err);
			printbuf_exit(&err);
			if (ret < 0) {
				fprintf(stderr, "Error parsing %s=%s\n",
					opt->attr.name, new_opt_strs.by_id[i]);
				continue;
			}

			if (opt->flags & OPT_FS) {
				ret = bch2_opt_hook_pre_set(c, NULL, 0, i, v, true, NULL);
				if (ret < 0) {
					fprintf(stderr, "Error setting %s: %s\n",
						opt->attr.name, bch2_err_str(ret));
					continue;
				}
				bch2_opt_set_sb(c, NULL, opt, v,
						new_opt_strs.by_id[i]);
				modified = true;
			}

			if (opt->flags & OPT_DEVICE) {
				if (dev_idxs.nr) {
					darray_for_each(dev_idxs, d)
					{
						struct bch_dev *ca = bch2_dev_tryget_noerror(c, *d);
						if (!ca) {
							fprintf(stderr, "Couldn't look up device %u\n", *d);
							continue;
						}

						ret = bch2_opt_hook_pre_set(c, ca, 0, i, v, true, NULL);
						if (ret < 0) {
							fprintf(stderr, "error setting %s: %s\n",
								opt->attr.name, bch2_err_str(ret));
							bch2_dev_put(ca);
							continue;
						}
						bch2_opt_set_sb(c, ca, opt, v,
								new_opt_strs.by_id[i]);
						bch2_dev_put(ca);
						modified = true;
					}
				} else {
					for (int j = 0; j < argc; j++) {
						int idx = name_to_dev_idx(c, argv[j]);
						if (idx < 0) {
							fprintf(stderr,"Couldn't look up device %s\n", argv[j]);
							continue;
						}

						struct bch_dev *ca = c->devs[idx];
						ret = bch2_opt_hook_pre_set(c, ca, 0, i, v, true, NULL);
						if (ret < 0) {
							fprintf(stderr, "error setting %s: %s\n",
								opt->attr.name, bch2_err_str(ret));
							continue;
						}
						bch2_opt_set_sb(c, ca, opt, v,
								new_opt_strs.by_id[i]);
						modified = true;
					}
				}
			}
		}

		if (modified) {
			if (!BCH_SB_INITIALIZED(c->disk_sb.sb))
				die("superblock not initialized (filesystem was never started): "
				    "bch2_write_super would silently skip the write; mount it once first");
			{
				guard(mutex_noio)(&c->sb_lock);
				bool saved = c->opts.nochanges;
				c->opts.nochanges = false;
				bch2_write_super(c);
				c->opts.nochanges = saved;
			}
		}
		bch2_fs_stop(c);
		darray_exit(&devs);
	}

	darray_exit(&dev_idxs);
	bch2_opt_strs_free(&new_opt_strs);
	return 0;
}
