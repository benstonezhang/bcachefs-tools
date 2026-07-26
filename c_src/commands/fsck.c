#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "init/error.h"
#include "init/fs.h"
#include "fs/check.h"
#include "sb/io.h"
#include "cmds.h"

static void fsck_usage(void)
{
	puts("bcachefs fsck - Check an existing filesystem for errors\n"
	     "Usage: bcachefs fsck [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -p, -a                  Automatic repair (no questions)\n"
	     "  -n                      Don't repair, only check for errors\n"
	     "  -y                      Assume \"yes\" to all questions\n"
	     "  -f                      Force checking even if filesystem is marked clean\n"
	     "  -o mount_options        Additional mount options\n"
	     "  -r, --ratelimit_errors  Don't display more than 10 errors of a given type\n"
	     "  -k, --kernel            Use the in-kernel fsck implementation\n"
	     "  -K, --no-kernel         Don't use the in-kernel fsck implementation\n"
	     "      --noexcl            Open devices with O_NOEXCL\n"
	     "  -v                      Be verbose\n"
	     "  -h, --help              Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static void setnonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		die("fcntl error: %m");
}

static int do_splice(int rfd, int wfd)
{
	char buf[4096];
	ssize_t r = read(rfd, buf, sizeof(buf));

	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
	if (r == 0)
		return 1; /* EOF */

	ssize_t written = 0;
	while (written < r) {
		ssize_t w = write(wfd, buf + written, r - written);
		if (w < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd p = { .fd = wfd,
						    .events = POLLOUT };
				poll(&p, 1, -1);
				continue;
			}
			return -1;
		}
		written += w;
	}
	return 0;
}

static int splice_fd_to_stdinout(int fd)
{
	setnonblocking(STDIN_FILENO);
	setnonblocking(fd);

	bool stdin_closed = false;

	while (true) {
		struct pollfd pfds[2];
		int n = 0;

		pfds[n].fd = fd;
		pfds[n].events = POLLIN;
		n++;

		if (!stdin_closed) {
			pfds[n].fd = STDIN_FILENO;
			pfds[n].events = POLLIN;
			n++;
		}

		if (poll(pfds, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			die("poll error: %m");
		}

		int r = do_splice(fd, STDOUT_FILENO);
		if (r < 0)
			die("write to stdout error: %m");
		if (r > 0)
			break; /* fd closed */

		if (!stdin_closed) {
			r = do_splice(STDIN_FILENO, fd);
			if (r < 0)
				die("write to fsck_fd error: %m");
			if (r > 0)
				stdin_closed = true;
		}
	}

	return close(fd);
}

static int fsck_online(struct bchfs_handle fs, const char *opt_str)
{
	struct bch_ioctl_fsck_online fsck = { .opts = (unsigned long)opt_str };

	int fsck_fd = ioctl(fs.ioctl_fd, BCH_IOCTL_FSCK_ONLINE, &fsck);
	if (fsck_fd < 0)
		die("BCH_IOCTL_FSCK_ONLINE error: %s", bch2_err_str(errno));

	return splice_fd_to_stdinout(fsck_fd);
}

static void append_opt(struct printbuf *out, const char *opt)
{
	if (out->pos)
		prt_char(out, ',');
	prt_str(out, opt);
}

static bool should_use_kernel_fsck(darray_const_str devs)
{
	unsigned kernel_version = bcachefs_kernel_version();
	if (!kernel_version)
		return false;

	unsigned current = bcachefs_metadata_version_current;
	if (kernel_version == current)
		return false;

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);
	opt_set(opts, noexcl, true);
	opt_set(opts, nochanges, true);
	opt_set(opts, read_only, true);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	if (IS_ERR(c))
		return false;

	unsigned sb_version = c->sb.version;
	bool ret =
		((current < kernel_version && kernel_version <= sb_version) ||
		 (sb_version <= kernel_version && kernel_version < current));

	if (ret) {
		struct printbuf buf = PRINTBUF;
		prt_str(&buf, "fsck binary is version ");
		bch2_version_to_text(&buf, current);
		prt_str(&buf, " but filesystem is ");
		bch2_version_to_text(&buf, sb_version);
		prt_str(&buf, " and kernel is ");
		bch2_version_to_text(&buf, kernel_version);
		prt_str(&buf, ", using kernel fsck\n");
		printf("%s", buf.buf);
		printbuf_exit(&buf);
	}

	bch2_fs_stop(c);
	return ret;
}

static bool is_blockdev(const char *path)
{
	struct stat s;
	if (stat(path, &s))
		return true;
	return S_ISBLK(s.st_mode);
}

static void loopdev_free(const char *path)
{
	char *cmd = mprintf("losetup -d %s", path);
	(void)!system(cmd);
	free(cmd);
}

static char *loopdev_alloc(const char *path)
{
	char *cmd = mprintf("losetup --show -f %s", path);
	FILE *f = popen(cmd, "r");
	free(cmd);
	if (!f)
		return NULL;

	char *line = NULL;
	size_t n = 0;
	if (getline(&line, &n, f) < 0) {
		pclose(f);
		return NULL;
	}
	pclose(f);
	strim(line);
	return line;
}

static bool has_recovery_passes(const char *optarg)
{
	if (!optarg)
		return false;
	char *dup = strdup(optarg);
	if (!dup)
		die("malloc error: %m");
	char *p = dup;
	char *tok;
	bool found = false;
	while ((tok = strsep(&p, ","))) {
		if (!strcmp(tok, "recovery_passes") ||
		    !strncmp(tok, "recovery_passes=", 16)) {
			found = true;
			break;
		}
	}
	free(dup);
	return found;
}

int cmd_fsck(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "ratelimit_errors", no_argument, NULL, 'r' },
		{ "kernel", no_argument, NULL, 'k' },
		{ "no-kernel", no_argument, NULL, 'K' },
		{ "noexcl", no_argument, NULL, 'e' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	int kernel = -1;
	int opt, ret = 0;
	struct printbuf user_opts = PRINTBUF;
	struct printbuf opts_str = PRINTBUF;
	bool force = false;
	bool yes = false;
	bool nochanges = false;
	bool ratelimit_errors = false;
	bool noexcl = false;
	bool verbose = false;
	bool user_set_recovery_passes = false;

	if (getenv("BCACHEFS_KERNEL_ONLY"))
		kernel = 1;

	while ((opt = getopt_long(argc, argv, "apynfo:rkKvh", longopts,
				  NULL)) != -1)
		switch (opt) {
		case 'a':
		case 'p':
			/* automatic run */
			exit(EXIT_SUCCESS);
		case 'y':
			yes = true;
			break;
		case 'n':
			nochanges = true;
			break;
		case 'f':
			force = true;
			break;
		case 'o':
			if (has_recovery_passes(optarg))
				user_set_recovery_passes = true;
			append_opt(&user_opts, optarg);
			break;
		case 'r':
			ratelimit_errors = true;
			break;
		case 'k':
			kernel = 1;
			break;
		case 'K':
			if (!getenv("BCACHEFS_KERNEL_ONLY"))
				kernel = 0;
			break;
		case 'e':
			noexcl = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			fsck_usage();
			exit(EXIT_SUCCESS);
		default:
			fsck_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	append_opt(&opts_str, "degraded");
	if (!user_set_recovery_passes)
		append_opt(&opts_str, "fsck");
	append_opt(&opts_str, "fix_errors=ask");
	append_opt(&opts_str, "read_only");
	append_opt(&opts_str, "noreconcile_enabled");

	if (yes)
		append_opt(&opts_str, "fix_errors=yes");
	if (nochanges) {
		append_opt(&opts_str, "nochanges");
		append_opt(&opts_str, "fix_errors=no");
	}
	if (user_opts.pos)
		append_opt(&opts_str, user_opts.buf);
	if (ratelimit_errors)
		append_opt(&opts_str, "ratelimit_errors");
	if (noexcl)
		append_opt(&opts_str, "noexcl");
	if (verbose)
		append_opt(&opts_str, "verbose");

	if (!argc)
		die("Please supply device(s) to check");

	/*
	 * Discover all devices in a multi-device filesystem. When the user
	 * specifies a single device, scan for other members by UUID — same
	 * as mount does.
	 */
	darray_const_str devices = { 0 };
	if (argc == 1) {
		char *sbs = bch2_scan_devices(argv[0]);
		if (sbs) {
			char *p = sbs, *s;
			while ((s = strsep(&p, ":")))
				darray_push(&devices, s);
		} else {
			darray_push(&devices, argv[0]);
		}
	} else {
		for (int i = 0; i < argc; i++)
			darray_push(&devices, argv[i]);
	}

	/*
	 * Honor explicit user-supplied paths, but warn when a path appears to be
	 * a multipath component because that is typically unintended.
	 */
	darray_for_each(devices, dev)
	{
		char *mpath_dev = find_multipath_holder(*dev);
		if (mpath_dev) {
			warn_multipath_component(*dev, mpath_dev);
			free(mpath_dev);
		}
	}

	if (devices.nr == 1 && S_ISDIR(xstat(devices.data[0]).st_mode)) {
		printf("Running fsck online\n");
		struct bchfs_handle fs = bcache_fs_open(devices.data[0]);
		ret = fsck_online(fs, opts_str.buf);
		goto out;
	}

	darray_for_each(devices, dev)
	{
		if (dev_mounted(*dev)) {
			printf("Running fsck online\n");
			int dev_idx;
			struct bchfs_handle fs =
				bchu_fs_open_by_dev(*dev, &dev_idx);
			ret = fsck_online(fs, opts_str.buf);
			goto out;
		}
	}

	if (kernel == 1)
		(void)!system("modprobe bcachefs");

	int kernel_probed = kernel;
	if (kernel_probed < 0)
		kernel_probed = should_use_kernel_fsck(devices);

	if (kernel_probed) {
		darray_str loopdevs = { 0 };
		int fsck_fd = -1;

		printf("Running in-kernel offline fsck\n");
		struct bch_ioctl_fsck_offline *fsck =
			calloc(sizeof(*fsck) + sizeof(u64) * devices.nr, 1);
		fsck->opts = (unsigned long)opts_str.buf;
		fsck->nr_devs = devices.nr;

		darray_for_each(devices, dev)
		{
			if (is_blockdev(*dev)) {
				fsck->devs[dev - devices.data] =
					(unsigned long)*dev;
			} else {
				char *l = loopdev_alloc(*dev);
				if (!l) {
					darray_for_each(loopdevs, i)
						loopdev_free(*i);
					darray_exit(&loopdevs);
					free(fsck);
					if (kernel == 1)
						die("error setting up loop devices");
					goto userland_fsck;
				}
				darray_push(&loopdevs, l);
				fsck->devs[dev - devices.data] =
					(unsigned long)l;
			}
		}

		int ctl_fd = bcachectl_open();
		fsck_fd = ioctl(ctl_fd, BCH_IOCTL_FSCK_OFFLINE, fsck);
		close(ctl_fd);
		free(fsck);

		darray_for_each(loopdevs, i) loopdev_free(*i);
		darray_exit(&loopdevs);

		if (fsck_fd < 0) {
			if (kernel < 0)
				goto userland_fsck;
			die("BCH_IOCTL_FSCK_OFFLINE error: %s",
			    bch2_err_str(errno));
		}

		/* The return code from fsck is returned via close() on this fd */
		ret = splice_fd_to_stdinout(fsck_fd);
	} else {
userland_fsck:
		printf("Running userspace offline fsck\n");
		struct bch_opts opts = bch2_opts_empty();
		struct printbuf err = PRINTBUF;
		if (bch2_parse_mount_opts(NULL, &opts, &err, opts_str.buf,
					  false))
			die("error parsing options: %s", err.buf);
		printbuf_exit(&err);

		struct bch_fs *c = bch2_fs_open(&devices, &opts);
		if (IS_ERR(c))
			die("error opening %s: %s", devices.data[0],
			    bch2_err_str(PTR_ERR(c)));

		struct printbuf fsck_err = PRINTBUF;
		ret = bch2_fs_fsck_errcode(c, &fsck_err);
		if (ret)
			fprintf(stderr, "%s", fsck_err.buf);

		int ret2 = bch2_fs_stop(c);
		if (ret2) {
			fprintf(stderr, "error shutting down filesystem: %s\n",
				bch2_err_str(ret2));
			ret |= 8;
		}
		printbuf_exit(&fsck_err);
	}

out:
	printbuf_exit(&user_opts);
	printbuf_exit(&opts_str);
	darray_exit(&devices);
	return ret;
}
