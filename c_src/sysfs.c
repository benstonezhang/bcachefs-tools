/*
 * sysfs: Discover bcachefs devices and attributes.
 *
 * Ported from src/wrappers/sysfs.rs.
 */

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mntent.h>

#include "libbcachefs.h"

/**
 * dev_name_from_sysfs - Resolve the block device name for a bcachefs sysfs device directory.
 *
 * Reads the `block` symlink at `dev_sysfs_path/block` (e.g.
 * `/sys/fs/bcachefs/<UUID>/dev-0/block`) and returns the basename
 * of the target (e.g. "sda"). Falls back to the directory name
 * (e.g. "dev-0") if the symlink is absent (offline device).
 */
char *dev_name_from_sysfs(const char *dev_sysfs_path)
{
	char block_link[PATH_MAX];
	snprintf(block_link, sizeof(block_link), "%s/block", dev_sysfs_path);

	char target[PATH_MAX];
	ssize_t r = readlink(block_link, target, sizeof(target) - 1);
	if (r > 0) {
		target[r] = '\0';
		return strdup(basename(target));
	}

	char *tmp = strdup(dev_sysfs_path);
	char *name = strdup(basename(tmp));
	free(tmp);
	return name;
}

/**
 * Resolve the block device display name for a bcachefs sysfs device directory.
 *
 * Returns the raw kernel name by default, or the mapper basename for
 * dm-multipath devices when requested and available.
 */
char *dev_display_name_from_sysfs(const char *dev_sysfs_path,
				  enum device_name_mode mode)
{
	char *name = dev_name_from_sysfs(dev_sysfs_path);

	if (mode == DEVICE_NAME_RAW)
		return name;

	char *mapper = preferred_multipath_devnode_for_block_name(name);
	if (!mapper)
		return name;

	char *base = strdup(basename(mapper));
	free(mapper);
	if (base) {
		free(name);
		return base;
	}
	return name;
}

/**
 * sysfs_path_from_fd - Resolve the sysfs path from a file descriptor.
 *
 * Uses /proc/self/fd/<fd> readlink to find the directory's absolute path.
 */
char *sysfs_path_from_fd(int fd)
{
	char link[64];
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);

	char *path = xmalloc(PATH_MAX);
	ssize_t r = readlink(link, path, PATH_MAX - 1);
	if (r < 0) {
		free(path);
		return NULL;
	}
	path[r] = '\0';
	return path;
}

/**
 * read_sysfs_u64 - Read a sysfs attribute as a u64.
 *
 * Strips whitespace and parses the value.
 */
u64 read_sysfs_u64(const char *path)
{
	return read_file_u64(AT_FDCWD, path);
}

/**
 * read_sysfs_fd_str - Read a sysfs attribute as a string, relative to a directory fd.
 *
 * Returns a trimmed, newly allocated string.
 */
char *read_sysfs_fd_str(int dirfd, const char *path)
{
	return read_file_str(dirfd, path);
}

#define KERNEL_VERSION_PATH "/sys/module/bcachefs/parameters/version"

/**
 * bcachefs_kernel_version - Read the bcachefs kernel module metadata version.
 *
 * Returns 0 if the module isn't loaded.
 */
u64 bcachefs_kernel_version(void)
{
	if (access(KERNEL_VERSION_PATH, R_OK))
		return 0;
	return read_sysfs_u64(KERNEL_VERSION_PATH);
}

/**
 * dev_mounted - Check if a block device is currently mounted.
 *
 * Parses /proc/mounts and compares device identity (st_rdev for block
 * devices, st_dev+st_ino for files) against each mount's device path(s).
 * bcachefs mounts list multiple devices separated by colons.
 *
 * Returns 0 if not mounted, 1 if mounted RO, 2 if mounted RW.
 */
int dev_mounted(const char *path)
{
	struct mntent *mnt;
	FILE *f = setmntent("/proc/mounts", "r");
	if (!f)
		die("error opening /proc/mounts: %m");

	struct stat d1 = xstat(path);
	int ret = 0;

	while ((mnt = getmntent(f))) {
		char *dev, *p = mnt->mnt_fsname;

		while ((dev = strsep(&p, ":"))) {
			struct stat d2;

			if (stat(dev, &d2))
				continue;

			if (S_ISBLK(d1.st_mode) != S_ISBLK(d2.st_mode))
				continue;

			if (S_ISBLK(d1.st_mode)) {
				if (d1.st_rdev != d2.st_rdev)
					continue;
			} else {
				if (d1.st_dev != d2.st_dev ||
				    d1.st_ino != d2.st_ino)
					continue;
			}

			if (hasmntopt(mnt, "ro"))
				ret = 1;
			else
				ret = 2;
			goto found;
		}
	}
found:
	fclose(f);
	return ret;
}

/**
 * sysfs_write_str - Write a string value to a sysfs attribute file relative to a directory fd.
 *
 * Both failures are returned rather than discarded: a non-runtime option's
 * attribute is created 0444, so opening it O_WRONLY fails with EACCES, and
 * a caller that ignored that reported success while changing nothing.
 *
 * Returns 0 on success, or -errno.
 */
int sysfs_write_str(int sysfs_fd, const char *path, const char *value)
{
	int fd = openat(sysfs_fd, path, O_WRONLY);
	ssize_t wrote, len;
	int err;

	if (fd < 0)
		return -errno;

	len = strlen(value);
	wrote = write(fd, value, len);
	err = wrote == len ? 0 : -EIO;
	close(fd);
	return err;
}

static int dev_info_cmp(const void *_l, const void *_r)
{
	const struct dev_name *l = _l, *r = _r;
	return (l->idx > r->idx) - (l->idx < r->idx);
}

/**
 * fs_get_devices - Enumerate devices for a mounted filesystem from its sysfs directory path.
 *
 * Reads `dev-N/` subdirectories under `sysfs_path`, extracting the block
 * device display name for each. Offline devices (dangling block symlink)
 * are included with online=false.
 */
dev_names fs_get_devices(const char *sysfs_path, enum device_name_mode mode)
{
	DIR *dir = opendir(sysfs_path);
	struct dirent *d;
	dev_names devs;

	darray_init(&devs);
	if (!dir)
		return devs;

	while ((errno = 0), (d = readdir(dir))) {
		struct dev_name n = { 0 };

		if (sscanf(d->d_name, "dev-%u", &n.idx) != 1)
			continue;

		char dev_path[PATH_MAX - 6];
		snprintf(dev_path, sizeof(dev_path), "%s/%s", sysfs_path,
			 d->d_name);

		/*
		 * metadata follows the symlink: a hot-removed device can leave
		 * dev-N/block dangling, and that should read as offline.
		 */
		struct stat st;
		char block_path[PATH_MAX];
		snprintf(block_path, sizeof(block_path), "%s/block", dev_path);
		n.online = !stat(block_path, &st);

		n.dev = dev_display_name_from_sysfs(dev_path, mode);

		char label_path[PATH_MAX];
		snprintf(label_path, sizeof(label_path), "%s/label", d->d_name);
		n.label = read_file_str(dirfd(dir), label_path);

		char durability_path[PATH_MAX];
		snprintf(durability_path, sizeof(durability_path),
			 "%s/durability", d->d_name);
		char *dur_s = read_file_str(dirfd(dir), durability_path);
		if (dur_s) {
			n.durability = strtoul(dur_s, NULL, 10);
			free(dur_s);
		} else {
			n.durability = 1;
		}

		char fd_path[PATH_MAX];
		snprintf(fd_path, sizeof(fd_path), "%s/failure_domain",
			 d->d_name);
		char *fd_s = read_file_str(dirfd(dir), fd_path);
		if (fd_s) {
			if (!strcmp(fd_s, "none"))
				free(fd_s);
			else
				n.failure_domain = fd_s;
		}

		darray_push(&devs, n);
	}
	closedir(dir);

	qsort(devs.data, devs.nr, sizeof(devs.data[0]), dev_info_cmp);
	return devs;
}

void dev_names_free(dev_names *devs)
{
	for (struct dev_name *d = devs->data; d < devs->data + devs->nr; d++) {
		free(d->dev);
		free(d->label);
		free(d->failure_domain);
	}
	darray_exit(devs);
}
