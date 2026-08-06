/*
 * Filesystem handles (ioctl, sysfs dir).
 *
 * Ported from src/wrappers/handle.rs.
 */

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>

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
#include <time.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include <linux/fs.h>
#include <linux/page.h>
#include <linux/mm.h>

#include "libbcachefs.h"
#include "alloc/buckets.h"
#include "sb/io.h"

/* Global control device: */
int bcachectl_open(void)
{
	return xopen("/dev/bcachefs-ctl", O_RDWR);
}

/* Filesystem handles (ioctl, sysfs dir): */

/**
 * bcache_fs_close - Close a filesystem handle
 * @fs: Handle to close
 */
void bcache_fs_close(struct bchfs_handle fs)
{
	if (fs.ioctl_fd >= 0)
		xclose(fs.ioctl_fd);
	if (fs.sysfs_fd >= 0)
		xclose(fs.sysfs_fd);
}

/**
 * bcache_fs_open_by_name - Open a filesystem by its sysfs name (UUID string)
 * @name: sysfs name
 * @fs: Handle to fill
 *
 * Reads minor number, opens /dev/bcachefsN-ctl.
 */
static int bcache_fs_open_by_name(const char *name, struct bchfs_handle *fs)
{
	if (uuid_parse(name, fs->uuid.b))
		memset(&fs->uuid, 0, sizeof(fs->uuid));

	char *sysfs = mprintf(SYSFS_BASE "%s", name);
	fs->sysfs_fd = open(sysfs, O_RDONLY);
	free(sysfs);

	if (fs->sysfs_fd < 0)
		return -errno;

	char *minor = read_file_str(fs->sysfs_fd, "minor");
	if (!minor) {
		xclose(fs->sysfs_fd);
		return -EIO;
	}

	char *ctl = mprintf("/dev/bcachefs%s-ctl", minor);
	fs->ioctl_fd = open(ctl, O_RDWR);
	free(minor);
	free(ctl);

	if (fs->ioctl_fd < 0) {
		int ret = -errno;
		xclose(fs->sysfs_fd);
		return ret;
	}

	return 0;
}

#ifndef FS_IOC_GETFSSYSFSPATH
struct fs_sysfs_path {
	__u8 len;
	__u8 name[128];
};
#define FS_IOC_GETFSSYSFSPATH _IOR(0x15, 1, struct fs_sysfs_path)
#endif

/**
 * bcache_fs_open_fallible - Open a bcachefs filesystem
 * @path: UUID string, mountpoint, or device path
 * @fs: Handle to fill
 *
 * Returns 0 on success, or negative error code.
 */
int bcache_fs_open_fallible(const char *path, struct bchfs_handle *fs)
{
	memset(fs, 0, sizeof(*fs));
	fs->dev_idx = -1;

	/* Try as UUID string first */
	if (!uuid_parse(path, fs->uuid.b))
		return bcache_fs_open_by_name(path, fs);

	/* It's a path — open it */
	int path_fd = open(path, O_RDONLY);
	if (path_fd < 0)
		return -errno;

	/* Try BCH_IOCTL_QUERY_UUID — if it succeeds, it's a mounted fs path */
	struct bch_ioctl_query_uuid query_uuid;
	if (!ioctl(path_fd, BCH_IOCTL_QUERY_UUID, &query_uuid)) {
		fs->ioctl_fd = path_fd;
		fs->uuid = query_uuid.uuid;

		/* Try FS_IOC_GETFSSYSFSPATH to get sysfs path */
		struct fs_sysfs_path fs_sysfs_path;
		if (!ioctl(path_fd, FS_IOC_GETFSSYSFSPATH, &fs_sysfs_path)) {
			char *sysfs = mprintf("/sys/fs/%s", fs_sysfs_path.name);
			fs->sysfs_fd = open(sysfs, O_RDONLY);
			free(sysfs);
		} else {
			/* Fallback: use UUID */
			char uuid_str[40];
			uuid_unparse(query_uuid.uuid.b, uuid_str);

			char *sysfs = mprintf(SYSFS_BASE "%s", uuid_str);
			fs->sysfs_fd = open(sysfs, O_RDONLY);
			free(sysfs);
		}

		if (fs->sysfs_fd < 0) {
			int ret = -errno;
			xclose(fs->ioctl_fd);
			return ret;
		}

		return 0;
	}

	/* stat the path to distinguish block device vs file */
	struct stat stat;
	if (fstat(path_fd, &stat)) {
		int ret = -errno;
		xclose(path_fd);
		return ret;
	}
	xclose(path_fd);

	char buf[1024], *uuid_str;

	if (S_ISBLK(stat.st_mode)) {
		/* Block device: try sysfs symlink */
		char *sysfs = mprintf("/sys/dev/block/%u:%u/bcachefs",
				      major(stat.st_rdev), minor(stat.st_rdev));

		ssize_t len = readlink(sysfs, buf, sizeof(buf));
		free(sysfs);

		if (len > 0 && len < (ssize_t)sizeof(buf)) {
			buf[len] = '\0';
			/* target looks like "../../fs/bcachefs/<uuid>/dev-N" */
			char *p = strrchr(buf, '/');
			if (p && sscanf(p + 1, "dev-%u", &fs->dev_idx) == 1) {
				*p = '\0';
				p = strrchr(buf, '/');
				if (p) {
					uuid_str = p + 1;
					return bcache_fs_open_by_name(uuid_str, fs);
				}
			}
		}
	}

	/* Fallback: read superblock to get UUID */
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, noexcl, true);
	opt_set(opts, nochanges, true);

	struct bch_sb_handle sb;
	int ret = bch2_read_super(path, &opts, &sb);
	if (ret)
		return ret;

	fs->dev_idx = sb.sb->dev_idx;
	fs->uuid = sb.sb->user_uuid;
	uuid_unparse(sb.sb->user_uuid.b, buf);
	uuid_str = buf;

	bch2_free_super(&sb);

	return bcache_fs_open_by_name(uuid_str, fs);
}

/**
 * bcache_fs_open_if_mounted - Open the filesystem a path belongs to, if it's
 * currently mounted: a UUID, a path on a mounted filesystem, or a block device
 * that's a member of one.
 *
 * Returns 0 if mounted (@fs filled), 1 if the path doesn't resolve to a
 * mounted filesystem, or a negative error code on a real error (e.g. EACCES
 * on the ctl device) - which must not be mistaken for "not mounted", or
 * callers fall back to offline superblock access on a live filesystem.
 *
 * Regular files are never resolved: a filesystem image is not itself a
 * mounted filesystem, and an image stored on a mounted bcachefs would
 * otherwise resolve to the outer filesystem. Callers treat images as offline
 * superblocks.
 *
 * Ported from src/wrappers/handle.rs:open_if_mounted.
 */
int bcache_fs_open_if_mounted(const char *path, struct bchfs_handle *fs)
{
	memset(fs, 0, sizeof(*fs));
	fs->dev_idx = -1;

	/*
	 * Try as UUID string first (normalized: the sysfs dir is canonical
	 * lowercase-with-dashes, the user's spelling may not be). No sysfs dir
	 * for the UUID means not mounted; any other error (e.g. EACCES on the
	 * ctl device) is real and must not be mistaken for "not mounted":
	 */
	if (!uuid_parse(path, fs->uuid.b)) {
		char uuid_str[40];
		uuid_unparse(fs->uuid.b, uuid_str);
		int ret = bcache_fs_open_by_name(uuid_str, fs);
		if (!ret)
			return 0;
		if (ret == -ENOENT)
			return 1;
		return ret;
	}

	/* It's a path - open it; failure means not mounted: */
	int path_fd = open(path, O_RDONLY);
	if (path_fd < 0)
		return 1;

	struct stat stat;
	if (fstat(path_fd, &stat)) {
		int ret = -errno;
		xclose(path_fd);
		return ret;
	}

	/* Regular files are never resolved to a mounted filesystem: */
	if (S_ISREG(stat.st_mode)) {
		xclose(path_fd);
		return 1;
	}

	/* Try BCH_IOCTL_QUERY_UUID - if it succeeds, it's a mounted fs path */
	struct bch_ioctl_query_uuid query_uuid;
	if (!ioctl(path_fd, BCH_IOCTL_QUERY_UUID, &query_uuid)) {
		fs->ioctl_fd = path_fd;
		fs->uuid = query_uuid.uuid;

		/* Try FS_IOC_GETFSSYSFSPATH to get sysfs path */
		struct fs_sysfs_path fs_sysfs_path;
		if (!ioctl(path_fd, FS_IOC_GETFSSYSFSPATH, &fs_sysfs_path)) {
			char *sysfs = mprintf("/sys/fs/%s", fs_sysfs_path.name);
			fs->sysfs_fd = open(sysfs, O_RDONLY);
			free(sysfs);
		} else {
			/* Fallback: use UUID */
			char uuid_str[40];
			uuid_unparse(query_uuid.uuid.b, uuid_str);

			char *sysfs = mprintf(SYSFS_BASE "%s", uuid_str);
			fs->sysfs_fd = open(sysfs, O_RDONLY);
			free(sysfs);
		}

		if (fs->sysfs_fd < 0) {
			int ret = -errno;
			xclose(fs->ioctl_fd);
			return ret;
		}

		return 0;
	}

	xclose(path_fd);

	if (S_ISBLK(stat.st_mode)) {
		/* Block device: try sysfs symlink */
		char *sysfs = mprintf("/sys/dev/block/%u:%u/bcachefs",
				      major(stat.st_rdev), minor(stat.st_rdev));

		char buf[1024], *uuid_str;
		ssize_t len = readlink(sysfs, buf, sizeof(buf));
		free(sysfs);

		if (len > 0 && len < (ssize_t)sizeof(buf)) {
			buf[len] = '\0';
			/* target looks like "../../fs/bcachefs/<uuid>/dev-N" */
			char *p = strrchr(buf, '/');
			if (p && sscanf(p + 1, "dev-%u", &fs->dev_idx) == 1) {
				*p = '\0';
				p = strrchr(buf, '/');
				if (p) {
					uuid_str = p + 1;
					uuid_parse(uuid_str, fs->uuid.b);
					int ret = bcache_fs_open_by_name(uuid_str, fs);
					if (ret == -ENOENT)
						return 1;
					return ret;
				}
			}
		}
	}

	return 1;
}

/**
 * bcache_fs_open_if_mounted_any - Multi-device form of
 * bcache_fs_open_if_mounted(): open the filesystem if any of the given paths
 * resolves to a mounted one.
 */
int bcache_fs_open_if_mounted_any(const char **paths, unsigned nr,
				  struct bchfs_handle *fs)
{
	for (unsigned i = 0; i < nr; i++) {
		int ret = bcache_fs_open_if_mounted(paths[i], fs);
		if (ret != 1)
			return ret;
	}
	return 1;
}

/**
 * bchu_read_super - Read the filesystem superblock via BCH_IOCTL_READ_SUPER.
 *
 * Returns a heap-allocated buffer containing the raw superblock.
 * Retries with a larger buffer if the kernel returns ERANGE.
 */
void *bchu_read_super(struct bchfs_handle fs, u64 *size)
{
	u64 s = 4096;

	while (1) {
		void *buf = xmalloc(s);
		struct bch_ioctl_read_super arg = {
			.size = s,
			.sb = (unsigned long)buf,
		};

		if (!ioctl(fs.ioctl_fd, BCH_IOCTL_READ_SUPER, &arg)) {
			*size = s;
			return buf;
		}

		free(buf);
		if (errno == ERANGE && s < 1 << 20) {
			s *= 4;
			continue;
		}

		die("ioctl BCH_IOCTL_READ_SUPER error: %m");
	}
}

/**
 * bchu_sb_version - Read the on-disk metadata version from the filesystem
 * superblock.
 */
u16 bchu_sb_version(struct bchfs_handle fs)
{
	u64 size;
	struct bch_sb *sb = bchu_read_super(fs, &size);
	u16 version = le16_to_cpu(sb->version);
	free(sb);
	return version;
}

/**
 * bcache_fs_open - Open a bcachefs filesystem or die
 * @path: UUID string, mountpoint, or device path
 */
struct bchfs_handle bcache_fs_open(const char *path)
{
	struct bchfs_handle fs;
	int ret = bcache_fs_open_fallible(path, &fs);
	if (ret)
		die("Error opening filesystem at %s: %s", path, strerror(-ret));
	return fs;
}

/**
 * bchu_fs_open_by_dev - Given a path to a block device, open the filesystem it
 * belongs to; also return the device's idx.
 */
struct bchfs_handle bchu_fs_open_by_dev(const char *path, int *idx)
{
	struct bchfs_handle fs;
	int ret = bcache_fs_open_fallible(path, &fs);
	if (ret)
		die("Error opening filesystem at %s: %s", path, strerror(-ret));
	*idx = fs.dev_idx;
	return fs;
}

/**
 * bchu_dev_path_to_idx - Get device index for a path
 */
int bchu_dev_path_to_idx(struct bchfs_handle fs, const char *dev_path)
{
	int idx;
	struct bchfs_handle fs2 = bchu_fs_open_by_dev(dev_path, &idx);

	if (memcmp(&fs.uuid, &fs2.uuid, sizeof(fs.uuid)))
		idx = -1;
	bcache_fs_close(fs2);
	return idx;
}

/**
 * bchu_data - Perform data operation with progress reporting
 */
int bchu_data(struct bchfs_handle fs, struct bch_ioctl_data cmd)
{
	int progress_fd = xioctl(fs.ioctl_fd, BCH_IOCTL_DATA, &cmd);

	while (1) {
		struct bch_ioctl_data_event e;

		if (read(progress_fd, &e, sizeof(e)) != sizeof(e))
			die("error reading from progress fd %m");

		if (e.type)
			continue;

		if (e.ret || e.p.data_type == U8_MAX)
			break;

		printf("\33[2K\r");

		printf("%llu%% complete: current position %s",
		       e.p.sectors_total ?
			       e.p.sectors_done * 100 / e.p.sectors_total :
				     0,
		       bch2_data_type_str(e.p.data_type));

		switch (e.p.data_type) {
		case BCH_DATA_btree:
		case BCH_DATA_user:
			printf(" %s:%llu:%llu", bch2_btree_id_str(e.p.btree_id),
			       e.p.pos.inode, e.p.pos.offset);
		}

		fflush(stdout);
		sleep(1);
	}
	printf("\nDone\n");

	xclose(progress_fd);
	return 0;
}

/**
 * bchu_fs_get_devices - Get list of devices in a filesystem
 */
dev_names bchu_fs_get_devices(struct bchfs_handle fs)
{
	return bchu_fs_get_devices_mode(fs, DEVICE_NAME_RAW);
}

dev_names bchu_fs_get_devices_mode(struct bchfs_handle fs,
				   enum device_name_mode mode)
{
	char *sysfs_path = sysfs_path_from_fd(fs.sysfs_fd);
	dev_names devs = { 0 };

	if (sysfs_path) {
		devs = fs_get_devices(sysfs_path, mode);

		/* Also fill uuid from sysfs when available. */
		darray_for_each(devs, n)
		{
			char *uuid_attr = mprintf("dev-%u/uuid", n->idx);
			char *uuid_str = read_file_str(fs.sysfs_fd, uuid_attr);
			if (uuid_str) {
				uuid_parse(uuid_str, n->uuid);
				free(uuid_str);
			}
			free(uuid_attr);
		}
		free(sysfs_path);
	}

	return devs;
}

/**
 * dev_idx_to_name - Find device info by index
 */
struct dev_name *dev_idx_to_name(dev_names *dev_names, unsigned idx)
{
	darray_for_each(*dev_names, dev) if (dev->idx == idx) return dev;
	return NULL;
}

/**
 * bchu_disk_add - Add a device to the filesystem
 */
void bchu_disk_add(struct bchfs_handle fs, const char *dev)
{
	struct bch_ioctl_disk_v2 v2 = { .dev = (unsigned long)dev };
	struct bch_ioctl_disk v1 = { .dev = (unsigned long)dev };

	xbchu_ioctl(fs, BCH_IOCTL_DISK_ADD_v2, BCH_IOCTL_DISK_ADD, v2, v1);
}

/**
 * bchu_disk_remove - Remove a device from the filesystem
 */
void bchu_disk_remove(struct bchfs_handle fs, unsigned dev_idx, unsigned flags)
{
	struct bch_ioctl_disk_v2 v2 = {
		.flags = flags | BCH_BY_INDEX,
		.dev = dev_idx,
	};
	struct bch_ioctl_disk v1 = {
		.flags = flags | BCH_BY_INDEX,
		.dev = dev_idx,
	};

	xbchu_ioctl(fs, BCH_IOCTL_DISK_REMOVE_v2, BCH_IOCTL_DISK_REMOVE, v2,
		    v1);
}

/**
 * bchu_disk_online - Re-add an offline device
 */
void bchu_disk_online(struct bchfs_handle fs, const char *dev)
{
	struct bch_ioctl_disk_v2 v2 = { .dev = (unsigned long)dev };
	struct bch_ioctl_disk v1 = { .dev = (unsigned long)dev };

	xbchu_ioctl(fs, BCH_IOCTL_DISK_ONLINE_v2, BCH_IOCTL_DISK_ONLINE, v2,
		    v1);
}

/**
 * bchu_disk_offline - Take a device offline
 */
void bchu_disk_offline(struct bchfs_handle fs, unsigned dev_idx, unsigned flags)
{
	struct bch_ioctl_disk_v2 v2 = {
		.flags = flags | BCH_BY_INDEX,
		.dev = dev_idx,
	};
	struct bch_ioctl_disk v1 = {
		.flags = flags | BCH_BY_INDEX,
		.dev = dev_idx,
	};

	xbchu_ioctl(fs, BCH_IOCTL_DISK_OFFLINE_v2, BCH_IOCTL_DISK_OFFLINE, v2,
		    v1);
}

/**
 * bchu_disk_set_state - Change device state
 */
void bchu_disk_set_state(struct bchfs_handle fs, unsigned dev,
			 unsigned new_state, unsigned flags)
{
	struct bch_ioctl_disk_set_state_v2 v2 = {
		.flags = flags | BCH_BY_INDEX,
		.new_state = new_state,
		.dev = dev,
	};
	struct bch_ioctl_disk_set_state v1 = {
		.flags = flags | BCH_BY_INDEX,
		.new_state = new_state,
		.dev = dev,
	};

	xbchu_ioctl(fs, BCH_IOCTL_DISK_SET_STATE_v2, BCH_IOCTL_DISK_SET_STATE,
		    v2, v1);
}

/**
 * bchu_disk_resize - Resize filesystem on a device
 */
void bchu_disk_resize(struct bchfs_handle fs, unsigned dev_idx, u64 nbuckets)
{
	struct bch_ioctl_disk_resize_v2 v2 = {
		.flags = BCH_BY_INDEX,
		.dev = dev_idx,
		.nbuckets = nbuckets,
	};
	struct bch_ioctl_disk_resize v1 = {
		.flags = BCH_BY_INDEX,
		.dev = dev_idx,
		.nbuckets = nbuckets,
	};

	xbchu_ioctl(fs, BCH_IOCTL_DISK_RESIZE_v2, BCH_IOCTL_DISK_RESIZE, v2,
		    v1);
}

/**
 * bchu_disk_resize_journal - Resize journal on a device
 */
void bchu_disk_resize_journal(struct bchfs_handle fs, unsigned dev_idx,
			      u64 nbuckets)
{
	struct bch_ioctl_disk_resize_journal_v2 v2 = {
		.flags = BCH_BY_INDEX,
		.dev = dev_idx,
		.nbuckets = nbuckets,
	};
	struct bch_ioctl_disk_resize_journal v1 = {
		.flags = BCH_BY_INDEX,
		.dev = dev_idx,
		.nbuckets = nbuckets,
	};

	xbchu_ioctl(fs, BCH_IOCTL_DISK_RESIZE_JOURNAL_v2,
		    BCH_IOCTL_DISK_RESIZE_JOURNAL, v2, v1);
}

/**
 * bchu_subvolume_create - Create a subvolume
 */
int bchu_subvolume_create(struct bchfs_handle fs, const char *path)
{
	struct bch_ioctl_subvolume_v2 v2 = {
		.dirfd = AT_FDCWD,
		.mode = 0777,
		.dst_ptr = (unsigned long)path,
	};
	struct bch_ioctl_subvolume v1 = {
		.dirfd = AT_FDCWD,
		.mode = 0777,
		.dst_ptr = (unsigned long)path,
	};

	xbchu_ioctl(fs, BCH_IOCTL_SUBVOLUME_CREATE_v2,
		    BCH_IOCTL_SUBVOLUME_CREATE, v2, v1);
	return 0;
}

/**
 * bchu_subvolume_destroy - Delete a subvolume
 */
int bchu_subvolume_destroy(struct bchfs_handle fs, const char *path)
{
	struct bch_ioctl_subvolume_v2 v2 = {
		.dirfd = AT_FDCWD,
		.dst_ptr = (unsigned long)path,
	};
	struct bch_ioctl_subvolume v1 = {
		.dirfd = AT_FDCWD,
		.dst_ptr = (unsigned long)path,
	};

	xbchu_ioctl(fs, BCH_IOCTL_SUBVOLUME_DESTROY_v2,
		    BCH_IOCTL_SUBVOLUME_DESTROY, v2, v1);
	return 0;
}

/**
 * bchu_subvolume_snapshot - Create a snapshot
 */
int bchu_subvolume_snapshot(struct bchfs_handle fs, const char *src,
			    const char *dst, unsigned flags)
{
	struct bch_ioctl_subvolume_v2 v2 = {
		.flags = BCH_SUBVOL_SNAPSHOT_CREATE | flags,
		.dirfd = AT_FDCWD,
		.mode = 0777,
		.dst_ptr = (unsigned long)dst,
		.src_ptr = (unsigned long)src,
	};
	struct bch_ioctl_subvolume v1 = {
		.flags = BCH_SUBVOL_SNAPSHOT_CREATE | flags,
		.dirfd = AT_FDCWD,
		.mode = 0777,
		.dst_ptr = (unsigned long)dst,
		.src_ptr = (unsigned long)src,
	};

	xbchu_ioctl(fs, BCH_IOCTL_SUBVOLUME_CREATE_v2,
		    BCH_IOCTL_SUBVOLUME_CREATE, v2, v1);
	return 0;
}
