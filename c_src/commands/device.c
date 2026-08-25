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
#include <libudev.h>
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

	struct bch_fs *c = bch2_fs_open(&devs, &bch_opts);
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
	opt_set(opts, degraded, BCH_DEGRADED_very);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
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
		struct bch_dev *ca = bch2_dev_have_ref(c, dev_idx);
		ca->mi.state = new_state;
		bch2_write_super(c);
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
		struct bch_fs *c = bch2_fs_open(&devs, &opts);
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
		struct bch_fs *c = bch2_fs_open(&devs, &opts);
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

/* scan */

static darray_str get_all_block_devnodes_udev(void)
{
	darray_str devs = { 0 };
	struct udev *udev = udev_new();
	if (!udev)
		return devs;

	struct udev_enumerate *enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "block");
	udev_enumerate_scan_devices(enumerate);

	struct udev_list_entry *devices =
		udev_enumerate_get_list_entry(enumerate);
	struct udev_list_entry *entry;

	udev_list_entry_foreach(entry, devices)
	{
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev =
			udev_device_new_from_syspath(udev, path);
		if (dev) {
			const char *devnode = udev_device_get_devnode(dev);
			if (devnode)
				darray_push(&devs, strdup(devnode));
			udev_device_unref(dev);
		}
	}

	udev_enumerate_unref(enumerate);
	udev_unref(udev);
	return devs;
}

static darray_str get_all_block_devnodes_procfs(void)
{
	darray_str devs = { 0 };
	FILE *f = fopen("/proc/partitions", "r");
	if (!f)
		return devs;

	char line[1024];
	// skip 2 header lines
	if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
		fclose(f);
		return devs;
	}

	while (fgets(line, sizeof(line), f)) {
		char name[256];
		if (sscanf(line, "%*d %*d %*d %255s", name) == 1) {
			char *path = mprintf("/dev/%s", name);
			if (!access(path, F_OK))
				darray_push(&devs, path);
			else
				free(path);
		}
	}

	fclose(f);
	return devs;
}

static darray_str get_all_block_devnodes(void)
{
	darray_str devs = get_all_block_devnodes_udev();
	if (devs.nr == 0)
		devs = get_all_block_devnodes_procfs();
	return devs;
}

struct sb_with_path {
	char *path;
	struct bch_sb *sb;
};
typedef DARRAY(struct sb_with_path) sbs_with_path;

static bool should_skip_multipath_component_udev(struct udev_device *dev)
{
	const char *prop =
		udev_device_get_property_value(dev, "DM_MULTIPATH_DEVICE_PATH");
	const char *devnode = udev_device_get_devnode(dev);

	if (prop && !strcmp(prop, "1"))
		return true;

	if (devnode) {
		char *mpath = find_multipath_holder(devnode);
		if (mpath) {
			free(mpath);
			return true;
		}
	}
	return false;
}

/* Prefer /dev/mapper path when the device is a multipath map. */
static char *dev_path_preferred(const char *path)
{
	char *preferred = preferred_multipath_devnode(path);

	return preferred ? preferred : strdup(path);
}

static bool sb_label_matches(struct bch_sb *sb, const char *label)
{
	size_t label_len = strnlen((const char *)sb->label, sizeof(sb->label));

	return label_len == strlen(label) &&
	       !memcmp(sb->label, label, label_len);
}

static sbs_with_path read_sbs_matching_uuid(uuid_t uuid, darray_str devices,
					    bool filter_multipath)
{
	sbs_with_path res = { 0 };

	darray_for_each(devices, i) {
		if (filter_multipath) {
			char *mpath = find_multipath_holder(*i);
			if (mpath) {
				free(mpath);
				continue;
			}
		}

		int fd = open(*i, O_RDONLY);
		if (fd < 0)
			continue;

		struct bch_sb *sb = __bch2_super_read(fd, BCH_SB_SECTOR);
		close(fd);
		if (!sb)
			continue;

		if (!uuid_compare(sb->user_uuid.b, uuid)) {
			darray_push(&res, ((struct sb_with_path){
						  .path = dev_path_preferred(*i),
						  .sb = sb }));
		} else {
			free(sb);
		}
	}
	return res;
}

static sbs_with_path read_sbs_matching_label(const char *label,
					     darray_str devices,
					     bool filter_multipath)
{
	sbs_with_path res = { 0 };

	darray_for_each(devices, i) {
		if (filter_multipath) {
			char *mpath = find_multipath_holder(*i);
			if (mpath) {
				free(mpath);
				continue;
			}
		}

		int fd = open(*i, O_RDONLY);
		if (fd < 0)
			continue;

		struct bch_sb *sb = __bch2_super_read(fd, BCH_SB_SECTOR);
		close(fd);
		if (!sb)
			continue;

		if (sb_label_matches(sb, label)) {
			darray_push(&res, ((struct sb_with_path){
						  .path = strdup(*i),
						  .sb = sb }));
		} else {
			free(sb);
		}
	}
	return res;
}

static darray_str get_devices_by_uuid_udev(uuid_t uuid)
{
	darray_str devs = { 0 };
	struct udev *udev = udev_new();
	if (!udev)
		return devs;

	struct udev_enumerate *enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "block");
	udev_enumerate_add_match_property(enumerate, "ID_FS_TYPE", "bcachefs");
	udev_enumerate_scan_devices(enumerate);

	struct udev_list_entry *devices =
		udev_enumerate_get_list_entry(enumerate);
	struct udev_list_entry *entry;

	char uuid_str[40];
	uuid_unparse(uuid, uuid_str);

	udev_list_entry_foreach(entry, devices)
	{
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev =
			udev_device_new_from_syspath(udev, path);
		if (!dev)
			continue;

		const char *dev_uuid =
			udev_device_get_property_value(dev, "ID_FS_UUID");
		const char *devnode = udev_device_get_devnode(dev);

		if (dev_uuid && !strcmp(dev_uuid, uuid_str) && devnode &&
		    !should_skip_multipath_component_udev(dev))
			darray_push(&devs, strdup(devnode));

		udev_device_unref(dev);
	}

	udev_enumerate_unref(enumerate);
	udev_unref(udev);
	return devs;
}

static darray_str get_bcachefs_devnodes_udev(void)
{
	darray_str devs = { 0 };
	struct udev *udev = udev_new();
	if (!udev)
		return devs;

	struct udev_enumerate *enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "block");
	udev_enumerate_add_match_property(enumerate, "ID_FS_TYPE", "bcachefs");
	udev_enumerate_scan_devices(enumerate);

	struct udev_list_entry *devices =
		udev_enumerate_get_list_entry(enumerate);
	struct udev_list_entry *entry;

	udev_list_entry_foreach(entry, devices)
	{
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev =
			udev_device_new_from_syspath(udev, path);
		if (!dev)
			continue;

		const char *devnode = udev_device_get_devnode(dev);
		if (devnode && !should_skip_multipath_component_udev(dev))
			darray_push(&devs, strdup(devnode));

		udev_device_unref(dev);
	}

	udev_enumerate_unref(enumerate);
	udev_unref(udev);
	return devs;
}

static char *join_device_paths(darray_str *devs)
{
	struct printbuf out = PRINTBUF;

	darray_for_each(*devs, i) {
		if (i != devs->data)
			prt_str(&out, ":");
		prt_str(&out, *i);
		free(*i);
	}
	darray_exit(devs);

	char *res = strdup(out.buf);
	printbuf_exit(&out);
	return res;
}

static char *get_devices_by_uuid(uuid_t uuid, bool use_udev)
{
	darray_str matching_devs = { 0 };

	if (use_udev) {
		darray_str udev_devs = get_devices_by_uuid_udev(uuid);

		if (udev_devs.nr > 0) {
			sbs_with_path sbs =
				read_sbs_matching_uuid(uuid, udev_devs, false);
			unsigned expected =
				sbs.nr ? sbs.data[0].sb->nr_devices : 0;

			darray_for_each(udev_devs, i) free(*i);
			darray_exit(&udev_devs);

			if (sbs.nr >= expected) {
				darray_for_each(sbs, i) {
					darray_push(&matching_devs, i->path);
					free(i->sb);
				}
				darray_exit(&sbs);
				return join_device_paths(&matching_devs);
			}

			darray_for_each(sbs, i) {
				free(i->path);
				free(i->sb);
			}
			darray_exit(&sbs);
		} else {
			darray_exit(&udev_devs);
		}
	}

	darray_str all_devs = get_all_block_devnodes();
	sbs_with_path matching_sbs =
		read_sbs_matching_uuid(uuid, all_devs, true);

	darray_for_each(matching_sbs, i) {
		darray_push(&matching_devs, i->path);
		free(i->sb);
	}
	darray_exit(&matching_sbs);
	darray_for_each(all_devs, i) free(*i);
	darray_exit(&all_devs);

	return join_device_paths(&matching_devs);
}

static char *get_devices_by_label(const char *label, bool use_udev)
{
	sbs_with_path sbs = { 0 };

	if (use_udev) {
		darray_str udev_devs = get_bcachefs_devnodes_udev();
		if (udev_devs.nr > 0)
			sbs = read_sbs_matching_label(label, udev_devs, false);
		darray_for_each(udev_devs, i) free(*i);
		darray_exit(&udev_devs);
	}

	if (!sbs.nr) {
		darray_str all_devs = get_all_block_devnodes();
		sbs = read_sbs_matching_label(label, all_devs, true);
		darray_for_each(all_devs, i) free(*i);
		darray_exit(&all_devs);
	}

	if (!sbs.nr) {
		darray_exit(&sbs);
		return NULL;
	}

	/* Deduplicate UUIDs; error if multiple filesystems share the label. */
	uuid_t first_uuid;
	bool have_uuid = false;

	darray_for_each(sbs, i) {
		if (!have_uuid) {
			uuid_copy(first_uuid, i->sb->user_uuid.b);
			have_uuid = true;
		} else if (uuid_compare(first_uuid, i->sb->user_uuid.b)) {
			darray_for_each(sbs, j) {
				free(j->path);
				free(j->sb);
			}
			darray_exit(&sbs);
			die("multiple bcachefs filesystems found with label '%s'",
			    label);
		}
	}

	darray_for_each(sbs, i) {
		free(i->path);
		free(i->sb);
	}
	darray_exit(&sbs);

	return get_devices_by_uuid(first_uuid, use_udev);
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

	int fd = open(device_or_uuid, O_RDONLY);
	if (fd < 0)
		return strdup(device_or_uuid);

	struct bch_sb *sb = __bch2_super_read(fd, BCH_SB_SECTOR);
	if (!sb) {
		close(fd);
		return strdup(device_or_uuid);
	}
	uuid_copy(uuid, sb->user_uuid.b);

	if (sb->nr_devices == 1) {
		free(sb);
		close(fd);
		return strdup(device_or_uuid);
	}
	free(sb);
	close(fd);

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

	if (!cmd)
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
	return -EINVAL;
}
