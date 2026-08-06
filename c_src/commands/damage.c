/*
 * bcachefs damage - show recorded filesystem damage
 *
 * The damage btree persistently records which inodes were damaged by errors
 * and repairs. Two ioctls, layered: BCHFS_IOC_READDIR_FLAGS with damaged finds
 * the files - cost proportional to recorded damage, not tree size - and
 * BCHFS_IOC_GET_DAMAGE per file returns its accumulated error list (unioned
 * across ancestor snapshots) as bch_sb_error_id, printed with the same names
 * fsck and the superblock error counters use.
 *
 * Ported from src/commands/damage.rs.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "libbcachefs.h"
#include "sb/errors.h"
#include "cmds.h"

/* DT_SUBVOL from dirent_format.h; not exposed by the C library */
#define DT_SUBVOL	16

static void damage_usage(void)
{
	puts("bcachefs damage - show recorded filesystem damage\n"
	     "Usage: bcachefs damage <PATH>\n"
	     "       bcachefs damage ls [-R] [DIRECTORY]\n"
	     "       bcachefs damage clear [-R] PATH\n"
	     "\n"
	     "With a path, shows the recorded damage on that file or directory.\n"
	     "\n"
	     "Commands:\n"
	     "  ls     List damaged files in a directory\n"
	     "  clear  Clear recorded damage (requires owning the file, like chattr;\n"
	     "         snapshots keep their view of the record)\n"
	     "\n"
	     "Options:\n"
	     "  -R, --recursive   Recurse: damaged files anywhere under the directory\n"
	     "  -h, --help        Display this help and exit\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

/* One accumulated damage record, unpacked from bch_sb_field_error_entry_v2 -
 * the same record the errors superblock section keeps: error id, a saturating
 * occurrence count, and first and last occurrence times. */
struct damage_entry {
	u32	id;
	u64	nr;
	s64	first;
	s64	last;
};

static char *fmt_time(s64 t)
{
	time_t tt = t;
	struct tm tm;

	if (gmtime_r(&tt, &tm)) {
		char buf[64];

		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
		return xstrdup(buf);
	}
	return mprintf("%lld", (long long)t);
}

/* BCHFS_IOC_GET_DAMAGE: nr_entries in is capacity, out is the true count -
 * retry bigger if we undershot. Returns 0 on success, negative errno. */
static int get_damage(int fd, struct damage_entry **out, size_t *out_nr)
{
	u32 cap = 16;
	struct bch_ioctl_get_damage *arg = NULL;

	for (;;) {
		arg = xrealloc(arg, sizeof(*arg) + cap *
			       sizeof(bch_sb_field_error_entry_v2));
		memset(arg, 0, sizeof(*arg));
		arg->nr_entries = cap;

		if (ioctl(fd, BCHFS_IOC_GET_DAMAGE, arg))
			return -errno;

		u32 nr = arg->nr_entries;
		if (nr > cap) {
			cap = nr;
			continue;
		}

		struct damage_entry *entries = xcalloc(nr, sizeof(*entries));

		for (u32 i = 0; i < nr; i++) {
			entries[i].id	= BCH_SB_ERROR_ENTRY_V2_ID(&arg->entries[i]);
			entries[i].nr	= BCH_SB_ERROR_ENTRY_V2_NR(&arg->entries[i]);
			entries[i].first = BCH_SB_ERROR_ENTRY_V2_FIRST(&arg->entries[i]);
			entries[i].last	= BCH_SB_ERROR_ENTRY_V2_LAST(&arg->entries[i]);
		}

		free(arg);
		*out = entries;
		*out_nr = nr;
		return 0;
	}
}

static void prt_sb_error(struct printbuf *out, u32 id)
{
	bch2_sb_error_id_to_text(out, id);
}

struct dir_entry {
	u8	d_type;
	char	*name;
};

/* One BCHFS_IOC_READDIR_FLAGS call, advancing the opaque cursor. An empty
 * result means enumeration is complete. Returns 0 on success, negative errno. */
static int readdir_flags(int fd, u32 flags, u64 pos[2],
			 struct dir_entry **out, size_t *out_nr)
{
	char *buf = xcalloc(1, 64 << 10);
	struct bch_ioctl_readdir_flags arg = {
		.pos		= { pos[0], pos[1] },
		.buf		= (u64)buf,
		.buf_size	= 64 << 10,
		.flags		= flags,
		.used		= 0,
	};

	if (ioctl(fd, BCHFS_IOC_READDIR_FLAGS, &arg)) {
		int ret = -errno;
		free(buf);
		return ret;
	}
	pos[0] = arg.pos[0];
	pos[1] = arg.pos[1];

	size_t nr = 0;
	struct dir_entry *entries = NULL;

	size_t offset = 0;
	while (offset + offsetof(struct bch_ioctl_readdir_entry, name) <
	       arg.used) {
		struct bch_ioctl_readdir_entry *rec =
			(void *)(buf + offset);
		size_t name_len = rec->name_len;
		size_t reclen = round_up(
			offsetof(struct bch_ioctl_readdir_entry, name) +
			name_len, 8);

		if (name_len == 0 || offset + reclen > arg.used)
			break;

		entries = xrealloc(entries, (nr + 1) * sizeof(*entries));
		entries[nr].d_type = rec->d_type;
		/* name_len includes the nul */
		entries[nr].name = xstrndup(rec->name, name_len - 1);
		nr++;
		offset += reclen;
	}

	free(buf);
	*out = entries;
	*out_nr = nr;
	return 0;
}

/* The damage details for a listed entry, best effort: only inode types we can
 * open and ioctl (a device node's fd would take the ioctl to the device
 * driver). Recursive mode reports DT_UNKNOWN, so stat then.
 * Returns 0 with err_nr possibly 0 on success, negative errno on failure. */
static int entry_errors(int dirfd, struct dir_entry *e,
			struct damage_entry **errs, size_t *err_nr)
{
	u8 d_type = e->d_type;
	struct stat st;

	if (d_type == DT_UNKNOWN) {
		if (fstatat(dirfd, e->name, &st, AT_SYMLINK_NOFOLLOW))
			return -errno;
		if (S_ISREG(st.st_mode))
			d_type = DT_REG;
		else if (S_ISDIR(st.st_mode))
			d_type = DT_DIR;
		else
			return -ENOTSUP;
	}

	if (d_type != DT_REG && d_type != DT_DIR && d_type != DT_SUBVOL)
		return -ENOTSUP;

	int fd = openat(dirfd, e->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	int ret = get_damage(fd, errs, err_nr);
	close(fd);
	return ret;
}

static void free_entries(struct dir_entry *entries, size_t nr)
{
	for (size_t i = 0; i < nr; i++)
		free(entries[i].name);
	free(entries);
}

static int cmd_damage_ls(const char *path, bool recursive)
{
	int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0)
		die("Error opening %s: %m", path);

	u32 flags = BCH_READDIR_damaged |
		    (recursive ? BCH_READDIR_recursive : 0);
	u64 pos[2] = { 0, 0 };

	for (;;) {
		struct dir_entry *entries;
		size_t nr;

		int ret = readdir_flags(dir, flags, pos, &entries, &nr);
		if (ret)
			die("BCHFS_IOC_READDIR_FLAGS: %s", strerror(-ret));

		if (!nr) {
			free(entries);
			break;
		}

		for (size_t i = 0; i < nr; i++) {
			struct damage_entry *errs;
			size_t err_nr;

			int r = entry_errors(dir, &entries[i], &errs, &err_nr);

			if (!r && err_nr) {
				struct printbuf buf = PRINTBUF;

				prt_str(&buf, entries[i].name);
				prt_str(&buf, ":");
				for (size_t j = 0; j < err_nr; j++) {
					prt_str(&buf, " ");
					prt_sb_error(&buf, errs[j].id);
				}
				printf("%s\n", buf.buf);
				printbuf_exit(&buf);
				free(errs);
			} else {
				printf("%s\n", entries[i].name);
				if (!r)
					free(errs);
			}
		}
		free_entries(entries, nr);
	}

	close(dir);
	return 0;
}

static int cmd_damage_show(const char *path)
{
	struct stat st = xstat(path);

	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		die("%s: not a regular file or directory", path);

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("Error opening %s: %m", path);

	struct damage_entry *errs;
	size_t nr;

	int ret = get_damage(fd, &errs, &nr);
	if (ret)
		die("BCHFS_IOC_GET_DAMAGE: %s", strerror(-ret));
	close(fd);

	for (size_t i = 0; i < nr; i++) {
		struct printbuf buf = PRINTBUF;
		char *first = fmt_time(errs[i].first);
		char *last = fmt_time(errs[i].last);

		prt_sb_error(&buf, errs[i].id);
		printf("%-48s nr %-8llu first %s  last %s\n",
		       buf.buf, (unsigned long long)errs[i].nr,
		       first, last);
		printbuf_exit(&buf);
		free(first);
		free(last);
	}
	free(errs);
	return 0;
}

static int clear_fd(int fd)
{
	if (ioctl(fd, BCHFS_IOC_CLEAR_DAMAGE, NULL))
		return -errno;
	return 0;
}

static int cmd_damage_clear(const char *path, bool recursive)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("Error opening %s: %m", path);

	int ret = clear_fd(fd);
	close(fd);
	if (ret)
		die("clearing damage on %s: %s", path, strerror(-ret));

	if (!recursive)
		return 0;

	/*
	 * The damaged-only recursive listing is also the work list: entries
	 * vanish from it as they're cleared, and the cursor only moves
	 * forward, so clearing behind it is safe. Per-entry failures
	 * (ownership, racing renames) are reported and skipped.
	 */
	int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0)
		die("Error opening %s: %m", path);

	u32 flags = BCH_READDIR_damaged | BCH_READDIR_recursive;
	u64 pos[2] = { 0, 0 };
	u64 failed = 0;

	for (;;) {
		struct dir_entry *entries;
		size_t nr;

		ret = readdir_flags(dir, flags, pos, &entries, &nr);
		if (ret)
			die("BCHFS_IOC_READDIR_FLAGS: %s", strerror(-ret));

		if (!nr) {
			free(entries);
			break;
		}

		for (size_t i = 0; i < nr; i++) {
			int cfd = openat(dir, entries[i].name,
					 O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

			if (cfd < 0) {
				fprintf(stderr, "%s: %s\n",
					entries[i].name, strerror(errno));
				failed++;
				continue;
			}

			ret = clear_fd(cfd);
			if (ret) {
				fprintf(stderr, "%s: %s\n",
					entries[i].name, strerror(-ret));
				failed++;
			}
			close(cfd);
		}
		free_entries(entries, nr);
	}

	close(dir);

	if (failed) {
		fprintf(stderr, "failed to clear %llu files\n",
			(unsigned long long)failed);
		return 1;
	}
	return 0;
}

int cmd_damage(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "recursive", no_argument, NULL, 'R' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};

	char *cmd = pop_cmd(&argc, argv);
	if (!cmd) {
		damage_usage();
		return -EINVAL;
	}

	if (!strcmp(cmd, "ls")) {
		bool recursive = false;
		int opt;

		while ((opt = getopt_long(argc, argv, "Rh", longopts, NULL)) != -1)
			switch (opt) {
			case 'R':
				recursive = true;
				break;
			case 'h':
				damage_usage();
				exit(EXIT_SUCCESS);
			default:
				return -EINVAL;
			}
		args_shift(optind);

		char *path = arg_pop() ?: ".";
		if (argc)
			return -EINVAL;
		return cmd_damage_ls(path, recursive);
	}

	if (!strcmp(cmd, "clear")) {
		bool recursive = false;
		int opt;

		while ((opt = getopt_long(argc, argv, "Rh", longopts, NULL)) != -1)
			switch (opt) {
			case 'R':
				recursive = true;
				break;
			case 'h':
				damage_usage();
				exit(EXIT_SUCCESS);
			default:
				return -EINVAL;
			}
		args_shift(optind);

		char *path = arg_pop();
		if (!path)
			die("a path is required for clear");
		if (argc)
			return -EINVAL;
		return cmd_damage_clear(path, recursive);
	}

	/* default: show */
	if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
		damage_usage();
		exit(EXIT_SUCCESS);
	}
	args_shift(1);	/* program name */
	if (argc)
		return -EINVAL;
	return cmd_damage_show(cmd);
}
