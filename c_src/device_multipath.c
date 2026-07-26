/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Helpers for detecting device-mapper multipath relationships.
 *
 * The core entrypoints are find_multipath_holder(), which walks sysfs
 * holders to determine whether a block device sits under a dm-multipath map,
 * and preferred_multipath_devnode(), which normalizes dm-multipath devices
 * to their /dev/mapper/ path when that path exists and refers to the same
 * device.
 * Both top-level maps (mpath-...) and partition maps
 * (partN-mpath-..., including nested partition prefixes) are treated as
 * multipath.
 *
 * Ported from src/device_multipath.rs.
 */

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libbcachefs.h"

#define MAX_MULTIPATH_DEPTH 8

bool is_multipath_dm_uuid(const char *uuid)
{
	const char *rest = uuid;

	while (rest && *rest) {
		if (!strncmp(rest, "mpath-", 6))
			return true;

		if (strncmp(rest, "part", 4))
			return false;

		rest += 4;
		if (!isdigit(*rest))
			return false;

		while (isdigit(*rest))
			rest++;

		if (*rest != '-')
			return false;

		rest++;
	}

	return false;
}

static char *read_sysfs_attr(const char *dir, const char *attr)
{
	char *path = mprintf("%s/%s", dir, attr);
	char *val = read_file_str(AT_FDCWD, path);

	free(path);
	if (!val)
		return NULL;

	/* trim trailing whitespace/newlines */
	size_t len = strlen(val);
	while (len && (val[len - 1] == '\n' || val[len - 1] == '\r' ||
		       val[len - 1] == ' ' || val[len - 1] == '\t'))
		val[--len] = '\0';

	return val;
}

/*
 * True if a sysfs block directory is a dm-multipath map (or map partition),
 * per its dm UUID — membership is decided on the UUID alone.
 */
static bool is_multipath_dm(const char *block_sysfs)
{
	char *dm_dir = mprintf("%s/dm", block_sysfs);
	char *uuid = read_sysfs_attr(dm_dir, "uuid");
	bool ret = uuid && is_multipath_dm_uuid(uuid);

	free(uuid);
	free(dm_dir);
	return ret;
}

static char *multipath_dm_name_from_sysfs(const char *block_sysfs)
{
	if (!is_multipath_dm(block_sysfs))
		return NULL;

	char *dm_dir = mprintf("%s/dm", block_sysfs);
	char *name = read_sysfs_attr(dm_dir, "name");

	free(dm_dir);
	return name;
}

/* The dev_t of a sysfs block directory, parsed from its major:minor attr. */
static bool sysfs_block_dev(const char *block_sysfs, dev_t *out)
{
	char *dev = read_sysfs_attr(block_sysfs, "dev");
	unsigned major_n, minor_n;
	bool ok = false;

	if (dev && sscanf(dev, "%u:%u", &major_n, &minor_n) == 2) {
		*out = makedev(major_n, minor_n);
		ok = true;
	}
	free(dev);
	return ok;
}

/*
 * /dev/mapper/<dm_name>, but only if it exists AND resolves to expect_dev.
 * The rdev match is the safety property: we substitute a friendlier name
 * for a device, we never redirect to a different one.
 */
static char *mapper_path_if_matches(const char *dm_name, dev_t expect_dev)
{
	char *mapper_path = mprintf("/dev/mapper/%s", dm_name);
	struct stat st;

	if (stat(mapper_path, &st) < 0 || !S_ISBLK(st.st_mode) ||
	    st.st_rdev != expect_dev) {
		free(mapper_path);
		return NULL;
	}
	return mapper_path;
}

static char *preferred_multipath_devnode_from_sysfs(const char *block_sysfs)
{
	char *dm_name = multipath_dm_name_from_sysfs(block_sysfs);
	dev_t dev;
	char *ret = NULL;

	if (!dm_name)
		return NULL;
	if (!sysfs_block_dev(block_sysfs, &dev)) {
		free(dm_name);
		return NULL;
	}

	ret = mapper_path_if_matches(dm_name, dev);
	free(dm_name);
	return ret;
}

char *preferred_multipath_devnode(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0 || !S_ISBLK(st.st_mode) || !st.st_rdev)
		return NULL;

	char *sysfs = mprintf("/sys/dev/block/%u:%u", major(st.st_rdev),
			      minor(st.st_rdev));
	char *ret = preferred_multipath_devnode_from_sysfs(sysfs);

	free(sysfs);
	return ret;
}

char *preferred_multipath_devnode_for_block_name(const char *name)
{
	if (strncmp(name, "dm-", 3))
		return NULL;

	char *sysfs = mprintf("/sys/block/%s", name);
	char *ret = preferred_multipath_devnode_from_sysfs(sysfs);

	free(sysfs);
	return ret;
}

static char *find_multipath_holder_inner(const char *path, unsigned depth)
{
	if (depth >= MAX_MULTIPATH_DEPTH) {
		fprintf(stderr,
			"Warning: Reached maximum multipath holder depth (%u) at %s.\n"
			"This may indicate a circular holder relationship or unusually deep device stacking.\n",
			MAX_MULTIPATH_DEPTH, path);
		return NULL;
	}

	struct stat statbuf;
	if (stat(path, &statbuf) < 0 || !S_ISBLK(statbuf.st_mode) ||
	    !statbuf.st_rdev)
		return NULL;

	char *sysfs_holders_path = mprintf("/sys/dev/block/%u:%u/holders",
					   major(statbuf.st_rdev),
					   minor(statbuf.st_rdev));

	DIR *dir = opendir(sysfs_holders_path);
	free(sysfs_holders_path);
	if (!dir)
		return NULL;

	char *ret = NULL;
	struct dirent *d;
	while ((d = readdir(dir))) {
		if (strncmp(d->d_name, "dm-", 3))
			continue;

		char *holder_sysfs = mprintf("/sys/block/%s", d->d_name);
		if (!is_multipath_dm(holder_sysfs)) {
			free(holder_sysfs);
			continue;
		}

		/*
		 * Prefer /dev/mapper/<name>; fall back to /dev/dm-N when it's
		 * absent, unverifiable, or the dm name attribute is unreadable.
		 */
		ret = preferred_multipath_devnode_from_sysfs(holder_sysfs);
		if (!ret)
			ret = mprintf("/dev/%s", d->d_name);
		free(holder_sysfs);
		break;
	}

	closedir(dir);

	if (ret) {
		char *higher = find_multipath_holder_inner(ret, depth + 1);
		if (higher) {
			free(ret);
			ret = higher;
		}
	}

	return ret;
}

char *find_multipath_holder(const char *path)
{
	return find_multipath_holder_inner(path, 0);
}

void warn_multipath_component(const char *path, const char *mpath_dev)
{
	fprintf(stderr,
		"Warning: %s appears to be a multipath component device.\n",
		path);
	fprintf(stderr, "Consider using the multipath device (%s) instead.\n",
		mpath_dev);
}
