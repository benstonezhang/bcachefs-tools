/*
 * The Linux fs_context mount API: fsopen(2), fsconfig(2), fsmount(2),
 * move_mount(2).
 *
 * Nothing here is bcachefs-specific; it's the generic new-mount-API plumbing
 * that mount.bcachefs sits on top of.
 *
 * # Why this exists at all
 *
 * mount(2) can only fail with an errno. A filesystem that has something
 * specific to say about why a mount failed has nowhere to put it, so the user
 * gets "Invalid argument" and the real explanation goes to dmesg at best. The
 * fs_context API replaces the single syscall with an object: you open a
 * context, feed it parameters one at a time, ask it to create a superblock,
 * and turn the result into a mount. The context carries a message log, which
 * is the whole point - bch_fs_context_drain_log() is where a filesystem's own
 * account of the failure comes back.
 *
 * # Shape of a mount
 *
 *	bch_fs_context_open()?            // fsopen
 *	  .set("source", devs)?            // fsconfig, once per parameter
 *	  .set("errors", "panic")?
 *	  .bch_fs_context_create()?        // fsconfig(FSCONFIG_CMD_CREATE)
 *	  .bch_fs_context_fsmount(attrs)?  // -> detached mount fd
 *	move_mount(&mnt, target)?          // attach it to the tree
 *
 * Setting parameters individually rather than as one comma-joined string is
 * not just tidier: the kernel reports a rejected parameter against the
 * parameter, so errors name the option that was wrong.
 *
 * # Two kinds of mount flag
 *
 * The old MS_* namespace conflates two things the new API separates, and
 * getting this wrong loses flags silently:
 *
 * - Per-mount (rdonly, nosuid, nodev, noexec, atime behaviour) become
 *   MOUNT_ATTR_* passed to fsmount(). See bch_fs_context_mount_attrs().
 * - Per-superblock (sync, dirsync, mand, lazytime) have no MOUNT_ATTR_*
 *   equivalent and travel as named fsconfig parameters, which the VFS consumes
 *   in vfs_parse_sb_flag() before the filesystem ever sees them. See
 *   bch_fs_context_sb_flag_params().
 *
 * # Availability
 *
 * These syscalls landed in Linux 5.2. bch_fs_context_open() returns -ENOSYS so
 * callers can fall back to mount(2) rather than refusing to work on an older
 * kernel.
 *
 * Reconfiguring an already-mounted filesystem (MS_REMOUNT) is a different
 * entry point - fspick(2) plus FSCONFIG_CMD_RECONFIGURE - and isn't
 * implemented here.
 *
 * Constants are from uapi linux/mount.h. libc has the syscall numbers but not
 * these; they're stable ABI.
 */

#include "libbcachefs.h"
#include "tools-util.h"

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/syscall.h>

#define FSOPEN_CLOEXEC		0x00000001
#define FSMOUNT_CLOEXEC		0x00000001

#define FSCONFIG_SET_FLAG	0
#define FSCONFIG_SET_STRING	1
#define FSCONFIG_CMD_CREATE	6

#define MOVE_MOUNT_F_EMPTY_PATH	0x00000004

/*
 * The most a single fsconfig(2) parameter value can carry.
 *
 * The kernel copies it with `strndup_user(_value, 256)` (fs/fsopen.c), and
 * strndup_user() returns -EINVAL when the string doesn't fit rather than
 * truncating it - so 255 bytes of value, and no way to send more.
 *
 * mount(2) has no equivalent limit: it takes the source through
 * copy_mount_string(), which allows PATH_MAX. So the new API cannot express
 * every mount the old one could, and a caller with a long value has to fall
 * back rather than adapt.
 *
 * The refusal also arrives with an empty fs_context log, because it happens in
 * the syscall wrapper before the VFS sees the parameter - worth knowing, since
 * everything else here reports failures through that log.
 */
#define BCH_FS_CONTEXT_PARAM_VALUE_MAX	255

/* MS_LAZYTIME, which libc doesn't define. */
#ifndef MS_LAZYTIME
#define MS_LAZYTIME	(1UL << 25)
#endif

/*
 * Translate the per-mount MS_* flags to the MOUNT_ATTR_* fsmount() takes.
 *
 * Flags with no per-mount equivalent are dropped here on purpose; see
 * bch_fs_context_sb_flag_params() for where the superblock ones go instead.
 * The two sets are not disjoint: MS_RDONLY is both, and has to be sent both
 * ways.
 */
u32 bch_fs_context_mount_attrs(unsigned long mountflags)
{
	const struct {
		unsigned long	ms;
		u32		attr;
	} map[] = {
		{ MS_RDONLY,	0x00000001 },	/* MOUNT_ATTR_RDONLY */
		{ MS_NOSUID,	0x00000002 },	/* MOUNT_ATTR_NOSUID */
		{ MS_NODEV,	0x00000004 },	/* MOUNT_ATTR_NODEV */
		{ MS_NOEXEC,	0x00000008 },	/* MOUNT_ATTR_NOEXEC */
		{ MS_NOATIME,	0x00000010 },	/* MOUNT_ATTR_NOATIME */
		{ MS_STRICTATIME, 0x00000020 },	/* MOUNT_ATTR_STRICTATIME */
		{ MS_NODIRATIME, 0x00000080 },	/* MOUNT_ATTR_NODIRATIME */
	};
	u32 attrs = 0;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(map); i++)
		if (mountflags & map[i].ms)
			attrs |= map[i].attr;

	return attrs;
}

/*
 * The MS_* flags that are superblock state, as the fsconfig() parameter names
 * the VFS knows them by.
 *
 * MS_RDONLY is here *and* in bch_fs_context_mount_attrs(), because mount(2)
 * conflated two things the new API separates: MOUNT_ATTR_RDONLY makes this
 * mount read-only, while fsconfig("ro") sets fc->sb_flags |= SB_RDONLY, which
 * is what tells the filesystem to open its devices read-only. Send only the
 * former and a write-protected device fails the mount outright - the
 * filesystem asks for O_RDWR and gets EACCES, and mount(8)'s read-only retry
 * has nothing to change.
 *
 * Writes the parameter names into @params (static strings, up to the capacity
 * in the last two entries of the map below) and returns how many were set.
 */
unsigned bch_fs_context_sb_flag_params(unsigned long mountflags,
				       const char **params)
{
	const struct {
		unsigned long	ms;
		const char	*name;
	} map[] = {
		{ MS_RDONLY,		"ro" },
		{ MS_SYNCHRONOUS,	"sync" },
		{ MS_DIRSYNC,		"dirsync" },
		{ MS_MANDLOCK,		"mand" },
		{ MS_LAZYTIME,		"lazytime" },
	};
	unsigned nr = 0, i;

	for (i = 0; i < ARRAY_SIZE(map); i++)
		if (mountflags & map[i].ms)
			params[nr++] = map[i].name;

	return nr;
}

/**
 * bch_fs_context_open - fsopen(2).
 *
 * Returns the fd on success. -ENOSYS means the kernel predates the new mount
 * API and the caller should fall back to mount(2). -ENODEV means the
 * filesystem type isn't registered - typically the module isn't loaded.
 */
int bch_fs_context_open(const char *fstype)
{
	long ret = syscall(SYS_fsopen, fstype, FSOPEN_CLOEXEC);

	if (ret < 0)
		return (int)-errno;
	return (int)ret;
}

static int fs_config(int fd, unsigned cmd, const char *key, const char *value)
{
	long ret = syscall(SYS_fsconfig, fd, cmd, key, value, 0);

	if (ret < 0)
		return (int)-errno;
	return 0;
}

/**
 * bch_fs_context_set - fsconfig(2): set one parameter, with a value or as a
 * bare flag (@value == NULL).
 */
int bch_fs_context_set(int fd, const char *key, const char *value)
{
	return fs_config(fd, value ? FSCONFIG_SET_STRING : FSCONFIG_SET_FLAG,
			key, value);
}

/**
 * bch_fs_context_status_fd - Ask bcachefs for a status channel, and get the
 * file descriptor back.
 *
 * Not a mount option: bcachefs's `parse_param` creates the descriptor and
 * returns its number as this fsconfig(2) call's return value, which
 * vfs_parse_fs_param() passes back untouched. Must come before
 * bch_fs_context_create().
 *
 * Returns the status fd, or 0 when the kernel doesn't know the parameter -
 * mounting without a channel is how it has always worked, so absence isn't a
 * failure.
 */
int bch_fs_context_status_fd(int fd)
{
	long ret = syscall(SYS_fsconfig, fd, FSCONFIG_SET_FLAG,
			   "status_fd", NULL, 0);

	if (ret > 0)
		return (int)ret;

	/*
	 * Zero would mean the parameter was accepted without producing a
	 * descriptor, which nothing does - don't invent an fd 0 out of it.
	 * Negative is an older module rejecting the name, which also leaves an
	 * "Unknown parameter" in the context log; drain it so it isn't reported
	 * later as though the mount had complained.
	 */
	bch_fs_context_drain_log(fd);
	return 0;
}

/**
 * bch_fs_context_create - fsconfig(FSCONFIG_CMD_CREATE): actually create the
 * superblock. This is where a filesystem does its real work, and where it has
 * the most to say if it fails.
 */
int bch_fs_context_create(int fd)
{
	return fs_config(fd, FSCONFIG_CMD_CREATE, NULL, NULL);
}

/**
 * bch_fs_context_fsmount - fsmount(2): turn the created superblock into a
 * detached mount. Attach it with bch_fs_context_move_mount().
 */
int bch_fs_context_fsmount(int fd, u32 attrs)
{
	long ret = syscall(SYS_fsmount, fd, FSMOUNT_CLOEXEC, attrs);

	if (ret < 0)
		return (int)-errno;
	return (int)ret;
}

/**
 * bch_fs_context_drain_log - Read out everything the kernel has logged
 * against this context.
 *
 * Reading is destructive - each message is returned exactly once - so call
 * this at the point the messages are wanted, and before dropping the context.
 * An empty result just means the filesystem didn't say anything, which is
 * common.
 *
 * Returns a NULL-terminated array (last entry has text == NULL); free with
 * bch_fs_context_msgs_free().
 */
struct bch_fs_context_msg *bch_fs_context_drain_log(int fd)
{
	size_t nr = 0, cap = 8;
	struct bch_fs_context_msg *msgs = xmalloc(cap * sizeof(msgs[0]));
	char buf[4096];

	/*
	 * Each read() yields one message, "<level> <text>\n", and ENODATA once
	 * the log is empty. Any other error also ends the loop; there's nothing
	 * useful to do about it while already reporting a failure.
	 */
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		char *line, *space, *text;
		enum bch_fs_context_level level;

		if (n <= 0)
			break;
		buf[n] = 0;
		line = buf;
		n = strlen(line);
		if (n && line[n - 1] == '\n')
			line[n - 1] = 0;

		space = strchr(line, ' ');
		if (space) {
			*space = 0;
			if (line[0] == 'e' && !line[1])
				level = BCH_FS_CONTEXT_ERROR;
			else if (line[0] == 'w' && !line[1])
				level = BCH_FS_CONTEXT_WARNING;
			else if (line[0] == 'i' && !line[1])
				level = BCH_FS_CONTEXT_NOTICE;
			else {
				/*
				 * Unrecognised prefix: keep the line whole
				 * rather than eat it.
				 */
				*space = ' ';
				level = BCH_FS_CONTEXT_ERROR;
				text = line;
				space = NULL;
			}
			if (space)
				text = space + 1;
		} else {
			level = BCH_FS_CONTEXT_ERROR;
			text = line;
		}

		if (nr == cap - 1) {
			cap *= 2;
			msgs = xrealloc(msgs, cap * sizeof(msgs[0]));
		}
		msgs[nr].level = level;
		msgs[nr].text = xstrdup(text);
		nr++;
	}

	msgs[nr].text = NULL;
	return msgs;
}

void bch_fs_context_msgs_free(struct bch_fs_context_msg *msgs)
{
	unsigned i;

	for (i = 0; msgs[i].text; i++)
		free(msgs[i].text);
	free(msgs);
}

/**
 * bch_fs_context_move_mount - move_mount(2): attach a detached mount from
 * bch_fs_context_fsmount() to a path.
 *
 * Dropping the mount fd without doing this discards the mount and releases
 * the superblock.
 */
int bch_fs_context_move_mount(int mnt_fd, const char *target)
{
	long ret = syscall(SYS_move_mount, mnt_fd, "", AT_FDCWD, target,
			   MOVE_MOUNT_F_EMPTY_PATH);

	if (ret < 0)
		return (int)-errno;
	return 0;
}