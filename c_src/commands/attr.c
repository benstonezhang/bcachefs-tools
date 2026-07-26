/*
 * set-file-option / get-file-option: Per-file IO path options.
 *
 * Sets or shows per-file or per-directory IO path options, overriding the
 * filesystem-wide defaults. When set on a directory, options are
 * propagated recursively to existing children and inherited by new files.
 *
 * Changed options take effect immediately for new writes. For existing
 * data, reconcile applies the new options in the background. Use
 * --option=- to remove a specific option, or --remove-all to clear all.
 *
 * Ported from src/commands/attr.rs.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"

static void propagate_recurse(int dirfd, const char *path)
{
	DIR *dir = fdopendir(dirfd);
	struct dirent *d;

	if (!dir) {
		fprintf(stderr, "%s: fdopendir() error: %m\n", path);
		close(dirfd);
		return;
	}

	while ((errno = 0), (d = readdir(dir))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		int ret = ioctl(dirfd, BCHFS_IOC_REINHERIT_ATTRS, d->d_name);
		if (ret < 0) {
			fprintf(stderr,
				"%s/%s: error propagating attributes: %m\n",
				path, d->d_name);
			continue;
		}

		if (!ret) /* did no work */
			continue;

		struct stat st;
		if (fstatat(dirfd, d->d_name, &st, AT_SYMLINK_NOFOLLOW))
			continue;

		if (!S_ISDIR(st.st_mode))
			continue;

		int fd = openat(dirfd, d->d_name, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "%s/%s: error opening: %m\n", path,
				d->d_name);
			continue;
		}

		char *next_path = mprintf("%s/%s", path, d->d_name);
		propagate_recurse(fd, next_path);
		free(next_path);
	}

	if (errno)
		fprintf(stderr, "%s: readdir error: %m\n", path);
	closedir(dir);
}

static void remove_bcachefs_attr(const char *path, const char *attr_name)
{
	if (removexattr(path, attr_name)) {
		if (errno != ENODATA && errno != EINVAL) {
			fprintf(stderr,
				"error removing attribute %s from %s: %m\n",
				attr_name, path);
		}
	}
}

static void do_setattr(char *path, struct bch_opt_strs opts, bool remove_all)
{
	unsigned i;

	if (remove_all) {
		for (i = 0; i < bch2_opts_nr; i++) {
			const struct bch_option *opt = &bch2_opt_table[i];
			if (!(opt->flags & OPT_INODE))
				continue;
			if (!strcmp(opt->attr.name, "casefold"))
				continue;

			char *n = mprintf("bcachefs.%s", opt->attr.name);
			remove_bcachefs_attr(path, n);
			free(n);
		}
	}

	for (i = 0; i < bch2_opts_nr; i++) {
		if (!opts.by_id[i])
			continue;

		char *n = mprintf("bcachefs.%s", bch2_opt_table[i].attr.name);

		if (!strcmp(opts.by_id[i], "-")) {
			remove_bcachefs_attr(path, n);
		} else {
			if (setxattr(path, n, opts.by_id[i],
				     strlen(opts.by_id[i]), 0))
				die("setting %s on %s: %m", n, path);
		}

		free(n);
	}

	struct stat st = xstat(path);
	if (S_ISDIR(st.st_mode)) {
		int dirfd = open(path, O_RDONLY);
		if (dirfd < 0)
			die("error opening %s: %m", path);

		propagate_recurse(dirfd, path);
	}
}

static void setattr_usage(void)
{
	puts("bcachefs set-file-option - set attributes on files in a bcachefs filesystem\n"
	     "Usage: bcachefs set-file-option [OPTIONS]... <files>\n"
	     "\n"
	     "Options:\n"
	     "      --remove-all      Remove all file options");

	bch2_opts_usage(OPT_INODE, 0);
	puts("  -h            Display this help and exit\n"
	     "\n"
	     "To remove a specific option, use: --option=-\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_setattr(int argc, char *argv[])
{
	bool remove_all = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--remove-all")) {
			remove_all = true;
			memmove(&argv[i], &argv[i + 1],
				sizeof(char *) * (argc - i - 1));
			argc--;
			argv[argc] = NULL;
			i--;
		} else if (!strcmp(argv[i], "-h")) {
			setattr_usage();
			exit(EXIT_SUCCESS);
		}
	}

	struct bch_opt_strs opts =
		bch2_cmdline_opts_get(&argc, argv, OPT_INODE);

	for (i = 1; i < argc; i++)
		if (argv[i][0] == '-') {
			printf("invalid option %s\n", argv[i]);
			setattr_usage();
			exit(EXIT_FAILURE);
		}

	if (argc <= 1)
		die("Please supply one or more files");

	for (i = 1; i < argc; i++)
		do_setattr(argv[i], opts, remove_all);

	bch2_opt_strs_free(&opts);

	return 0;
}

/*
 * Read a bcachefs.* xattr. Returns a malloc'd string, or NULL when the
 * attribute is unset / unsupported (NODATA, NOTSUP, INVAL).
 */
static char *read_bcachefs_attr(const char *path, const char *attr)
{
	ssize_t len = getxattr(path, attr, NULL, 0);

	if (len < 0) {
		if (errno == ENODATA || errno == EOPNOTSUPP ||
		    errno == EINVAL)
			return NULL;
		die("reading %s from %s: %m", attr, path);
	}

	char *buf = malloc(len + 1);
	if (!buf)
		die("out of memory");

	ssize_t n = getxattr(path, attr, buf, len);
	if (n < 0) {
		free(buf);
		if (errno == ENODATA || errno == EOPNOTSUPP ||
		    errno == EINVAL)
			return NULL;
		die("reading %s from %s: %m", attr, path);
	}

	buf[n] = '\0';
	return buf;
}

static void getattr_usage(void)
{
	puts("bcachefs get-file-option - show file-level options\n"
	     "Usage: bcachefs get-file-option [OPTIONS]... <files>\n"
	     "\n"
	     "Options:\n"
	     "  -e, --effective       Show inherited/effective file options\n"
	     "  -a, --all             Show unset options as '-'\n"
	     "  -h, --help            Display this help and exit\n"
	     "\n"
	     "By default only explicitly set file options are printed.\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_getattr(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "effective", no_argument, NULL, 'e' },
		{ "all", no_argument, NULL, 'a' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	bool effective = false, all = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "eah", longopts, NULL)) != -1)
		switch (opt) {
		case 'e':
			effective = true;
			break;
		case 'a':
			all = true;
			break;
		case 'h':
			getattr_usage();
			exit(EXIT_SUCCESS);
		default:
			getattr_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (argc == 0)
		die("Please supply one or more files");

	const char *prefix = effective ? "bcachefs_effective" : "bcachefs";
	bool multi_file = argc > 1;

	for (int f = 0; f < argc; f++) {
		const char *file = argv[f];

		for (unsigned i = 0; i < bch2_opts_nr; i++) {
			const struct bch_option *opt = &bch2_opt_table[i];
			if (!(opt->flags & OPT_INODE))
				continue;

			const char *name = opt->attr.name;
			char *attr = mprintf("%s.%s", prefix, name);
			char *value = read_bcachefs_attr(file, attr);

			free(attr);

			if (value) {
				if (multi_file)
					printf("%s\t%s\t%s\n", file, name,
					       value);
				else
					printf("%s\t%s\n", name, value);
				free(value);
			} else if (all) {
				if (multi_file)
					printf("%s\t%s\t-\n", file, name);
				else
					printf("%s\t-\n", name);
			}
		}
	}

	return 0;
}

static void reflink_propagate_usage(void)
{
	puts("bcachefs reflink-option-propagate - propagate IO options to reflinked extents\n"
	     "Usage: bcachefs reflink-option-propagate [OPTIONS]... <files>\n"
	     "\n"
	     "Options:\n"
	     "      --set-may-update  Enable option propagation on old reflink_p extents\n"
	     "  -h, --help            Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

/*
 * reflink-option-propagate: Propagate IO options to reflinked extents.
 *
 * Propagates each file's current IO options to its extents, including reflinked
 * extents. Reflinked data is shared, so propagation is gated by the
 * REFLINK_P_MAY_UPDATE_OPTIONS permission flag.
 *
 * Use --set-may-update to enable the flag on old reflink pointers.
 */
int cmd_reflink_option_propagate(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "set-may-update", no_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	bool set_may_update = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "sh", longopts, NULL)) != -1)
		switch (opt) {
		case 's':
			set_may_update = true;
			break;
		case 'h':
			reflink_propagate_usage();
			exit(EXIT_SUCCESS);
		default:
			reflink_propagate_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (argc == 0)
		die("Please supply one or more files");

	bool errors = false;
	for (int i = 0; i < argc; i++) {
		int fd = open(argv[i], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "%s: error opening: %m\n", argv[i]);
			errors = true;
			continue;
		}

		if (set_may_update) {
			if (ioctl(fd, BCHFS_IOC_SET_REFLINK_P_MAY_UPDATE_OPTS) <
			    0) {
				fprintf(stderr,
					"%s: set may_update_opts error: %m\n",
					argv[i]);
				close(fd);
				errors = true;
				continue;
			}
		}

		if (ioctl(fd, BCHFS_IOC_PROPAGATE_REFLINK_P_OPTS) < 0) {
			if (errno == EPERM) {
				fprintf(stderr,
					"%s: reflink_p extents without may_update_options set;\n"
					"rerun as root with --set-may-update\n",
					argv[i]);
			} else {
				fprintf(stderr,
					"%s: propagate reflink opts error: %m\n",
					argv[i]);
			}
			errors = true;
		}
		close(fd);
	}

	if (errors)
		fprintf(stderr, "some files had errors\n");
	return errors ? 1 : 0;
}
