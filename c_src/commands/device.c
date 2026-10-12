/*
 * device: Manage devices within a filesystem.
 *
 * Supports adding, removing, resizing, and changing the state of devices.
 * Also includes data evacuation and online/offline transitions.
 *
 * Ported from src/commands/device.rs.
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "sb/members.h"
#include "sb/io.h"
#include "init/fs.h"
#include "init/dev.h"
#include "journal/init.h"
#include "cmds.h"

/* Helpers */

static int open_dev(const char *path, struct bchfs_handle *fs, int *dev_idx)
{
	*fs = bcache_fs_open(path);
	*dev_idx = fs->dev_idx;
	if (*dev_idx < 0) {
		fprintf(stderr,
			"'%s' does not appear to be a block device member\n",
			path);
		return -1;
	}
	return 0;
}

static int resolve_dev(struct bchfs_handle fs, const char *dev_str)
{
	char *end;
	int idx = strtol(dev_str, &end, 10);
	if (*dev_str && !*end)
		return idx;

	struct bchfs_handle dev_fs;
	if (bcache_fs_open_fallible(dev_str, &dev_fs))
		die("opening '%s': %m", dev_str);

	if (!uuid_equal(&fs.uuid, &dev_fs.uuid))
		die("%s does not appear to be a member of this filesystem",
		    dev_str);

	idx = dev_fs.dev_idx;
	bcache_fs_close(dev_fs);

	if (idx < 0)
		die("Could not determine device index for '%s'", dev_str);

	return idx;
}

static int open_dev_by_path_or_index(const char *device, const char *fs_path,
				     struct bchfs_handle *fs, int *dev_idx)
{
	if (fs_path) {
		*fs = bcache_fs_open(fs_path);
		*dev_idx = resolve_dev(*fs, device);
		return 0;
	}

	char *end;
	(void)strtol(device, &end, 10);
	if (*device && !*end)
		die("Filesystem path required when specifying device by index");

	return open_dev(device, fs, dev_idx);
}

static u64 get_device_size(const char *dev)
{
	int fd = xopen(dev, O_RDONLY);
	u64 size = get_size(fd);
	close(fd);
	return size;
}

/* add */

static void device_add_usage(void)
{
	puts("bcachefs device add - add a new device to an existing filesystem\n"
	     "Usage: bcachefs device add [OPTION]... <filesystem> <device>\n"
	     "\n"
	     "Options:");
	bch2_opts_usage(OPT_FORMAT | OPT_DEVICE, 0);
	puts("  -l, --label=label           Disk label\n"
	     "  -f, --force                 Use device even if it appears to already be formatted\n"
	     "  -h, --help                  Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int device_add_format(const char *dev_path, bool force,
			     struct bch_opt_strs opts,
			     u32 block_size, u32 btree_node_size)
{
	struct dev_opts dev_opts = dev_opts_default();
	dev_opts.path = dev_path;

	for (unsigned i = 0; i < bch2_opts_nr; i++) {
		if (!opts.by_id[i])
			continue;

		const struct bch_option *opt = bch2_opt_table + i;
		u64 v;
		struct printbuf err = PRINTBUF;
		int ret = bch2_opt_parse(NULL, opt, opts.by_id[i], &v, &err);
		printbuf_exit(&err);
		if (ret == -BCH_ERR_option_needs_open_fs) {
			/* Values that resolve against a superblock (labels) -
			 * the new device's sb, once format_for_device_add
			 * builds it: */
			dev_opt_str_push(&dev_opts, i, opts.by_id[i]);
		} else if (ret < 0) {
			die("invalid option %s: %s", opt->attr.name,
			    opts.by_id[i]);
		} else {
			bch2_opt_set_by_id(&dev_opts.opts, i, v);
		}
	}

	/*
	 * Honor explicit user-supplied paths, but warn when a path appears to be
	 * a multipath component because that is typically unintended.
	 */
	{
		char *mpath = find_multipath_holder(dev_path);
		if (mpath) {
			warn_multipath_component(dev_path, mpath);
			free(mpath);
			if (!force)
				die("device appears to be a multipath component, use -f/--force to add anyway");
		}
	}

	int ret = open_for_format(&dev_opts, 0, force);
	if (ret)
		die("error opening %s: %s", dev_path, strerror(-ret));

	ret = bch2_format_for_device_add(&dev_opts, block_size,
					 btree_node_size);
	if (ret)
		die("error formatting %s: %s", dev_path, bch2_err_str(ret));

	/* Refresh udev so the new member is discoverable immediately. */
	{
		char *cmd = mprintf("udevadm trigger --settle %s", dev_path);
		if (system(cmd)) {
			/* best-effort */
		}
		free(cmd);
	}

	return 0;
}

static int cmd_device_add_online(struct bchfs_handle fs, const char *dev_path,
				 bool force, struct bch_opt_strs opts)
{
	u32 block_size = read_file_u64(fs.sysfs_fd, "options/block_size");
	u32 btree_node_size =
		read_file_u64(fs.sysfs_fd, "options/btree_node_size");

	device_add_format(dev_path, force, opts, block_size,
			  btree_node_size);
	bchu_disk_add(fs, dev_path);

	char *cmd = mprintf("udevadm trigger --settle %s", dev_path);
	if (system(cmd)) {
	}
	free(cmd);

	return 0;
}

static int cmd_device_add_offline(const char *fs_path, const char *dev_path,
				  bool force, struct bch_opt_strs opts)
{
	/*
	 * Discover all devices in a multi-device filesystem. When the user
	 * specifies a single device, scan for other members by UUID — same
	 * as mount does.
	 */
	char *devs_str = bch2_scan_devices(fs_path);
	if (!devs_str)
		die("no devices found for %s", fs_path);

	darray_const_str devs = { 0 };
	char *p = devs_str, *s;
	while ((s = strsep(&p, ":")))
		darray_push(&devs, s);

	struct bch_opts bch_opts = bch2_opts_empty();
	opt_set(bch_opts, nostart, true);
	opt_set(bch_opts, copygc_enabled, false);
	opt_set(bch_opts, reconcile_enabled, false);

	struct bch_fs *c = bch2_fs_open(&devs, &bch_opts, NULL);
	if (IS_ERR(c))
		die("opening filesystem '%s': %s", fs_path,
		    bch2_err_str(PTR_ERR(c)));

	u32 block_size = c->opts.block_size;
	u32 btree_node_size = c->opts.btree_node_size;

	device_add_format(dev_path, force, opts, block_size,
			  btree_node_size);

	CLASS(printbuf, err)();
	int ret = bch2_dev_add(c, dev_path, &err);
	if (ret)
		die("adding device '%s': %s", dev_path, err.buf);

	ret = bch2_fs_start(c);
	if (ret)
		die("starting filesystem: %s", bch2_err_str(ret));

	char *cmd = mprintf("udevadm trigger --settle %s", dev_path);
	if (system(cmd)) {
	}
	free(cmd);

	bch2_fs_stop(c);
	free(devs_str);
	darray_exit(&devs);
	return 0;
}

static int cmd_device_add(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "label", required_argument, NULL, 'l' },
		{ "force", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	const char *label = NULL;
	bool force = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "l:fh", longopts, NULL)) != -1)
		switch (opt) {
		case 'l':
			label = optarg;
			break;
		case 'f':
			force = true;
			break;
		case 'h':
			device_add_usage();
			exit(EXIT_SUCCESS);
		}

	struct bch_opt_strs opts =
		bch2_cmdline_opts_get(&argc, argv, OPT_FORMAT | OPT_DEVICE);
	if (label)
		opts.by_id[Opt_label] = strdup(label);
	args_shift(optind);

	const char *fs_path = arg_pop();
	if (!fs_path)
		die("Please supply a filesystem");

	const char *dev_path = arg_pop();
	if (!dev_path)
		die("Please supply a device");

	if (argc)
		die("too many arguments");

	struct bchfs_handle fs;
	int ret;
	if (bcache_fs_open_fallible(fs_path, &fs) == 0) {
		ret = cmd_device_add_online(fs, dev_path, force, opts);
		bcache_fs_close(fs);
	} else {
		ret = cmd_device_add_offline(fs_path, dev_path, force,
					     opts);
	}
	bch2_opt_strs_free(&opts);

	return ret;
}

/* remove */

static void device_remove_usage(void)
{
	puts("bcachefs device remove - remove a device from a filesystem\n"
	     "Usage: bcachefs device remove [OPTION]... <device> [filesystem-path]\n"
	     "\n"
	     "Options:\n"
	     "  -f, --force                 Force removal, even if some data couldn't be migrated\n"
	     "  -F, --force-metadata        Force removal, even if some metadata couldn't be migrated\n"
	     "  -h, --help                  Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int cmd_device_remove(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' },
		{ "force-metadata", no_argument, NULL, 'F' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	int flags = BCH_FORCE_IF_DEGRADED;
	int opt;

	while ((opt = getopt_long(argc, argv, "fFh", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			flags |= BCH_FORCE_IF_DATA_LOST;
			break;
		case 'F':
			flags |= BCH_FORCE_IF_METADATA_LOST;
			break;
		case 'h':
			device_remove_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	const char *device = arg_pop();
	if (!device)
		die("Please supply a device");

	const char *fs_path = arg_pop();

	struct bchfs_handle fs;
	int dev_idx;
	open_dev_by_path_or_index(device, fs_path, &fs, &dev_idx);

	bchu_disk_remove(fs, dev_idx, flags);
	bcache_fs_close(fs);
	return 0;
}

/* online / offline */

static int cmd_device_online(int argc, char *argv[])
{
	args_shift(1);
	const char *dev = arg_pop();
	if (!dev)
		die("Please supply a device");

	struct bchfs_handle fs = bcache_fs_open(dev);
	bchu_disk_online(fs, (char *)dev);
	bcache_fs_close(fs);

	char *cmd = mprintf("udevadm trigger --settle %s", dev);
	if (system(cmd)) {
	}
	free(cmd);

	return 0;
}

static int cmd_device_offline(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' }, { NULL }
	};
	int flags = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "f", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			flags |= BCH_FORCE_IF_DEGRADED;
			break;
		}
	args_shift(optind);

	const char *dev = arg_pop();
	if (!dev)
		die("Please supply a device");

	struct bchfs_handle fs;
	int dev_idx;
	if (open_dev(dev, &fs, &dev_idx))
		return -1;

	bchu_disk_offline(fs, dev_idx, flags);
	bcache_fs_close(fs);
	return 0;
}

/* set-state */

static void device_set_state_usage(void)
{
	puts("bcachefs device set-state - Set a device state (rw, ro, evacuating, or spare)\n"
	     "Usage: bcachefs device set-state [OPTION]... <new-state> <device> [filesystem-path]\n"
	     "\n"
	     "Device states: rw, ro, evacuating, spare\n"
	     "\n"
	     "Options:\n"
	     "  -f, --force                 Force if data redundancy will be degraded\n"
	     "  -F  --force-if-data-lost    Force even if data will be lost\n"
	     "  -o, --offline               Set state of an offline device\n"
	     "  -h, --help                  Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

static int set_state_offline(const char *device, unsigned new_state)
{
	char *devs_str = bch2_scan_devices(device);
	if (!devs_str)
		die("no devices found for %s", device);

	darray_const_str devs = { 0 };
	char *p = devs_str, *s;
	while ((s = strsep(&p, ":")))
		darray_push(&devs, s);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, true);
	opt_set(opts, will_not_start, true);
	opt_set(opts, degraded, BCH_DEGRADED_very);

	struct bch_fs *c = bch2_fs_open(&devs, &opts, NULL);
	if (IS_ERR(c))
		die("Error opening filesystem: %s", bch2_err_str(PTR_ERR(c)));

	struct bch_sb_handle sb;
	int ret = bch2_read_super(device, &opts, &sb);
	if (ret)
		die("error reading superblock: %s", bch2_err_str(ret));
	int dev_idx = sb.sb->dev_idx;
	bch2_free_super(&sb);

	if (!BCH_SB_INITIALIZED(c->disk_sb.sb))
		die("superblock not initialized (filesystem was never started): "
		    "bch2_write_super would silently skip the write; mount it once first");

	{
		guard(mutex_noio)(&c->sb_lock);
		/*
		 * Set the state in the superblock member itself, like Rust's
		 * fs.member_mut(dev_idx).set_member_state(): writing
		 * ca->mi.state only changed the in-memory copy -
		 * bch2_sb_members_from_cpu() syncs just the error counters
		 * back, so the new state never reached disk and the command
		 * was a no-op.
		 */
		struct bch_member *m =
			bch2_members_v2_get_mut(c->disk_sb.sb, dev_idx);
		/* this codebase's BITMASK setters are SET_<field>-prefixed
		 * (LE64_BITMASK generates SET_##name), as fs/init/dev.c
		 * uses them */
		SET_BCH_MEMBER_STATE(m, new_state);

		int wret = bch2_write_super(c);
		if (wret)
			die("error writing superblock: %s", bch2_err_str(wret));
	}

	bch2_fs_stop(c);
	free(devs_str);
	darray_exit(&devs);
	return 0;
}

static int cmd_device_set_state(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' },
		{ "force-if-data-lost", no_argument, NULL, 'F' },
		{ "offline", no_argument, NULL, 'o' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	int flags = 0;
	bool offline = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "fFoh", longopts, NULL)) != -1)
		switch (opt) {
		case 'f':
			flags |= BCH_FORCE_IF_DEGRADED;
			break;
		case 'F':
			flags |= BCH_FORCE_IF_DEGRADED |
				 BCH_FORCE_IF_DATA_LOST |
				 BCH_FORCE_IF_METADATA_LOST;
			break;
		case 'o':
			offline = true;
			break;
		case 'h':
			device_set_state_usage();
			exit(EXIT_SUCCESS);
		default:
			device_set_state_usage();
			exit(EXIT_FAILURE);
		}
	args_shift(optind);

	const char *new_state_str = arg_pop();
	if (!new_state_str)
		die("Please supply a device state");

	unsigned new_state = read_string_list_or_die(
		new_state_str, bch2_member_states, "device state");

	const char *device = arg_pop();
	if (!device)
		die("Please supply a device");

	if (offline) {
		char *end;
		(void)strtoul(device, &end, 10);
		if (*device && !*end)
			die("Cannot specify offline device by id");
		return set_state_offline(device, new_state);
	}

	const char *fs_path = arg_pop();
	struct bchfs_handle fs;
	int dev_idx;
	open_dev_by_path_or_index(device, fs_path, &fs, &dev_idx);

	bchu_disk_set_state(fs, dev_idx, new_state, flags);
	bcache_fs_close(fs);
	return 0;
}

/* resize */

static int cmd_device_resize(int argc, char *argv[])
{
	args_shift(1);
	const char *dev = arg_pop();
	if (!dev)
		die("Please supply a device");

	u64 size_bytes;
	const char *size_str = arg_pop();
	if (!size_str)
		size_bytes = get_device_size(dev);
	else if (bch2_strtoull_h(size_str, &size_bytes))
		die("invalid size");
	u64 size_sectors = size_bytes >> 9;

	struct bchfs_handle fs;
	int dev_idx;
	if (bcache_fs_open_fallible(dev, &fs) == 0 &&
	    (dev_idx = fs.dev_idx) >= 0) {
		printf("Doing online resize of %s\n", dev);
		struct bch_ioctl_dev_usage_v2 *u = bchu_dev_usage(fs, dev_idx);
		u64 nbuckets = size_sectors / u->bucket_size;

		printf("resizing %s to %llu buckets\n", dev, nbuckets);
		bchu_disk_resize(fs, dev_idx, nbuckets);
		free(u);
		bcache_fs_close(fs);
	} else {
		printf("Doing offline resize of %s\n", dev);
		char *devs_str = bch2_scan_devices(dev);
		if (!devs_str)
			die("no devices found for %s", dev);

		darray_const_str devs = { 0 };
		char *p = devs_str, *s;
		while ((s = strsep(&p, ":")))
			darray_push(&devs, s);

		struct bch_opts opts = bch2_opts_empty();
		struct bch_fs *c = bch2_fs_open(&devs, &opts, NULL);
		if (IS_ERR(c))
			die("error opening %s: %s", dev,
			    bch2_err_str(PTR_ERR(c)));

		struct bch_dev *ca = NULL;
		for_each_online_member(c, _ca, 0)
		{
			if (ca)
				die("multiple devices online, offline resize requires exactly one");
			ca = _ca;
		}
		if (!ca)
			die("no online device found");

		u64 nbuckets = size_sectors / ca->mi.bucket_size;
		bool shrinking = nbuckets < ca->mi.nbuckets;

		if (shrinking)
			die("shrinking not supported (requested %llu buckets, have %llu)",
			    (unsigned long long)nbuckets, (unsigned long long)ca->mi.nbuckets);

		printf("resizing to %llu buckets\n", nbuckets);
		struct printbuf err = PRINTBUF;
		int ret = bch2_dev_resize(c, ca, nbuckets, &err);
		if (ret) {
			char *msg = err.buf;
			err.buf = NULL;
			die("%s: %s\n%s", shrinking ? "shrinking device (requires kernel shrink support)" : "resizing device", bch2_err_str(ret), msg);
		}
		printbuf_exit(&err);

		bch2_fs_stop(c);
		free(devs_str);
		darray_exit(&devs);
	}
	return 0;
}

static int cmd_device_resize_journal(int argc, char *argv[])
{
	args_shift(1);
	const char *dev = arg_pop();
	const char *size_str = arg_pop();
	if (!dev || !size_str)
		die("Usage: bcachefs device resize-journal <device> <size>");

	u64 size_bytes;
	if (bch2_strtoull_h(size_str, &size_bytes))
		die("invalid size");
	u64 size_sectors = size_bytes >> 9;

	struct bchfs_handle fs;
	int dev_idx;
	if (bcache_fs_open_fallible(dev, &fs) == 0 &&
	    (dev_idx = fs.dev_idx) >= 0) {
		struct bch_ioctl_dev_usage_v2 *u = bchu_dev_usage(fs, dev_idx);
		u64 nbuckets = size_sectors / u->bucket_size;
		printf("resizing journal on %s to %llu buckets\n", dev,
		       nbuckets);
		bchu_disk_resize_journal(fs, dev_idx, nbuckets);
		free(u);
	} else {
		printf("%s is offline - starting:\n", dev);
		char *devs_str = bch2_scan_devices(dev);
		if (!devs_str)
			die("no devices found for %s", dev);

		darray_const_str devs = { 0 };
		char *p = devs_str, *s;
		while ((s = strsep(&p, ":")))
			darray_push(&devs, s);

		struct bch_opts opts = bch2_opts_empty();
		struct bch_fs *c = bch2_fs_open(&devs, &opts, NULL);
		if (IS_ERR(c))
			die("error opening %s: %s", dev,
			    bch2_err_str(PTR_ERR(c)));

		struct bch_dev *ca = NULL;
		for_each_online_member(c, _ca, 0)
		{
			if (ca)
				die("multiple devices online");
			ca = _ca;
		}
		if (!ca)
			die("no online device found");

		u64 nbuckets = size_sectors / ca->mi.bucket_size;
		printf("resizing journal to %llu buckets\n", nbuckets);
		int ret =
			bch2_set_nr_journal_buckets(c, ca, (unsigned)nbuckets);
		if (ret)
			die("resize error: %s", bch2_err_str(ret));

		bch2_fs_stop(c);
		free(devs_str);
		darray_exit(&devs);
	}
	return 0;
}

/* evacuate */

static int cmd_device_evacuate(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			puts("Usage: bcachefs device evacuate [OPTION]... <device>");
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (bcachefs_kernel_version() < bcachefs_metadata_version_reconcile)
		die("Kernel too old for evacuate path; need bcachefs metadata version >= %u",
		    bcachefs_metadata_version_reconcile);

	const char *dev_path = arg_pop();
	if (!dev_path)
		die("Please supply a device");

	struct bchfs_handle fs;
	int dev_idx;
	if (open_dev(dev_path, &fs, &dev_idx))
		return -1;

	/* Reconcile drives evacuation — check the filesystem has been upgraded */
	if (bchu_sb_version(fs) < bcachefs_metadata_version_reconcile)
		die("Filesystem has not been upgraded to the reconcile version.\n"
		    "Device evacuation requires reconcile. Remount with:\n"
		    "  mount -o remount,version_upgrade=incompatible <mountpoint>");

	struct bch_ioctl_dev_usage_v2 *u = bchu_dev_usage(fs, dev_idx);
	if (u->state == BCH_MEMBER_STATE_rw) {
		printf("Setting %s readonly\n", dev_path);
		/* Always use BCH_FORCE_IF_DEGRADED, matching Rust behavior */
		bchu_disk_set_state(fs, dev_idx, BCH_MEMBER_STATE_ro,
				    BCH_FORCE_IF_DEGRADED);
	}
	printf("Setting %s evacuating\n", dev_path);
	bchu_disk_set_state(fs, dev_idx, BCH_MEMBER_STATE_evacuating,
			    BCH_FORCE_IF_DEGRADED);
	free(u);

	write_file_str(fs.sysfs_fd, "internal/trigger_reconcile_wakeup", "1");

	while (1) {
		u = bchu_dev_usage(fs, dev_idx);
		u64 data_sectors = 0;
		for (unsigned i = 0; i < u->nr_data_types; i++) {
			if (data_type_is_empty(i) || data_type_is_hidden(i))
				continue;
			data_sectors += u->d[i].sectors;
		}

		struct printbuf buf = PRINTBUF;
		prt_human_readable_u64(&buf, data_sectors << 9);
		printf("\x1b[2K\r%s", buf.buf);
		fflush(stdout);
		printbuf_exit(&buf);

		if (data_sectors == 0) {
			printf("\nEvacuation complete — device ready to remove with 'bcachefs device remove %s'\n",
			       dev_path);
			free(u);
			break;
		}

		free(u);
		sleep(1);
	}

	return 0;
}

/*
 * Device discovery delegates to device_scan.c's scan, which reads
 * superblocks silently, filters out removed/diverged devices (splitbrain
 * check included) and dedups dev_idx before trusting a udev result. The
 * local copy this replaces predated all of that: it died on the first
 * non-bcachefs device on the machine and accepted stale members.
 */
static char *get_devices_by_uuid(uuid_t uuid, bool use_udev)
{
	struct bch_opts opts = bch2_opts_empty();
	bch_scanned_sbs sbs = { 0 };
	int ret = bch2_get_devices_by_uuid(uuid, &opts, use_udev, &sbs);

	if (ret)
		die("error scanning for devices: %s", bch2_err_str(ret));

	if (!sbs.nr) {
		bch2_scanned_sbs_exit(&sbs);
		return NULL;
	}

	char *res = bch2_scanned_joined_device_str(&sbs);
	bch2_scanned_sbs_exit(&sbs);
	return res;
}

static char *get_devices_by_label(const char *label, bool use_udev)
{
	struct bch_opts opts = bch2_opts_empty();
	bch_scanned_sbs sbs = { 0 };
	int ret = bch2_get_devices_by_label(label, &opts, use_udev, &sbs);

	/* Multiple filesystems: bch2_get_devices_by_label already said so. */
	if (ret == -EINVAL)
		return NULL;
	if (ret)
		die("error scanning for devices: %s", bch2_err_str(ret));

	if (!sbs.nr) {
		bch2_scanned_sbs_exit(&sbs);
		return NULL;
	}

	char *res = bch2_scanned_joined_device_str(&sbs);
	bch2_scanned_sbs_exit(&sbs);
	return res;
}

/*
 * bch2_scan_devices: Discover bcachefs devices for mount.
 *
 * Accepts a device path, UUID=, OLD_BLKID_UUID=, LABEL=, bare UUID, or a
 * colon-separated device list. Multi-device filesystems are discovered via
 * udev (fast) with a block-device scan fallback.
 *
 * Ported from src/device_scan.rs.
 */
char *bch2_scan_devices(const char *device_or_uuid)
{
	uuid_t uuid;
	bool use_udev = true; /* Default to true, same as Rust mount_trusts_udev */
	const char *uuid_str = NULL;

	if (!strncmp(device_or_uuid, "UUID=", 5))
		uuid_str = device_or_uuid + 5;
	else if (!strncmp(device_or_uuid, "OLD_BLKID_UUID=", 15))
		uuid_str = device_or_uuid + 15;

	if (uuid_str) {
		if (uuid_parse(uuid_str, uuid))
			return NULL;
		return get_devices_by_uuid(uuid, use_udev);
	}

	if (!strncmp(device_or_uuid, "LABEL=", 6))
		return get_devices_by_label(device_or_uuid + 6, use_udev);

	if (!uuid_parse(device_or_uuid, uuid))
		return get_devices_by_uuid(uuid, use_udev);

	/* Colon-separated explicit device list — honor as-is. */
	if (strchr(device_or_uuid, ':')) {
		char *copy = strdup(device_or_uuid);
		char *p = copy;
		char *tok;

		while ((tok = strsep(&p, ":"))) {
			char *mpath = find_multipath_holder(tok);
			if (mpath) {
				warn_multipath_component(tok, mpath);
				free(mpath);
			}
		}
		free(copy);
		return strdup(device_or_uuid);
	}

	/* Device path — warn on multipath components, then scan by UUID. */
	{
		char *mpath = find_multipath_holder(device_or_uuid);
		if (mpath) {
			warn_multipath_component(device_or_uuid, mpath);
			free(mpath);
		}
	}

	/*
	 * Silent read: this fallback must skip devices that are not bcachefs
	 * (or unreadable), like device_scan.rs read_super_silent().ok() -
	 * __bch2_super_read() dies on the first one, which aborted device
	 * add/resize on any machine with other filesystems installed.
	 */
	struct bch_opts scan_opts = bch2_opts_empty();
	struct bch_sb_handle sbh = { 0 };

	if (bch2_read_super_silent_opts(device_or_uuid, &scan_opts, &sbh))
		return strdup(device_or_uuid);

	uuid_copy(uuid, sbh.sb->user_uuid.b);
	bool single = sbh.sb->nr_devices == 1;
	bch2_free_super(&sbh);

	if (single)
		return strdup(device_or_uuid);

	return get_devices_by_uuid(uuid, use_udev);
}

int cmd_device_scan(int argc, char *argv[])
{
	if (argc < 2) {
		puts("Usage: bcachefs device scan <device|UUID=|LABEL=>");
		return -EINVAL;
	}

	char *res = bch2_scan_devices(argv[1]);
	if (res) {
		printf("%s\n", res);
		free(res);
	} else {
		die("no devices found");
	}
	return 0;
}

static int device_usage(void)
{
	puts("bcachefs device - manage devices within a running filesystem\n"
	     "Usage: bcachefs device <CMD> [OPTION]\n"
	     "\n"
	     "Commands:\n"
	     "  add                     add a new device to an existing filesystem\n"
	     "  remove                  remove a device from an existing filesystem\n"
	     "  online                  re-add an existing member to a filesystem\n"
	     "  offline                 take a device offline, without removing it\n"
	     "  evacuate                migrate data off a specific device\n"
	     "  set-state               mark a device as failed\n"
	     "  resize                  resize filesystem on a device\n"
	     "  resize-journal          resize journal on a device\n"
	     "  scan                    discover bcachefs devices\n"
	     "\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
	return 0;
}

int cmd_device(int argc, char *argv[])
{
	char *cmd = pop_cmd(&argc, argv);

	if (!cmd || !strcmp(cmd, "--help") || !strcmp(cmd, "-h") ||
	    !strcmp(cmd, "help"))
		return device_usage();
	if (!strcmp(cmd, "add"))
		return cmd_device_add(argc, argv);
	if (!strcmp(cmd, "remove"))
		return cmd_device_remove(argc, argv);
	if (!strcmp(cmd, "online"))
		return cmd_device_online(argc, argv);
	if (!strcmp(cmd, "offline"))
		return cmd_device_offline(argc, argv);
	if (!strcmp(cmd, "evacuate"))
		return cmd_device_evacuate(argc, argv);
	if (!strcmp(cmd, "set-state"))
		return cmd_device_set_state(argc, argv);
	if (!strcmp(cmd, "resize"))
		return cmd_device_resize(argc, argv);
	if (!strcmp(cmd, "resize-journal"))
		return cmd_device_resize_journal(argc, argv);
	if (!strcmp(cmd, "scan"))
		return cmd_device_scan(argc, argv);

	device_usage();
	return 1;
}
