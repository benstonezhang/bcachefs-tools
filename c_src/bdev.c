/*
 * Block device utilities.
 *
 * Ported from src/wrappers/bdev.rs.
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysmacros.h>

#include "libbcachefs.h"

/**
 * get_size - Returns the size of a file or block device in bytes.
 *
 * For block devices, uses BLKGETSIZE64 ioctl.
 * For regular files, returns st_size from fstat.
 */
u64 get_size(int fd)
{
	struct stat stat = xfstat(fd);

	if (S_ISBLK(stat.st_mode)) {
		u64 size = 0;
		if (ioctl(fd, BLKGETSIZE64, &size))
			die("ioctl BLKGETSIZE64 error: %m");
		return size;
	} else {
		return stat.st_size;
	}
}

/**
 * get_blocksize_physical_hint - Returns the physical block size of a block
 * device (the _larger_ of the two), with fallback to the filesystem block size
 * hint for regular files, in bytes. (to be used as a performance hint only)
 */
u32 get_blocksize_physical_hint(int fd)
{
	struct stat stat = xfstat(fd);

	if (S_ISBLK(stat.st_mode)) {
		unsigned bs = 0;
		if (ioctl(fd, BLKPBSZGET, &bs))
			die("ioctl BLKPBSZGET error: %m");
		return bs;
	} else {
		return stat.st_blksize;
	}
}

/**
 * get_blocksize_logical - Returns logical block size (LBA) of a block device
 * (the smaller of the two), with fallback to the filesystem block size hint for
 * regular files, in bytes. (suitable for use as an alignment for direct I/O or
 * similar)
 */
u32 get_blocksize_logical(int fd)
{
	struct stat stat = xfstat(fd);

	if (S_ISBLK(stat.st_mode)) {
		unsigned bs = 0;
		if (ioctl(fd, BLKSSZGET, &bs))
			die("ioctl BLKSSZGET error: %m");
		return bs;
	} else {
		return stat.st_blksize;
	}
}

/**
 * fd_to_dev_model - Returns the device model string for a block device fd, or a
 * fallback description for regular files / unknown devices.
 */
char *fd_to_dev_model(int fd)
{
	struct stat stat = xfstat(fd);

	if (!S_ISBLK(stat.st_mode))
		return strdup("(image file)");

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/",
		 major(stat.st_rdev), minor(stat.st_rdev));
	path[sizeof(path) - 1] = '\0';
	size_t sysfs_len = strlen(path);

	static const char *suffixes[] = { "device/model", "../device/model",
					  "loop/backing_file" };

	for (size_t i = 0; i < ARRAY_SIZE(suffixes); i++) {
		strncpy(path + sysfs_len, suffixes[i],
			sizeof(path) - sysfs_len - 1);
		char *contents = read_file_str(AT_FDCWD, path);
		if (contents) {
			char *trimmed = strdup(strim(contents));
			free(contents);
			if (trimmed && strlen(trimmed))
				return trimmed;
			free(trimmed);
		}
	}

	return strdup("(unknown model)");
}

/**
 * fd_to_dev_serial - Returns the device serial number for a block device fd,
 * or NULL if not available (image files, devices without serial sysfs entry).
 */
char *fd_to_dev_serial(int fd)
{
	struct stat stat = xfstat(fd);

	if (!S_ISBLK(stat.st_mode))
		return NULL;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/",
		 major(stat.st_rdev), minor(stat.st_rdev));
	path[sizeof(path) - 1] = '\0';
	size_t sysfs_len = strlen(path);

	static const char *suffixes[] = { "device/serial", "../device/serial" };

	for (size_t i = 0; i < ARRAY_SIZE(suffixes); i++) {
		strncpy(path + sysfs_len, suffixes[i],
			sizeof(path) - sysfs_len - 1);
		char *contents = read_file_str(AT_FDCWD, path);
		if (contents) {
			char *trimmed = strdup(strim(contents));
			free(contents);
			if (trimmed && strlen(trimmed))
				return trimmed;
			free(trimmed);
		}
	}

	return NULL;
}

/**
 * fd_to_parent_disk_sysfs - Returns a stable sysfs identity for the whole disk
 * backing this block device. Partitions are resolved to their parent disk.
 * Caller must free the returned string.
 */
char *fd_to_parent_disk_sysfs(int fd)
{
	struct stat st = xfstat(fd);

	if (!S_ISBLK(st.st_mode))
		return NULL;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u", major(st.st_rdev),
		 minor(st.st_rdev));

	char *canon = realpath(path, NULL);
	if (!canon)
		return NULL;

	char *partition = mprintf("%s/partition", canon);
	bool is_partition = !access(partition, F_OK);
	free(partition);

	if (is_partition) {
		char *slash = strrchr(canon, '/');
		if (slash && slash != canon)
			*slash = '\0';
	}

	return canon;
}

/**
 * nonrot - Returns true if the block device is non-rotational (SSD).
 *
 * For regular files (image files) the ioctl fails and we default to false.
 */
bool nonrot(int fd)
{
	/*
	 * BLKROTATIONAL = _IO(0x12, 126). Kernel returns !bdev_nonrot(bdev) via
	 * put_ushort — bdev_nonrot internally uses bdev_get_queue(bdev), which
	 * resolves to the parent disk's queue for partitions and handles LVM,
	 * md, loop devices, etc. uniformly.
	 */
	unsigned short rotational = 0;
	if (ioctl(fd, BLKROTATIONAL, &rotational))
		return false;
	return rotational == 0;
}

/**
 * open_device - Open a block device or file for formatting.
 *
 * Translates BLK_OPEN_* flags to POSIX open flags.
 * Returns the raw fd on success, or a negative errno on failure.
 */
int open_device(const char *path, unsigned mode)
{
	int flags = 0;

	unsigned rw = mode & (BLK_OPEN_READ | BLK_OPEN_WRITE);
	if (rw == (BLK_OPEN_READ | BLK_OPEN_WRITE))
		flags = O_RDWR;
	else if (mode & BLK_OPEN_READ)
		flags = O_RDONLY;
	else if (mode & BLK_OPEN_WRITE)
		flags = O_WRONLY;

	if (!(mode & BLK_OPEN_BUFFERED))
		flags |= O_DIRECT;

	if (mode & BLK_OPEN_EXCL)
		flags |= O_EXCL;

	if (mode & BLK_OPEN_CREAT)
		flags |= O_CREAT;

	int fd = open(path, flags, 0600);
	if (fd < 0)
		return -errno;
	return fd;
}

struct bch_ioctl_dev_usage_v2 *bchu_dev_usage(struct bchfs_handle fs,
					      unsigned idx)
{
	struct bch_ioctl_dev_usage_v2 *u =
		xcalloc(sizeof(*u) + sizeof(u->d[0]) * BCH_DATA_NR, 1);

	u->dev = idx;
	u->flags = BCH_BY_INDEX;
	u->nr_data_types = BCH_DATA_NR;

	if (!ioctl(fs.ioctl_fd, BCH_IOCTL_DEV_USAGE_V2, u))
		return u;

	fprintf(stderr, "warning: dev_usage V2 ioctl failed (kernel too old?), falling back to V1\n");

	struct bch_ioctl_dev_usage u_v1 = { .dev = idx, .flags = BCH_BY_INDEX };
	xioctl(fs.ioctl_fd, BCH_IOCTL_DEV_USAGE, &u_v1);

	u->state = u_v1.state;
	u->nr_data_types = ARRAY_SIZE(u_v1.d);
	u->bucket_size = u_v1.bucket_size;
	u->nr_buckets = u_v1.nr_buckets;

	for (unsigned i = 0; i < ARRAY_SIZE(u_v1.d); i++)
		u->d[i] = u_v1.d[i];

	return u;
}