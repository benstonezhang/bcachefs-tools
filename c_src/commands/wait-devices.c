/*
 * wait-devices: Discover and wait for all bcachefs member devices to appear.
 *
 * Utilizes udev to monitor for emerging block devices and cross-references
 * their UUIDs with the expected filesystem member list. The process blocks
 * until every member device is located or the specified timeout expires.
 *
 * Ported from src/commands/wait_devices.rs.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libudev.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "init/fs.h"
#include "sb/io.h"
#include "cmds.h"

struct wait_sb_entry {
	char *devnode;
	struct bch_sb_handle sb;
};

struct wait_initialized {
	uuid_t uuid;
	DARRAY(struct wait_sb_entry) sbs;
};

static bool should_skip_multipath_component(struct udev_device *dev)
{
	const char *v =
		udev_device_get_property_value(dev, "DM_MULTIPATH_DEVICE_PATH");
	if (v && !strcmp(v, "1"))
		return true;

	const char *devnode = udev_device_get_devnode(dev);
	if (devnode) {
		char *mpath = find_multipath_holder(devnode);
		if (mpath) {
			free(mpath);
			return true;
		}
	}
	return false;
}

static void wait_initialized_remove(struct wait_initialized *w,
				    const char *devnode)
{
	for (size_t i = 0; i < w->sbs.nr; i++) {
		if (!strcmp(w->sbs.data[i].devnode, devnode)) {
			free(w->sbs.data[i].devnode);
			bch2_free_super(&w->sbs.data[i].sb);
			darray_remove_item(&w->sbs, &w->sbs.data[i]);
			return;
		}
	}
}

static void wait_initialized_add(struct wait_initialized *w,
				 struct udev_device *dev)
{
	const char *fs_type = udev_device_get_property_value(dev, "ID_FS_TYPE");
	if (!fs_type || strcmp(fs_type, "bcachefs"))
		return;

	const char *fs_uuid_s =
		udev_device_get_property_value(dev, "ID_FS_UUID");
	if (!fs_uuid_s)
		return;

	uuid_t dev_uuid;
	if (uuid_parse(fs_uuid_s, dev_uuid))
		return;

	if (uuid_compare(dev_uuid, w->uuid))
		return;

	const char *devnode = udev_device_get_devnode(dev);
	if (!devnode)
		return;

	if (should_skip_multipath_component(dev))
		return;

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nochanges, true);
	opt_set(opts, noexcl, true);

	struct bch_sb_handle sb;
	if (bch2_read_super(devnode, &opts, &sb))
		return;

	if (uuid_compare(sb.sb->user_uuid.b, w->uuid)) {
		bch2_free_super(&sb);
		return;
	}

	if (sb.sb->dev_idx >= sb.sb->nr_devices) {
		fprintf(stderr,
			"wait-devices: superblock with invalid dev_idx: %u >= %u\n",
			sb.sb->dev_idx, sb.sb->nr_devices);
		bch2_free_super(&sb);
		return;
	}

	wait_initialized_remove(w, devnode);

	struct wait_sb_entry e = { .devnode = strdup(devnode), .sb = sb };
	darray_push(&w->sbs, e);
}

static bool every_device_is_initialized(struct wait_initialized *w)
{
	if (!w->sbs.nr)
		return false;

	bch_sb_handles handles = {};
	darray_for_each(w->sbs, e)
		darray_push(&handles, e->sb);

	struct bch_opts opts = bch2_opts_empty();
	int ret = bch2_sbs_filter_dead(&handles, &opts, NULL);
	darray_exit(&handles);
	if (ret)
		return false;

	if (!w->sbs.nr)
		return false;

	u32 expected_nr = bch2_sb_nr_devices(w->sbs.data[0].sb.sb);

	unsigned unique_indices = 0;
	for (size_t i = 0; i < w->sbs.nr; i++) {
		bool found = false;
		for (size_t j = 0; j < i; j++) {
			if (w->sbs.data[i].sb.sb->dev_idx ==
			    w->sbs.data[j].sb.sb->dev_idx) {
				found = true;
				break;
			}
		}
		if (!found)
			unique_indices++;
	}

	return unique_indices == expected_nr;
}

static void wait_devices_usage(void)
{
	puts("bcachefs wait-devices - wait until all devices of a filesystem are present\n"
	     "Usage: bcachefs wait-devices UUID=<uuid>\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help              Display this help and exit\n");
}

int cmd_wait_devices(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' }, { NULL }
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			wait_devices_usage();
			return 0;
		}
	args_shift(optind);

	char *uuid_arg = arg_pop();
	if (!uuid_arg) {
		wait_devices_usage();
		return -EINVAL;
	}

	char *uuid_s = uuid_arg;
	if (!strncmp(uuid_s, "UUID=", 5))
		uuid_s += 5;

	struct wait_initialized w = { 0 };
	if (uuid_parse(uuid_s, w.uuid))
		die("invalid device string: %s", uuid_arg);

	struct udev *udev = udev_new();
	if (!udev)
		die("udev_new() failed");

	struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "block", NULL);
	udev_monitor_enable_receiving(mon);

	struct udev_enumerate *en = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(en, "block");
	udev_enumerate_add_match_property(en, "ID_FS_TYPE", "bcachefs");
	udev_enumerate_scan_devices(en);

	struct udev_list_entry *devices, *dev_list_entry;
	devices = udev_enumerate_get_list_entry(en);
	udev_list_entry_foreach(dev_list_entry, devices)
	{
		const char *path = udev_list_entry_get_name(dev_list_entry);
		struct udev_device *dev =
			udev_device_new_from_syspath(udev, path);
		wait_initialized_add(&w, dev);
		udev_device_unref(dev);
	}
	udev_enumerate_unref(en);

	int fd = udev_monitor_get_fd(mon);

	while (!every_device_is_initialized(&w)) {
		struct pollfd fds = { .fd = fd, .events = POLLIN };
		if (poll(&fds, 1, -1) < 0)
			die("error on udev socket fd: %m");

		struct udev_device *dev = udev_monitor_receive_device(mon);
		if (dev) {
			const char *action = udev_device_get_action(dev);
			if (!strcmp(action, "remove"))
				wait_initialized_remove(
					&w, udev_device_get_devnode(dev));
			else if (!strcmp(action, "add") ||
				 !strcmp(action, "change"))
				wait_initialized_add(&w, dev);
			udev_device_unref(dev);
		}
	}

	darray_for_each(w.sbs, e) {
		free(e->devnode);
		bch2_free_super(&e->sb);
	}
	darray_exit(&w.sbs);
	udev_monitor_unref(mon);
	udev_unref(udev);
	return 0;
}
