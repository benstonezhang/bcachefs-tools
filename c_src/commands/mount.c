/*
 * mount - Mount a filesystem.
 *
 * Mount a bcachefs filesystem by its UUID or label. Devices are discovered
 * automatically by scanning for the filesystem UUID or label - unlike btrfs,
 * this is handled entirely in userspace.
 *
 * Use OLD_BLKID_UUID=<uuid> in fstab entries when systemd consumes UUID=<uuid>
 * before the bcachefs mount helper can scan all members.
 *
 * If the filesystem is encrypted, the passphrase will be looked up in the
 * kernel keyring first; if not found, the user is prompted interactively (or
 * reads from stdin if not a terminal). Use -k or --passphrase-file to specify
 * alternative unlock methods.
 *
 * Ported from src/commands/mount.rs.
 *
 * GPLv2
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "cmds.h"
#include "crypto.h"

/*
 * Our own messages, split by level as logging::setup() derives them from -v:
 * warnings and errors are on by default, because they are what a person sees
 * when a boot goes wrong; info and below are a developer, or someone being
 * talked through a problem.
 */
static unsigned g_verbose;

#define mnt_error(fmt, ...)							\
	fprintf(stderr, "mount: " fmt "\n", ##__VA_ARGS__)
#define mnt_warn(fmt, ...)							\
	fprintf(stderr, "mount: warning: " fmt "\n", ##__VA_ARGS__)
#define mnt_info(fmt, ...)							\
do {										\
	if (g_verbose)								\
		fprintf(stderr, "mount: " fmt "\n", ##__VA_ARGS__);		\
} while (0)
#define mnt_debug(fmt, ...)							\
do {										\
	if (g_verbose >= 2)							\
		fprintf(stderr, "mount: " fmt "\n", ##__VA_ARGS__);		\
} while (0)

typedef struct {
	const char *name;
	unsigned long mask;
} mount_flag_opt;

static const mount_flag_opt mount_flag_opts[] = {
	{ "dirsync", MS_DIRSYNC },
	{ "lazytime", 1UL << 25 }, /* MS_LAZYTIME */
	{ "mand", MS_MANDLOCK },
	{ "noatime", MS_NOATIME },
	{ "nodev", MS_NODEV },
	{ "nodiratime", MS_NODIRATIME },
	{ "noexec", MS_NOEXEC },
	{ "nosuid", MS_NOSUID },
	{ "relatime", MS_RELATIME },
	{ "remount", MS_REMOUNT },
	{ "ro", MS_RDONLY },
	{ "rw", 0 },
	{ "strictatime", MS_STRICTATIME },
	{ "sync", MS_SYNCHRONOUS },
	/* Userspace-only fstab options — not passed to the kernel */
	{ "auto", 0 },
	{ "noauto", 0 },
	{ "nofail", 0 },
	{ "_netdev", 0 },
	{ "user", 0 },
	{ "nouser", 0 },
	{ "users", 0 },
	{ "group", 0 },
	{ "owner", 0 },
};

/*
 * A mount that didn't happen, and both halves of what the kernel said about
 * it.
 *
 * @code is the half we can act on. bch2_fs_get_tree() hands back bcachefs's
 * own error codes rather than flattening them to an errno, so
 * device_splitbrain and the two missing-device refusals arrive as distinct
 * values instead of three EINVALs that could only be told apart by reading
 * their prose. bch2_degraded_ask_put() decides what to ask on that code alone.
 *
 * @text is the half for the user: whatever the filesystem logged against the
 * context, which is the whole reason for mounting through fs_context.
 * Keeping both means neither has to serve as the other.
 */
struct mount_error {
	int	code;
	char	*text;
};

static void mount_error_free(struct mount_error *e)
{
	free(e->text);
	e->text = NULL;
	e->code = 0;
}

/* Take ownership of @text, which the caller formatted. */
static void mount_error_take(struct mount_error *e, int code, char *text)
{
	free(e->text);
	e->code = code;
	e->text = text;
}

/*
 * Show warnings and notices; return the error-level messages so the caller
 * can fold them into its error, where they'll be reported exactly once.
 *
 * Takes ownership of @msgs - they are consumed exactly once either way.
 */
static char *report_log(struct bch_fs_context_msg *msgs)
{
	struct printbuf errors = PRINTBUF;
	unsigned i;

	for (i = 0; msgs && msgs[i].text; i++)
		switch (msgs[i].level) {
		case BCH_FS_CONTEXT_ERROR:
			if (errors.pos)
				prt_str(&errors, "; ");
			prt_str(&errors, msgs[i].text);
			break;
		case BCH_FS_CONTEXT_WARNING:
			mnt_warn("%s", msgs[i].text);
			break;
		case BCH_FS_CONTEXT_NOTICE:
			mnt_info("%s", msgs[i].text);
			break;
		}

	if (msgs)
		bch_fs_context_msgs_free(msgs);

	if (!errors.pos) {
		printbuf_exit(&errors);
		return NULL;
	}

	/* Grabbing the buffer instead of printbuf_exit() is the documented
	 * way to hand a heap string to the caller. */
	return errors.buf;
}

/*
 * @what is what we were attempting, @code what the kernel returned, and
 * @msgs what it had to say first - which supersedes @code in the text, since
 * strerror() on a bcachefs errcode is "Unknown error 2300".
 */
static void mount_error_new(struct mount_error *e, const char *what, int code,
			    struct bch_fs_context_msg *msgs)
{
	char *errors = report_log(msgs);

	free(e->text);
	e->code = code;
	e->text = errors ? mprintf("%s: %s", what, errors)
			 : mprintf("%s: %s", what, bch2_err_str(code));
	free(errors);
}

/*
 * Carry the mount's side of the conversation while it's coming up: the
 * filesystem to stderr, stdin back to the filesystem, and recovery progress
 * drawn below both.
 *
 * Detached. This thread is inside fsconfig(2) until the mount is done, so
 * there's no point in the sequence where joining would be right - and the
 * kernel marks the channel done on every path out, which ends the relay.
 *
 * Whether we draw decides whether we poll at all: the kernel stops logging
 * progress to dmesg as soon as anything reads BCH_IOCTL_RECOVERY_STATUS, on
 * the grounds that whoever read it is showing it. Polling from somewhere with
 * nowhere to draw would take progress out of both places at once - so the
 * display decides, in bch2_recovery_display_new(), and a NULL display here
 * means leaving it to dmesg.
 */
struct status_relay_arg {
	int			 fd;
	struct bch_status_display	*display;
};

static void *status_relay_thread(void *argp)
{
	struct status_relay_arg *a = argp;
	int ret = bch_thread_relay(a->fd, STDERR_FILENO, a->display);

	if (ret)
		mnt_debug("status channel: %s", bch2_err_str(ret));

	bch2_recovery_display_free(a->display);
	close(a->fd);
	free(a);
	return NULL;
}

static void spawn_status_relay(int fd, const char *source, const dev_names *devs)
{
	struct status_relay_arg *a = xmalloc(sizeof(*a));
	pthread_t thread;

	a->fd		= fd;
	a->display	= bch2_recovery_display_new(fd, source, devs);

	if (pthread_create(&thread, NULL, status_relay_thread, a))
		die("could not start the status relay: %m");

	pthread_detach(thread);
}

/* What came of an attempt on the fs_context path. */
enum mounted {
	/* Mounted. */
	MOUNTED_YES,
	/*
	 * The fs_context path can't carry this mount; the caller should use
	 * mount(2). Either the kernel has no fsopen(2), or the mount says
	 * something fsconfig(2) has no room for.
	 */
	MOUNTED_USE_LEGACY,
	/*
	 * The device is write-protected and a read-write mount wasn't
	 * explicitly asked for. mount(8) retries these read-only rather than
	 * failing.
	 */
	MOUNTED_WRITE_PROTECTED,
};

/*
 * Mount via the fs_context API, so that whatever bcachefs has to say about a
 * failure reaches the user instead of being flattened to an errno.
 */
static int mount_fs_context(const char *src, const char *target,
			    const char *fstype, unsigned long mountflags,
			    const char *data, const dev_names *devs,
			    const char *user_key, enum mounted *out,
			    struct mount_error *err)
{
	const char *params[8];
	char *copy = NULL, *p, *dev, *opt, *eq;
	char *fail_what = NULL;
	unsigned nparams, i;
	int fc, sfd, ret;
	int fail_code = 0;

	fc = bch_fs_context_open(fstype);
	if (fc == -ENOSYS) {
		mnt_debug("no fs_context mount API, falling back to mount(2)");
		*out = MOUNTED_USE_LEGACY;
		return 0;
	}
	if (fc < 0) {
		char *what = mprintf("fsopen(%s)", fstype);

		mount_error_new(err, what, fc, NULL);
		free(what);
		return -1;
	}

	/*
	 * The kernel's account of a failure is the entire reason for being
	 * here, so it becomes the error rather than being printed alongside
	 * one. Both outcomes are logged: a working channel with nothing to
	 * say looks exactly like a kernel that never offered one.
	 *
	 * Jumps rather than returns: @copy is still the caller's to free on
	 * every path out.
	 */
#define FC_FAIL(_code, ...)							\
do {										\
	fail_code = (_code);							\
	fail_what = mprintf(__VA_ARGS__);					\
	goto fail;								\
} while (0)

	/*
	 * One device per parameter. fsconfig(2) copies a value with
	 * strndup_user(_value, 256), which returns -EINVAL rather than
	 * truncating, so the whole colon-separated list doesn't fit past ~25
	 * devices - and that -EINVAL arrives with an empty fs_context log,
	 * since it happens in the syscall wrapper before the VFS sees the
	 * parameter. bch2_fs_parse_param() splits each "source" value on ':'
	 * and appends, so there is no ceiling on the list, and a device the
	 * kernel refuses is named on its own.
	 */
	copy = xstrdup(src);
	p = copy;
	while ((dev = strsep(&p, ":"))) {
		/*
		 * A path that doesn't fit can't be said in this API at all,
		 * and PATH_MAX is 4096. That alone still needs mount(2),
		 * whose copy_mount_string() allows the full length.
		 */
		if (strlen(dev) > BCH_FS_CONTEXT_PARAM_VALUE_MAX) {
			mnt_debug("device path is %zu bytes, over fsconfig()'s %d; using mount(2)",
				  strlen(dev), BCH_FS_CONTEXT_PARAM_VALUE_MAX);
			free(copy);
			close(fc);
			*out = MOUNTED_USE_LEGACY;
			return 0;
		}

		ret = bch_fs_context_set(fc, "source", dev);
		if (ret)
			FC_FAIL(ret, "source %s", dev);
	}
	free(copy);
	copy = NULL;

	nparams = bch_fs_context_sb_flag_params(mountflags, params);
	for (i = 0; i < nparams; i++) {
		ret = bch_fs_context_set(fc, params[i], NULL);
		if (ret)
			FC_FAIL(ret, "option %s", params[i]);
	}

	/*
	 * One at a time, so a rejected option is named in the error.
	 *
	 * A non-UTF-8 device path never gets here - the caller only takes the
	 * fs_context path for a path that is valid C string, which is all
	 * strndup_user() needs.
	 */
	if (data) {
		copy = xstrdup(data);
		p = copy;
		while ((opt = strsep(&p, ","))) {
			const char *value = NULL;

			if (!*opt)
				continue;

			eq = strchr(opt, '=');
			if (eq) {
				*eq = '\0';
				value = eq + 1;
			}

			ret = bch_fs_context_set(fc, opt, value);
			if (ret)
				FC_FAIL(ret, "option %s", opt);
		}
		free(copy);
		copy = NULL;
	}

	/*
	 * Not an option, and deliberately not in the option string: the key
	 * itself, handed to the kernel so it does not have to go looking in a
	 * keyring. fsconfig(2) takes it from our address space, so it never
	 * reaches anyone's ps output the way an -o would.
	 */
	if (user_key) {
		ret = bch_fs_context_set(fc, "user_key", user_key);
		if (ret)
			FC_FAIL(ret, "user_key");
	}

	/*
	 * Before creating the superblock: that's the call that blocks for the
	 * whole of recovery, which is what there is to report on.
	 */
	sfd = bch_fs_context_status_fd(fc);
	if (sfd > 0) {
		mnt_info("status channel on fd %d", sfd);
		spawn_status_relay(sfd, src, devs);
	} else {
		mnt_info("no status channel from this kernel; mounting without one");
	}

	mnt_info("creating superblock");
	ret = bch_fs_context_create(fc);
	if (ret) {
		/*
		 * Write-protected device: retry read-only, as mount(8) does.
		 * Say nothing yet - the retry produces the real outcome, and
		 * this context's log dies with it.
		 */
		if ((ret == -EACCES || ret == -EROFS) && !(mountflags & MS_RDONLY)) {
			close(fc);
			*out = MOUNTED_WRITE_PROTECTED;
			return 0;
		}

		FC_FAIL(ret, "mount");
	}

	/* A mount can succeed and still have things to say - degraded,
	 * recovery notes - so drain the log here too. */
	report_log(bch_fs_context_drain_log(fc));

	ret = bch_fs_context_fsmount(fc, bch_fs_context_mount_attrs(mountflags));
	if (ret < 0)
		FC_FAIL(ret, "fsmount");

	{
		int mnt = ret;

		ret = bch_fs_context_move_mount(mnt, target);
		close(mnt);
		if (ret) {
			char *what = mprintf("attaching to %s", target);

			mount_error_new(err, what, ret, NULL);
			free(what);
			close(fc);
			return -1;
		}
	}

	close(fc);
	*out = MOUNTED_YES;
	return 0;

fail:
	free(copy);
	mount_error_new(err, fail_what, fail_code, bch_fs_context_drain_log(fc));
	free(fail_what);
	close(fc);
	return -1;
#undef FC_FAIL
}

/* kfree is what darray_str's free item is; this just spells it out. */
static void darray_str_exit(darray_str *d)
{
	for (char **i = d->data; i < d->data + d->nr; i++)
		free(*i);
	darray_exit(d);
}

/*
 * The kernel can say why an ioctl failed, in the buffer it was handed - the
 * half of the message the errno doesn't carry.
 */
static void print_ioctl_err(const char *buf, size_t len)
{
	size_t n = strnlen(buf, len);

	if (n)
		fprintf(stderr, "ioctl error: %s\n", buf);
}

/*
 * Bring in members that turned up while we were deciding to mount.
 *
 * There is a window nothing else covers. scan_waiting_for_devices() drops its
 * udev monitor when it returns, and the udev rule won't act until
 * /sys/fs/bcachefs/<uuid> exists, which is bch2_fs_online() partway through
 * the mount. Everything between is nobody's: too late for one, too early for
 * the other.
 *
 * That window contains the degraded prompt, so it can be a minute long - and
 * it is exactly when a slow disk is most likely to finish coming up, since we
 * have just spent missing_dev_timeout waiting for it and given up.
 *
 * So look once more, now that the mount is done. This tests state rather than
 * waiting for an event, which is what lets it overlap the udev rule instead of
 * abutting it: by the time this runs the rule is already live, both may try,
 * and __bch2_dev_attach_bdev() turns the loser away with
 * device_already_online rather than doing anything to it.
 *
 * Best effort. The filesystem is mounted; failing to pick up a straggler is
 * worth saying out loud but is not a reason to fail the mount.
 */
static void online_late_devices(uuid_t uuid, const bool present[256],
				struct bch_opts *opts)
{
	bch_scanned_sbs found = { 0 };
	darray_str late = { 0 };
	bool use_udev = opt_get(*opts, mount_trusts_udev) != 0;
	char uuid_str[37];
	struct bchfs_handle fs;
	unsigned i;
	int ret;

	ret = bch2_get_devices_by_uuid(uuid, opts, use_udev, &found);
	if (ret) {
		mnt_debug("rescanning for late devices: %s", bch2_err_str(ret));
		bch2_scanned_sbs_exit(&found);
		return;
	}

	for (struct bch_scanned_sb *s = found.data; s < found.data + found.nr; s++)
		if (!present[s->sb.sb->dev_idx])
			darray_push(&late, xstrdup(s->path));

	if (!late.nr)
		goto out;

	uuid_unparse_lower(uuid, uuid_str);

	ret = bcache_fs_open_fallible(uuid_str, &fs);
	if (ret) {
		mnt_warn("%u device(s) turned up after mounting, but opening the filesystem to bring them online failed: %s",
			 (unsigned)late.nr, bch2_err_str(ret));
		goto out;
	}

	for (i = 0; i < late.nr; i++) {
		char err_buf[8192];
		struct bch_ioctl_disk_v2 v2 = {
			.dev	= (unsigned long)late.data[i],
			.err	= { .msg_ptr = (unsigned long)err_buf,
				    .msg_len = sizeof(err_buf) },
		};
		struct bch_ioctl_disk v1 = { .dev = (unsigned long)late.data[i] };

		memset(err_buf, 0, sizeof(err_buf));

		ret = ioctl(fs.ioctl_fd, BCH_IOCTL_DISK_ONLINE_v2, &v2);
		if (ret < 0) {
			int e = errno;

			if (e == ENOTTY) {
				ret = ioctl(fs.ioctl_fd, BCH_IOCTL_DISK_ONLINE,
					    &v1);
				e = errno;
			} else {
				print_ioctl_err(err_buf, sizeof(err_buf));
			}
			ret = ret < 0 ? -e : 0;
		} else {
			ret = 0;
		}

		/*
		 * Includes the benign case where the udev rule got there
		 * first; the ioctl reports device_already_online as a plain
		 * EINVAL, so we cannot tell that apart from a real refusal
		 * here. The kernel log has the reason either way.
		 */
		if (!ret)
			mnt_warn("%s turned up after we mounted; brought it online",
				 late.data[i]);
		else
			mnt_warn("%s turned up after we mounted, but bringing it online failed (%s); it may already be online",
				 late.data[i], bch2_err_str(ret));
	}

	bcache_fs_close(fs);
out:
	darray_str_exit(&late);
	bch2_scanned_sbs_exit(&found);
}

static int mount_legacy(const char *src, const char *target, const char *fstype,
			unsigned long mountflags, const char *data,
			struct mount_error *err);

static int mount_inner(const char *src, const char *target, const char *fstype,
		       unsigned long mountflags, const char *data,
		       const dev_names *devs, const struct bch_unlocked *unlocked,
		       struct mount_error *err)
{
	/*
	 * Only the fs_context path can carry the key; mount(2) has nowhere to
	 * put a parameter, so that one still goes through a keyring.
	 */
	char *user_key = unlocked ? bch2_unlocked_hex(unlocked) : NULL;
	int ret;

	if (!(mountflags & MS_REMOUNT)) {
		unsigned long flags = mountflags;

		for (;;) {
			enum mounted m;

			mnt_info("mounting filesystem");
			ret = mount_fs_context(src, target, fstype, flags, data,
					       devs, user_key, &m, err);
			if (ret)
				goto out;

			if (m == MOUNTED_YES) {
				ret = 0;
				goto out;
			}
			if (m == MOUNTED_WRITE_PROTECTED) {
				printf("mount: device write-protected, mounting read-only\n");
				flags |= MS_RDONLY;
				continue;
			}
			break;
		}
	}

	/*
	 * mount(2) from here: no parameters, so a key we are holding has to go
	 * into a keyring after all, for bch2_request_key() to find. Failing to
	 * place it is not fatal here - the mount below will say ENOKEY, which
	 * is the better error to report.
	 */
	if (unlocked) {
		int r = bch2_unlocked_to_keyring(unlocked);

		if (r < 0)
			mnt_warn("mount(2) has no way to carry the key, and putting it in a keyring failed: %s",
				 bch2_err_str(r));
	}

	ret = mount_legacy(src, target, fstype, mountflags, data, err);
out:
	free(user_key);
	return ret;
}

static int mount_legacy(const char *src, const char *target, const char *fstype,
			unsigned long mountflags, const char *data,
			struct mount_error *err)
{
	int ret, errval;

	for (;;) {
		mnt_info("mounting filesystem");
		/* REQUIRES: CAP_SYS_ADMIN */
		ret = mount(src, target, fstype, mountflags, data);
		errval = errno;

		if (!ret || (errval != EACCES && errval != EROFS) ||
		    (mountflags & MS_RDONLY))
			break;

		printf("mount: device write-protected, mounting read-only\n");
		mountflags |= MS_RDONLY;
	}

	if (ret) {
		/*
		 * mount(2) has nowhere to put an explanation, so an errno is
		 * all there is to report - which is the reason
		 * mount_fs_context() exists. EBUSY is worth translating: what
		 * it means here isn't obvious from the word.
		 */
		free(err->text);
		err->code = -errval;
		err->text = errval == EBUSY
			? mprintf("%s: %s already mounted or mount point busy",
				  target, src)
			: mprintf("%s: %s", src, strerror(errval));
		return -1;
	}

	return 0;
}

/*
 * A comma-separated mount option string split into its consumers.
 *
 * The same option vocabulary feeds two places - the mount(2) syscall
 * (`flags`) and the filesystem itself (`fs_opts`, handed to
 * bch2_parse_mount_opts later) - so it's tabulated once in mount_flag_opts
 * rather than re-derived per caller.
 */
static unsigned long parse_mountflag_options(const char *options,
					     char **fs_opts)
{
	struct printbuf buf = PRINTBUF;
	unsigned long flags = 0;
	char *copy, *p, *opt;

	mnt_debug("parsing mount options: %s", options ?: "");

	copy = xstrdup(options ?: "");
	p = copy;
	while ((opt = strsep(&p, ","))) {
		unsigned i;

		for (i = 0; i < ARRAY_SIZE(mount_flag_opts); i++)
			if (!strcmp(opt, mount_flag_opts[i].name))
				break;

		if (i < ARRAY_SIZE(mount_flag_opts)) {
			flags |= mount_flag_opts[i].mask;
			continue;
		}

		/* An empty token, and "rw": no flag, and nothing to pass on. */
		if (!*opt)
			continue;

		/* A configurer's own: nobody here, and not in the kernel. */
		if (!strncmp(opt, "x-", 2) || !strncmp(opt, "comment=", 8))
			continue;

		if (buf.pos)
			prt_char(&buf, ',');
		prt_str(&buf, opt);
	}
	free(copy);

	if (buf.pos) {
		*fs_opts = buf.buf;
	} else {
		printbuf_exit(&buf);
		*fs_opts = NULL;
	}
	return flags;
}

/*
 * `bcachefs mount`'s own options - Rust's `Cli` in src/commands/mount.rs.
 *
 * Defined here rather than beside cmd_mount(), which is where Rust puts it,
 * because every helper above takes it as a parameter and C cannot name a
 * struct it hasn't seen yet.
 */
struct mount_cli {
	/* Path to passphrase file */
	char			*passphrase_file;

	/*
	 * Passphrase policy, if one was asked for. Not the same as
	 * BCH_UNLOCK_POLICY_ask: unspecified means search the keyring, and
	 * only prompt if the key isn't there - which is what handle_unlock()
	 * does when this is not set.
	 */
	enum bch_unlock_policy	policy;
	bool			policy_explicit;

	/* Device, UUID=<UUID>, OLD_BLKID_UUID=<UUID> (fstab), or LABEL=<label> */
	char			*dev;

	/*
	 * Where the filesystem should be mounted. NULL means it won't be - but
	 * everything preceding mounting (asking for a passphrase) still
	 * happens.
	 */
	char			*mountpoint;

	/* Mount options */
	char			*options;

	/* Do not update /etc/mtab; accepted for mount(8) compatibility */
	bool			no_mtab;

	/* Fake mount: do everything except the mount syscall (mount(8) -f) */
	bool			fake;

	/*
	 * Ignore unrecognized mount options instead of failing
	 * (mount(8) -s). bcachefs already ignores unknown options, so this is
	 * accepted as a no-op.
	 */
	bool			sloppy;

	/* Only bcachefs.fuse changes anything this decides */
	const char		*fs_type;
};

/*
 * If a user explicitly specifies policy or passphrase file then use that
 * without falling back to other mechanisms. If these options are not used,
 * then search for the key or ask for it.
 */
static int handle_unlock(const struct mount_cli *cli, struct bch_sb_handle *sb,
			 struct bch_unlocked *unlocked, struct mount_error *err)
{
	struct bch_passphrase_correct correct;
	struct bch_unlock_socket *socket;
	int ret;

	if (cli->policy_explicit)
		return bch2_unlock_policy_apply(cli->policy, sb, unlocked);

	if (cli->passphrase_file) {
		ret = bch2_passphrase_read_from_file(cli->passphrase_file,
						     sb->sb, &correct);
		if (ret) {
			/*
			 * A file that couldn't be read is not a passphrase that
			 * was wrong, and saying so sends whoever hit it looking
			 * for the wrong thing entirely.
			 */
			mount_error_take(err, ret,
					 ret == -EINVAL
					 ? xstrdup("incorrect passphrase")
					 : xstrdup(strerror(-ret)));
			return -1;
		}

		unlocked->have_key = true;
		unlocked->correct = correct;
		return 0;
	}

	/*
	 * Somebody unlocked it already - `bcachefs unlock`, or an agent. The
	 * bytes are theirs and we never see them; the kernel asks the keyring
	 * itself.
	 */
	if (bch2_key_search(sb->sb)) {
		unlocked->have_key = false;
		return 0;
	}

	/*
	 * A second way to answer, up for as long as the prompt is: whoever is
	 * booting the machine may not be at it. Mounting is the only thing
	 * that wants one - `unlock` and `set-passphrase` are already somebody
	 * at a shell.
	 */
	socket = bch2_unlock_socket_open(sb);
	ret = bch2_passphrase_ask_and_check(sb, socket, &correct);
	bch2_unlock_socket_free(socket);

	if (ret) {
		mount_error_take(err, ret, xstrdup("incorrect passphrase"));
		return -1;
	}

	unlocked->have_key = true;
	unlocked->correct = correct;
	return 0;
}

/*
 * Whether @err is a splitbrain refusal.
 *
 * Failing to recognise it is silent - the mount just refuses, exactly as it
 * does when there is no split brain. The code comes from our own scan, so it
 * is one this build knows.
 */
static bool is_splitbrain(int err)
{
	return bch2_err_matches(err, BCH_ERR_device_splitbrain);
}

/*
 * Scan for the filesystem's devices, and if its history has forked, ask.
 *
 * The check runs deep in the scan - it has to, before bch2_sbs_filter_dead()
 * frees the divergent superblocks - but the *decision* cannot, because the
 * scan re-runs on every device arrival while waiting for members: a question
 * there would be asked repeatedly and the answer could not persist. So the
 * scan reports and refuses, and the question is put here, once.
 *
 * Consenting leaves the diverged devices out of the mount, which is the
 * missing-device case, so it answers the degraded question too.
 */
static int scan_or_ask_splitbrain(const char *device, struct bch_opts *opts,
				  bch_scanned_sbs *out, struct mount_error *err)
{
	bch_scanned_sbs sbs = { 0 };
	int ret, code;

	ret = bch2_scan_sbs_for_mount(device, opts, &sbs);
	if (!ret) {
		*out = sbs;
		return 0;
	}

	code = ret;
	if (!is_splitbrain(code)) {
		bch2_scanned_sbs_exit(&sbs);
		mount_error_take(err, code, xstrdup(bch2_err_str(code)));
		return -1;
	}

	/*
	 * The report is already out - the scan logged it on the way to
	 * failing. Re-scanning with the check off gives us the surviving side,
	 * which is both what to name in the question and what to mount if
	 * they say yes. Not the waiting scan: the one above already waited,
	 * and everything it is about to find is what that wait turned up.
	 */
	bch2_scanned_sbs_exit(&sbs);
	sbs = (bch_scanned_sbs){ 0 };

	opt_set(*opts, no_splitbrain_check, 1);
	ret = bch2_scan_sbs(device, opts, &sbs);
	if (ret) {
		bch2_scanned_sbs_exit(&sbs);
		mount_error_take(err, ret, xstrdup(bch2_err_str(ret)));
		return -1;
	}

	if (!sbs.nr) {
		bch2_scanned_sbs_exit(&sbs);
		mount_error_take(err, code, xstrdup(bch2_err_str(code)));
		return -1;
	}

	if (!bch2_splitbrain_ask(&sbs.data[0].sb)) {
		bch2_scanned_sbs_exit(&sbs);
		mount_error_take(err, code, xstrdup(bch2_err_str(code)));
		return -1;
	}

	opt_set(*opts, degraded, BCH_DEGRADED_yes);
	*out = sbs;
	return 0;
}

/*
 * What one mount attempt needs, taken from a scan.
 *
 * It holds no bch_sb_handle: those are open block devices, and the kernel
 * opens the members itself. So the scan is consumed here and everything an
 * attempt - or the question that follows a refused one - could want is copied
 * out first.
 */
struct attempt {
	/* Devices as one colon-separated list. */
	char			*devices;
	dev_names		 devinfo;
	uuid_t			 uuid;
	/*
	 * Members we found, by dev_idx - a member found twice (multipath, or
	 * udev and the block scan both contributing) counts once.
	 */
	bool			 present[256];
	unsigned		 nr_present;
	/* Short of the full set means someone may yet turn up. */
	bool			 short_members;
	/*
	 * The degraded question, if it is ours to put. Taken once it is
	 * answered: there is only one question here, and it is asked at most
	 * once.
	 */
	struct bch_degraded_ask	*ask;
};

/* Takes ownership of @sbs, which it closes before returning. */
static void attempt_new(struct attempt *a, bch_scanned_sbs *sbs,
			const struct bch_opts *opts)
{
	memset(a, 0, sizeof(*a));

	a->devices = bch2_scanned_joined_device_str(sbs);
	bch2_devices_from_superblocks(sbs, &a->devinfo);
	uuid_copy(a->uuid, sbs->data[0].sb.sb->user_uuid.b);

	for (struct bch_scanned_sb *s = sbs->data; s < sbs->data + sbs->nr; s++) {
		u8 dev_idx = s->sb.sb->dev_idx;

		if (!a->present[dev_idx]) {
			a->present[dev_idx] = true;
			a->nr_present++;
		}
	}

	a->short_members = a->nr_present < bch2_scanned_expected_devices(sbs);
	a->ask = bch2_degraded_ask_new(sbs, opts);

	bch2_scanned_sbs_exit(sbs);
}

static void attempt_free(struct attempt *a)
{
	free(a->devices);
	dev_names_free(&a->devinfo);
	if (a->ask)
		bch2_degraded_ask_free(a->ask);
	memset(a, 0, sizeof(*a));
}

/*
 * Mount, and put the degraded question if the kernel refuses for want of a
 * device.
 *
 * A loop rather than a call and a retry, because one of the answers is "the
 * disk just turned up" - and then there is a new device list to mount with,
 * not just a new option. Each pass either mounts, refuses for good, or comes
 * back with one of those two things changed.
 */
static int mount_asking_about_degraded(const struct mount_cli *cli,
				       const char *mountpoint,
				       bch_scanned_sbs *sbs,
				       struct bch_opts *opts,
				       unsigned long parsed_flags,
				       const char *parsed_fs_opts,
				       const struct bch_unlocked *unlocked,
				       struct mount_error *err)
{
	unsigned long flags = parsed_flags;
	char *fs_opts = xstrdup(parsed_fs_opts ?: "");
	struct attempt attempt, *a = &attempt;
	int ret = -1, pret;

	attempt_new(a, sbs, opts);

	for (;;) {
		struct bch_degraded_outcome outcome = { 0 };

		mnt_info("mounting with params: device: %s, target: %s, options: %s",
			 a->devices, mountpoint, cli->options ?: "");

		if (!mount_inner(a->devices, mountpoint, "bcachefs", flags,
				 fs_opts, &a->devinfo, unlocked, err)) {
			/*
			 * Mounted, but we left members behind: one of them may
			 * have shown up while we were asking about it.
			 */
			if (a->short_members)
				online_late_devices(a->uuid, a->present, opts);
			ret = 0;
			goto out;
		}

		/* Nothing here for us to decide: refuse as we were refused. */
		if (!a->ask)
			goto out;

		pret = bch2_degraded_ask_put(a->ask, err->code, &outcome);
		if (pret < 0) {
			mount_error_take(err, pret,
					 mprintf("could not ask about mounting degraded: %s",
						 bch2_err_str(pret)));
			goto out;
		}

		switch (outcome.kind) {
		case BCH_DEGRADED_MOUNT: {
			char *joined = bch2_degraded_append_opt(fs_opts,
								outcome.fs_opt);

			free(fs_opts);
			fs_opts = joined;

			/*
			 * Read-only is a mount flag, not a degraded= value,
			 * and it has to be read-only to the VFS - or
			 * /proc/mounts says rw and the user has only our word
			 * for it.
			 */
			if (outcome.read_only)
				flags |= MS_RDONLY;

			bch2_degraded_ask_free(a->ask);
			a->ask = NULL;
			break;
		}
		/* Everything is here now, so this scan does not wait. */
		case BCH_DEGRADED_RESCAN: {
			bch_scanned_sbs rescanned = { 0 };

			if (scan_or_ask_splitbrain(cli->dev, opts, &rescanned, err))
				goto out;

			if (!rescanned.nr) {
				bch2_scanned_sbs_exit(&rescanned);
				mount_error_take(err, -ENOENT,
						 mprintf("%s vanished while we were asking about it",
							 cli->dev));
				goto out;
			}

			attempt_free(a);
			attempt_new(a, &rescanned, opts);
			break;
		}
		case BCH_DEGRADED_NO:
			goto out;
		}
	}
out:
	attempt_free(a);
	free(fs_opts);
	return ret;
}

static int cmd_mount_inner(const struct mount_cli *cli, struct mount_error *err)
{
	bch_scanned_sbs sbs = { 0 };
	struct bch_sb_handle *first;
	struct bch_unlocked unlocked = { 0 };
	struct bch_opts opts = bch2_opts_empty();
	char *fs_opts = NULL, *devices = NULL;
	bool have_unlocked = false;
	unsigned long flags;
	int ret;

	if (cli->no_mtab)
		mnt_debug("ignoring -n/--no-mtab; mount.bcachefs does not update /etc/mtab");
	if (cli->sloppy)
		mnt_debug("ignoring -s/--sloppy; bcachefs already ignores unrecognized options");

	flags = parse_mountflag_options(cli->options, &fs_opts);

	if (bch2_parse_mount_opts(NULL, &opts, NULL, fs_opts, true))
		opts = bch2_opts_empty();

	ret = scan_or_ask_splitbrain(cli->dev, &opts, &sbs, err);
	if (ret) {
		free(fs_opts);
		return -1;
	}

	if (!sbs.nr) {
		mount_error_take(err, -ENOENT,
				 xstrdup("No device(s) to mount specified"));
		bch2_scanned_sbs_exit(&sbs);
		free(fs_opts);
		return -1;
	}

	devices = bch2_scanned_joined_device_str(&sbs);
	first = &sbs.data[0].sb;

	if (bch2_sb_is_encrypted(first->sb)) {
		if (handle_unlock(cli, first, &unlocked, err)) {
			bch2_scanned_sbs_exit(&sbs);
			free(devices);
			free(fs_opts);
			return -1;
		}
		have_unlocked = true;
	}

	if (cli->mountpoint) {
		if (cli->fake) {
			mnt_info("fake mount (-f/--fake): skipping the mount syscall for %s",
				 cli->mountpoint);
		} else {
			/*
			 * After the -f check, and inside this branch, on
			 * purpose: asking someone whether to mount without a
			 * device is only worth their time if we are going to
			 * mount. -f exists to not do the thing, and an
			 * invocation with no mountpoint isn't mounting either.
			 */
			ret = mount_asking_about_degraded(cli, cli->mountpoint,
							  &sbs, &opts, flags,
							  fs_opts,
							  have_unlocked ? &unlocked
									 : NULL,
							  err);
			free(devices);
			free(fs_opts);
			return ret;
		}
	} else {
		mnt_info("would mount with params: device: %s, options: %s",
			 devices, cli->options ?: "");
	}

	bch2_scanned_sbs_exit(&sbs);
	free(devices);
	free(fs_opts);
	return 0;
}

struct module_check {
	bool loaded;
	char *modprobe_error;
};

static struct module_check check_bcachefs_module(void)
{
	struct module_check r = { .loaded = false, .modprobe_error = NULL };
	struct stat st;

	if (!stat("/sys/module/bcachefs", &st)) {
		r.loaded = true;
		return r;
	}

	int status = system("modprobe bcachefs");
	if (status == -1)
		r.modprobe_error = mprintf("could not run modprobe bcachefs: %s",
					   strerror(errno));
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		r.modprobe_error =
			xstrdup("modprobe bcachefs exited unsuccessfully");

	r.loaded = !stat("/sys/module/bcachefs", &st);
	return r;
}

/*
 * `fuse` among the mount options: mount through the FUSE daemon instead of
 * the kernel, as `-t bcachefs.fuse` does, for callers that can only pass
 * options - an fstab line, or a test suite mounting `-t bcachefs` with its
 * own options appended. Consumed here; the filesystem never sees it.
 *
 * Returns whether to mount through FUSE, and the remaining options in @rest
 * (caller frees).
 */
static bool take_fuse_option(const char *options, char **rest)
{
	struct printbuf buf = PRINTBUF;
	char *copy = options ? xstrdup(options) : NULL, *p = copy, *tok;
	bool fuse = false;

	while ((tok = strsep(&p, ","))) {
		if (!strcmp(tok, "fuse")) {
			fuse = true;
		} else if (*tok) {
			if (buf.pos)
				prt_char(&buf, ',');
			prt_str(&buf, tok);
		}
	}

	free(copy);

	/* Always a string the caller can index into and free. */
	if (buf.pos) {
		*rest = buf.buf;
	} else {
		printbuf_exit(&buf);
		*rest = xstrdup("");
	}
	return fuse;
}

static void mount_usage(void)
{
	puts("bcachefs mount - mount a bcachefs filesystem\n"
	     "Usage: bcachefs mount [OPTION]... <device|UUID=|LABEL=> [mountpoint]\n"
	     "\n"
	     "Options:\n"
	     "  -o options             Mount options (comma separated)\n"
	     "  -t, --type=type        Filesystem type (e.g. bcachefs, bcachefs.fuse)\n"
	     "      --passphrase_file=path\n"
	     "                         Read passphrase from file\n"
	     "  -k, --key_location=(fail|wait|ask|stdin)\n"
	     "                         Passphrase policy (default: ask)\n"
	     "  -f, --fake             Do everything except the mount syscall\n"
	     "  -n, --no-mtab          Accepted for mount(8) compatibility (no-op)\n"
	     "  -s, --sloppy           Accepted for mount(8) compatibility (no-op)\n"
	     "  -c, --colorize=bool    Force color on/off (accepted; no-op)\n"
	     "  -v, --verbose          Verbose output\n"
	     "  -h, --help             Display this help and exit\n"
	     "\n"
	     "Device may be a path, UUID=<uuid>, OLD_BLKID_UUID=<uuid>, or LABEL=<label>.\n"
	     "\n"
	     "Mount options include both standard filesystem independent options (see\n"
	     "mount(8)) and bcachefs specific options:");

	bch2_opts_usage(OPT_MOUNT, 0);
}

int cmd_mount(int argc, char *argv[])
{
	struct mount_error err = { 0 };
	struct mount_cli cli = {
		.policy		= BCH_UNLOCK_POLICY_ask,
		.fs_type	= "bcachefs",
	};
	struct module_check module;
	char *fuse_options = NULL;
	/* The name we were invoked as - cmd_fusemount() needs it as argv[0] */
	char *prog __maybe_unused = argv[0];
	bool via_fuse;
	int opt, ret = 0;

	enum {
		OPT_PASSPHRASE_FILE = 1000,
	};
	static const struct option longopts[] = {
		{ "options", required_argument, NULL, 'o' },
		{ "type", required_argument, NULL, 't' },
		{ "passphrase_file", required_argument, NULL,
		  OPT_PASSPHRASE_FILE },
		{ "key_location", required_argument, NULL, 'k' },
		{ "fake", no_argument, NULL, 'f' },
		{ "no-mtab", no_argument, NULL, 'n' },
		{ "sloppy", no_argument, NULL, 's' },
		{ "colorize", required_argument, NULL, 'c' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};

	while ((opt = getopt_long(argc, argv, "o:t:c:k:fnsvh", longopts,
				  NULL)) != -1)
		switch (opt) {
		case 'o':
			cli.options = optarg;
			break;
		case 't':
			cli.fs_type = optarg;
			break;
		case OPT_PASSPHRASE_FILE:
			cli.passphrase_file = optarg;
			break;
		case 'k':
			if (!strcmp(optarg, "fail"))
				cli.policy = BCH_UNLOCK_POLICY_fail;
			else if (!strcmp(optarg, "wait"))
				cli.policy = BCH_UNLOCK_POLICY_wait;
			else if (!strcmp(optarg, "ask"))
				cli.policy = BCH_UNLOCK_POLICY_ask;
			else if (!strcmp(optarg, "stdin"))
				cli.policy = BCH_UNLOCK_POLICY_stdin;
			else
				die("invalid key_location %s", optarg);
			cli.policy_explicit = true;
			break;
		case 'f':
			cli.fake = true;
			break;
		case 'n':
			/* mount(8) compat: mount.bcachefs does not update mtab */
			cli.no_mtab = true;
			break;
		case 's':
			/* mount(8) compat: bcachefs already ignores unknown opts */
			cli.sloppy = true;
			break;
		case 'c':
			/* Output here is never colored; accepted for CLI parity */
			break;
		case 'v':
			g_verbose++;
			break;
		case 'h':
			mount_usage();
			exit(EXIT_SUCCESS);
		default:
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	if (argc < 1)
		die("Please supply device(s)");
	if (argc > 2)
		die("Too many arguments");

	cli.dev = argv[0];
	cli.mountpoint = argv[1];

	via_fuse = take_fuse_option(cli.options, &fuse_options);

	/*
	 * Before the module check: a FUSE mount mustn't modprobe the kernel
	 * driver, or a test run meant to exercise FUSE can quietly mount some
	 * of its filesystems through the kernel instead.
	 */
	if (!strcmp(cli.fs_type, "bcachefs.fuse") || via_fuse) {
		if (cli.fake) {
			mnt_info("fake mount (-f/--fake): skipping FUSE mount");
			free(fuse_options);
			return 0;
		}

#ifdef BCACHEFS_FUSE
		{
			char *fuse_argv[5];
			int fuse_argc = 0;

			fuse_argv[fuse_argc++] = prog;
			if (fuse_options[0]) {
				fuse_argv[fuse_argc++] = (char *)"-o";
				fuse_argv[fuse_argc++] = fuse_options;
			}
			fuse_argv[fuse_argc++] = cli.dev;
			fuse_argv[fuse_argc++] = cli.mountpoint ?: (char *)"";
			fuse_argv[fuse_argc] = NULL;

			/* cmd_fusemount() has a getopt loop of its own. */
			optind = 1;
			ret = cmd_fusemount(fuse_argc, fuse_argv);
		}
#else
		mnt_error("FUSE support not compiled in (build with BCACHEFS_FUSE=1)");
		free(fuse_options);
		return EXIT_FAILURE;
#endif
		free(fuse_options);
		if (ret) {
			mnt_error("FUSE mount failed");
			return EXIT_FAILURE;
		}
		return 0;
	}
	free(fuse_options);

	module = check_bcachefs_module();

	ret = cmd_mount_inner(&cli, &err);
	if (ret) {
		mnt_error("Mount failed for %s: %s", cli.dev,
			  err.text ?: "unknown error");
		if (!module.loaded) {
			mnt_error("bcachefs module not loaded?");
			if (module.modprobe_error)
				mnt_error("%s", module.modprobe_error);
		}
		mount_error_free(&err);
		free(module.modprobe_error);
		return EXIT_FAILURE;
	}

	mount_error_free(&err);
	free(module.modprobe_error);
	return 0;
}
