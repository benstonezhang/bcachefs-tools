/*
 * Finding a filesystem's member devices, all of which a mount needs.
 *
 * Three ways, in cost order. udev's database is fast but only knows devices
 * it has already tagged, which at boot may be none of them. A block scan
 * reads every superblock on the machine, which is slow but needs nothing
 * running - /proc/partitions when udev is unavailable (#344).
 *
 * Neither helps with a device that has not appeared yet, which is what
 * mounting by UUID at boot looks like, so a short search waits for one to
 * arrive and looks again, bounded by missing_dev_timeout (#308, #393). Only
 * when searching for a filesystem: a caller who names paths has already
 * decided what exists.
 *
 * Ported from src/device_scan.rs.
 */

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "tools-util.h"

#include "sb/io.h"
#include "sb/members.h"
#include "init/fs.h"

#include <libudev.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>

/* warn! level: something worth saying without being asked. */
#define dscan_warn(fmt, ...)	fprintf(stderr, "bcachefs: " fmt "\n", ##__VA_ARGS__)

/*
 * info! level: rust's default filter is Warn, so these only reach someone who
 * asked with -v; the C fork has no log levels, so they're gated on the mount
 * option of the same name.
 */
static void dscan_info(const struct bch_opts *opts, const char *fmt, ...)
{
	va_list args;

	if (!opt_get(*opts, verbose))
		return;
	va_start(args, fmt);
	fprintf(stderr, "bcachefs: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

static void darray_str_exit(darray_str *d)
{
	darray_for_each(*d, i)
		free(*(char **)i);
	darray_exit(d);
}

int bch2_read_super_silent_opts(const char *path, struct bch_opts *opts,
				struct bch_sb_handle *out)
{
	opt_set(*opts, noexcl, 1);
	opt_set(*opts, nochanges, 1);
	opt_set(*opts, no_version_check, 1);

	return bch2_read_super_silent(path, opts, out);
}

void bch2_scanned_sbs_exit(bch_scanned_sbs *sbs)
{
	struct bch_scanned_sb *i;

	darray_for_each(*sbs, i) {
		free(i->path);
		bch2_free_super(&i->sb);
	}
	darray_exit(sbs);
}

/* Set by multipath's udev rule; fall back to sysfs if not present. */
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

/* Match ID_FS_UUID the same way the block scan compares, so both pick the
 * same filesystem. The property is the canonical hex form.
 */
static bool scan_udev_uuid_eq(const char *prop_uuid, uuid_t want)
{
	uuid_t had;

	if (!prop_uuid || uuid_parse(prop_uuid, had))
		return false;
	return !uuid_compare(had, want);
}

static int get_devices_by_uuid_udev(uuid_t uuid, darray_str *devs)
{
	struct udev *udev = udev_new();
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *entry;
	int ret = 0;

	if (!udev)
		return -ENODEV;

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		ret = -ENOMEM;
		goto out;
	}
	udev_enumerate_add_match_is_initialized(enumerate);
	udev_enumerate_add_match_subsystem(enumerate, "block");
	udev_enumerate_add_match_property(enumerate, "ID_FS_TYPE", "bcachefs");
	if (udev_enumerate_scan_devices(enumerate)) {
		ret = -EIO;
		goto out_enumerate;
	}

	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(entry, devices) {
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
		const char *dev_uuid;
		const char *devnode;

		if (!dev)
			continue;
		dev_uuid = udev_device_get_property_value(dev, "ID_FS_UUID");
		devnode = udev_device_get_devnode(dev);

		if (devnode && scan_udev_uuid_eq(dev_uuid, uuid) &&
		    !should_skip_multipath_component_udev(dev))
			darray_push(devs, strdup(devnode));

		udev_device_unref(dev);
	}

out_enumerate:
	udev_enumerate_unref(enumerate);
out:
	udev_unref(udev);
	return ret;
}

static int get_bcachefs_devnodes_udev(darray_str *devs)
{
	struct udev *udev = udev_new();
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *entry;
	int ret = 0;

	if (!udev)
		return -ENODEV;

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		ret = -ENOMEM;
		goto out;
	}
	udev_enumerate_add_match_is_initialized(enumerate);
	udev_enumerate_add_match_subsystem(enumerate, "block");
	udev_enumerate_add_match_property(enumerate, "ID_FS_TYPE", "bcachefs");
	if (udev_enumerate_scan_devices(enumerate)) {
		ret = -EIO;
		goto out_enumerate;
	}

	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(entry, devices) {
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
		const char *devnode;

		if (!dev)
			continue;
		devnode = udev_device_get_devnode(dev);
		if (devnode && !should_skip_multipath_component_udev(dev))
			darray_push(devs, strdup(devnode));
		udev_device_unref(dev);
	}

out_enumerate:
	udev_enumerate_unref(enumerate);
out:
	udev_unref(udev);
	return ret;
}

static int get_all_block_devnodes_udev(darray_str *devs)
{
	struct udev *udev = udev_new();
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *entry;
	int ret = 0;

	if (!udev)
		return -ENODEV;

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		ret = -ENOMEM;
		goto out;
	}
	udev_enumerate_add_match_is_initialized(enumerate);
	udev_enumerate_add_match_subsystem(enumerate, "block");
	if (udev_enumerate_scan_devices(enumerate)) {
		ret = -EIO;
		goto out_enumerate;
	}

	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(entry, devices) {
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
		const char *devnode;

		if (!dev)
			continue;
		devnode = udev_device_get_devnode(dev);
		if (devnode)
			darray_push(devs, strdup(devnode));
		udev_device_unref(dev);
	}

out_enumerate:
	udev_enumerate_unref(enumerate);
out:
	udev_unref(udev);
	return ret;
}

/* Scan /proc/partitions for block devices. Works without udev. */
static int get_all_block_devnodes_procfs(darray_str *devs)
{
	FILE *f = fopen("/proc/partitions", "r");
	char line[256];

	if (!f)
		return -errno;

	while (fgets(line, sizeof(line), f)) {
		char name[256];
		char path[512];
		int major, minor;
		unsigned long blocks;

		if (sscanf(line, "%d %d %lu %255s", &major, &minor, &blocks, name) != 4)
			continue;
		if (snprintf(path, sizeof(path), "/dev/%s", name) >= (int)sizeof(path))
			continue;
		if (access(path, R_OK) == 0)
			darray_push(devs, strdup(path));
	}
	fclose(f);
	return 0;
}

/*
 * udev first - it knows which partitions actually have bcachefs superblocks
 * (procfs names every partition even for filesystems we couldn't recognize).
 * But udev may be entirely absent at boot; fall back to procfs when that
 * happens or when it found nothing.
 */
static int get_all_block_devnodes(darray_str *devs)
{
	darray_str udev_devs = { 0 };
	int ret = get_all_block_devnodes_udev(&udev_devs);

	if (!ret && udev_devs.nr) {
		*devs = udev_devs;
		return 0;
	}
	darray_exit(&udev_devs);
	return get_all_block_devnodes_procfs(devs);
}

typedef int (*scan_fn)(void *, bch_scanned_sbs *);

static bool have_every_device(const bch_scanned_sbs *sbs);

struct scan_closures {
	struct bch_opts	opts;
	bool		use_udev;
	union {
		struct {
			uuid_t uuid;
		} u;
		struct {
			const char *label;
		} l;
	};
};

static int filter_current_sbs(bch_scanned_sbs *sbs, struct bch_opts *opts);
static char *sb_handle_path(struct bch_sb_handle *sb);

/*
 * Drop superblocks for devices that have been properly removed, and for a
 * device that diverged the same way - but only after checking for a split
 * brain, which is also a difference in device sets.
 */
static int filter_current_sbs(bch_scanned_sbs *sbs, struct bch_opts *opts)
{
	bch_sb_handles handles = { 0 };
	unsigned k;
	int ret;

	if (!opt_get(*opts, no_splitbrain_check)) {
		bch_divergents divergent = { 0 };

		bch2_splitbrain_find(sbs, opts, &divergent);
		if (divergent.nr) {
			char *msg = bch2_splitbrain_report(sbs, &divergent);

			/*
			 * One warning, not one per line: the report is a single
			 * account with blank lines in it for readability, and
			 * a warn! per line stamps file:line on every one of
			 * them - including the blanks.
			 */
			dscan_warn("%s", msg);
			bch2_splitbrain_divergents_exit(&divergent);
			free(msg);
			bch2_scanned_sbs_exit(sbs);
			return -BCH_ERR_device_splitbrain;
		}
		bch2_splitbrain_divergents_exit(&divergent);
	}

	/*
	 * Move the superblocks out of the pairs, so we can hand bch2_sbs_filter_dead()
	 * the same thing the rust side gives it - a list of just handles.
	 */
	handles.nr = sbs->nr;
	darray_resize(&handles, sbs->nr);
	for (k = 0; k < sbs->nr; k++) {
		handles.data[k] = sbs->data[k].sb;
		free(sbs->data[k].path);
		sbs->data[k].path = NULL;
		sbs->data[k].sb = (struct bch_sb_handle){ 0 };
	}
	bch2_scanned_sbs_exit(sbs);

	/*
	 * Filters out dead / dropped devices, and swaps the device with the
	 * most recent superblock to the front.
	 */
	ret = bch2_sbs_filter_dead(&handles, opts, NULL);
	if (ret) {
		for (k = 0; k < handles.nr; k++)
			bch2_free_super(&handles.data[k]);
		free(handles.data);
		return ret;
	}

	darray_resize(sbs, handles.nr);
	sbs->nr = handles.nr;
	for (k = 0; k < handles.nr; k++) {
		char *p = sb_handle_path(&handles.data[k]);
		char *preferred = preferred_multipath_devnode(p);

		sbs->data[k].sb = handles.data[k];
		sbs->data[k].path = preferred ?: strdup(p);
		free(p);
	}
	free(handles.data);

	return 0;
}

static int read_sbs_matching_uuid(uuid_t uuid, darray_str devices,
				  bool filter_multipath,
				  struct bch_opts *opts,
				  bch_scanned_sbs *out)
{
	bch_scanned_sbs sbs = { 0 };
	int ret;

	darray_for_each(devices, i) {
		struct bch_sb_handle h = { 0 };
		char *path;
		int err;

		/*
		 * When not using udev (which already filters), skip multipath
		 * components.
		 */
		if (filter_multipath) {
			char *mpath = find_multipath_holder(*i);

			if (mpath) {
				free(mpath);
				continue;
			}
		}

		err = bch2_read_super_silent_opts(*i, opts, &h);
		if (err)
			continue;

		path = preferred_multipath_devnode(*i);
		if (!path)
			path = strdup(*i);

		if (!uuid_compare(h.sb->user_uuid.b, uuid)) {
			darray_push(&sbs, ((struct bch_scanned_sb){
						  .path = path, .sb = h }));
		} else {
			free(path);
			bch2_free_super(&h);
		}
	}

	ret = filter_current_sbs(&sbs, opts);
	*out = sbs;
	return ret;
}

static bool sb_label_matches(struct bch_sb *sb, const char *label)
{
	size_t label_len = strnlen((const char *)sb->label, sizeof(sb->label));

	return label_len == strlen(label) && !memcmp(sb->label, label, label_len);
}

static int read_sbs_matching_label(const char *label, darray_str devices,
				   bool filter_multipath,
				   struct bch_opts *opts,
				   bch_scanned_sbs *out)
{
	bch_scanned_sbs sbs = { 0 };
	int ret;

	darray_for_each(devices, i) {
		struct bch_sb_handle h = { 0 };
		int err;

		if (filter_multipath) {
			char *mpath = find_multipath_holder(*i);

			if (mpath) {
				free(mpath);
				continue;
			}
		}

		err = bch2_read_super_silent_opts(*i, opts, &h);
		if (err)
			continue;

		if (sb_label_matches(h.sb, label)) {
			darray_push(&sbs, ((struct bch_scanned_sb){
						  .path = strdup(*i), .sb = h }));
		} else {
			bch2_free_super(&h);
		}
	}

	ret = filter_current_sbs(&sbs, opts);
	*out = sbs;
	return ret;
}

static char *sb_handle_path(struct bch_sb_handle *sb)
{
	if (!sb->sb_name)
		return strdup("");
	return strdup(sb->sb_name);
}

/* Are all the members here? */
static bool have_every_device(const bch_scanned_sbs *sbs)
{
	unsigned expected = bch2_scanned_expected_devices(sbs);

	return expected != 0 && bch2_scanned_present_devices(sbs) >= expected;
}

int bch2_get_devices_by_uuid(uuid_t uuid, struct bch_opts *opts,
			     bool use_udev, bch_scanned_sbs *out)
{
	if (use_udev) {
		darray_str devs_from_udev = { 0 };
		int err = get_devices_by_uuid_udev(uuid, &devs_from_udev);

		if (err)
			return err;

		if (devs_from_udev.nr) {
			bch_scanned_sbs sbs = { 0 };
			int ret = read_sbs_matching_uuid(uuid, devs_from_udev,
							 false, opts, &sbs);

			darray_str_exit(&devs_from_udev);
			if (ret)
				return ret;

			/*
			 * Check if udev found all expected devices. During
			 * early boot, udev may not have finished processing
			 * all devices yet - if we got fewer than expected,
			 * fall back to scanning all block devices.
			 */
			if (have_every_device(&sbs)) {
				*out = sbs;
				return 0;
			}

			bch2_scanned_sbs_exit(&sbs);
		} else {
			darray_str_exit(&devs_from_udev);
		}
	}

	/*
	 * Falls back to /proc/partitions if udev is unavailable, so this works
	 * without udevd running.
	 */
	{
		darray_str all_devs = { 0 };
		int ret = get_all_block_devnodes(&all_devs);

		if (ret)
			return ret;
		ret = read_sbs_matching_uuid(uuid, all_devs, true, opts, out);
		darray_str_exit(&all_devs);
		return ret;
	}
}

/*
 * Members the filesystem should have, according to what we found. Zero when
 * we found nothing at all - not "no devices", but "don't know yet".
 */
unsigned bch2_scanned_expected_devices(const bch_scanned_sbs *sbs)
{
	return sbs->nr ? bch2_sb_nr_devices(sbs->data[0].sb.sb) : 0;
}

/*
 * By dev_idx, because a scan produces paths and the same device turns up
 * under more than one - multipath, or udev and the block scan both
 * contributing. Counting paths calls the set complete with a member missing.
 */
unsigned bch2_scanned_present_devices(const bch_scanned_sbs *sbs)
{
	u8 seen[BCH_SB_MEMBERS_MAX] = { 0 };
	struct bch_scanned_sb *i;
	unsigned n = 0;

	darray_for_each(*sbs, i) {
		if (seen[i->sb.sb->dev_idx])
			continue;
		seen[i->sb.sb->dev_idx] = 1;
		n++;
	}
	return n;
}

/*
 * The member devices as `bcachefs fs usage` wants to see them, built from the
 * superblocks we scanned instead of from sysfs.
 *
 * Same dev_name, so fs usage's durability and degraded accounting can be
 * reused on a filesystem that isn't mounted yet: there is no
 * /sys/fs/bcachefs/<uuid> to read until it is, and by then nobody needs the
 * answer.
 *
 * @online means "the scan found it", which is the question being asked here -
 * a member the superblock names and we didn't turn up is missing, whichever
 * way it went missing.
 *
 * Durability is one-biased on disk, zero meaning one, as bch2_mi_to_cpu()
 * reads it. Taking the raw field would report every default device as
 * contributing no durability at all.
 */
int bch2_devices_from_superblocks(const bch_scanned_sbs *sbs, dev_names *out)
{
	struct bch_sb *first;
	struct bch_sb_field_members_v2 *mi;
	struct bch_scanned_sb *i;
	unsigned idx, nr;

	if (!sbs->nr)
		return 0;

	first = sbs->data[0].sb.sb;
	mi = bch2_sb_field_get(first, members_v2);
	if (!mi)
		return 0;

	nr = first->nr_devices;
	for (idx = 0; idx < nr; idx++) {
		struct bch_member m = bch2_members_v2_get(mi, idx);
		unsigned long raw = BCH_MEMBER_DURABILITY(&m);
		const char *dev = NULL;
		struct dev_name d = { .idx = idx };

		if (!bch2_member_alive(&m))
			continue;

		darray_for_each(*sbs, i)
			if (i->sb.sb->dev_idx == idx) {
				dev = i->path;
				break;
			}

		if (dev)
			d.dev = strdup(dev);
		else {
			d.dev = xmalloc(16);
			snprintf(d.dev, 16, "dev-%u", idx);
		}
		d.durability	= raw != 0 ? raw - 1 : 1;
		d.online	= dev != NULL;
		darray_push(out, d);
	}
	return 0;
}

/* How long to wait for member devices before any of them have been found.
 *
 * The filesystem's own missing_dev_timeout is the number we want, but it's on
 * a disk we can't read yet, so the first stretch of the wait has to run on a
 * built-in. Once any member turns up, that filesystem's value takes over.
 */
#define DEFAULT_MISSING_DEV_TIMEOUT	30

/* Only for the case where there is nothing to be notified by: udev isn't
 * trusted (-o mount_trusts_udev=0) or isn't running, so a rescan is the only
 * way to learn anything. Slow on purpose - a rescan reads the superblock of
 * every block device on the machine.
 */
#define NO_UDEV_RESCAN_INTERVAL		1

/*
 * How long to wait for a member before mentioning that we are waiting.
 *
 * Long enough that somebody is starting to wonder. Most of the time every
 * device is already there and the wait is over before this, which is what
 * keeps a normal mount silent.
 */
#define QUIET_WAIT			2

/*
 * How long to keep waiting.
 *
 * -o missing_dev_timeout wins: the option is OPT_MOUNT, so someone who passes
 * it means this mount, not this filesystem. Otherwise the filesystem's own
 * value, if we've found enough of one to read it. Zero on disk means "unset",
 * since every filesystem written before the option existed reads back zero, so
 * it falls through to the same built-in as having found nothing at all.
 */
static u64 missing_dev_timeout(const bch_scanned_sbs *sbs,
			       const struct bch_opts *cli_opts)
{
	struct bch_opts sb_opts = bch2_opts_empty();
	u32 secs;

	if (opt_defined(*cli_opts, missing_dev_timeout))
		return opt_get(*cli_opts, missing_dev_timeout);

	if (!sbs->nr)
		return DEFAULT_MISSING_DEV_TIMEOUT;

	if (bch2_opts_from_sb(&sb_opts, sbs->data[0].sb.sb) != 0)
		return DEFAULT_MISSING_DEV_TIMEOUT;

	secs = opt_get(sb_opts, missing_dev_timeout);
	return secs ? secs : DEFAULT_MISSING_DEV_TIMEOUT;
}

/*
 * udevd's own test for whether it is running - libudev checks this same
 * socket in udev_queue_get_udev_is_active().
 */
static bool udevd_running(void)
{
	struct stat st;

	return stat("/run/udev/control", &st) == 0;
}

/*
 * The two netlink groups are not interchangeable: "udev" subscribes to the
 * one udevd writes after processing an event, "kernel" to the one the kernel
 * writes (kobject_uevent). In an initramfs - the case this whole wait exists
 * for - a "udev" monitor is a socket nobody writes to.
 *
 * Prefer udevd's when it is running: its events arrive after device-mapper
 * and md names are set up, where a kernel event for those can land before
 * there is anything to find. Callers rescan rather than trust the devnode in
 * the event, so either source carries all they need.
 */
static struct udev_monitor *block_device_monitor(void)
{
	struct udev_monitor *m;

	m = udev_monitor_new_from_netlink(NULL, udevd_running() ? "udev" : "kernel");
	if (!m)
		return NULL;
	if (udev_monitor_filter_add_match_subsystem_devtype(m, "block", NULL) ||
	    udev_monitor_enable_receiving(m)) {
		udev_monitor_unref(m);
		return NULL;
	}
	return m;
}

/*
 * Event-driven rather than polled: a rescan reads the superblock of every
 * block device on the machine, so polling one would keep every spun-down disk
 * awake for the length of a boot to learn nothing.
 *
 * The monitor is built before the first scan on purpose. A device arriving in
 * the gap would otherwise be in neither - too late for the scan, too early
 * for a socket that did not exist yet - and we would wait out the whole
 * timeout with it sitting there.
 *
 * Returns a short set rather than failing: what to do about missing members
 * is the degraded action's question, not this one's.
 */
static int scan_waiting_for_devices(const struct bch_opts *cli_opts,
				    scan_fn scan, void *arg,
				    bch_scanned_sbs *out)
{
	struct udev_monitor *monitor = block_device_monitor();
	struct timespec start;
	bool announced = false;
	u64 announced_timeout = 0;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &start);
	ret = scan(arg, out);
	if (ret)
		goto out;

	for (;;) {
		struct timespec now;
		double elapsed;
		u64 timeout, remaining;
		unsigned expected;

		if (have_every_device(out))
			break;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (double)(now.tv_sec - start.tv_sec) +
			  (now.tv_nsec - start.tv_nsec) / 1e9;

		timeout = missing_dev_timeout(out, cli_opts);
		if (elapsed >= (double)timeout)
			break;
		remaining = timeout - (u64)elapsed;

		/*
		 * Only once the pause is long enough to need explaining. This
		 * exists so a boot that stalls on a slow disk does not look
		 * wedged; a mount that pauses for a moment is not something
		 * anyone needs told about, and saying so anyway is how a log
		 * teaches people to skip its warnings.
		 *
		 * Not per event either - the console is not the place for a
		 * progress bar. Keyed on the timeout rather than a bare flag
		 * because the timeout changes: until the first member turns up
		 * we are working off the built-in, and the number we said out
		 * loud would otherwise be one nobody is waiting for.
		 */
		if (elapsed >= QUIET_WAIT && (!announced ||
					     announced_timeout != timeout)) {
			announced = true;
			announced_timeout = timeout;
			expected = bch2_scanned_expected_devices(out);
			if (!expected)
				dscan_warn("no devices found yet, waiting up to %llus",
					   (unsigned long long)timeout);
			else
				dscan_warn("found %u of %u devices, waiting up to %llus for the rest",
					   bch2_scanned_present_devices(out),
					   expected,
					   (unsigned long long)timeout);
		}

		if (monitor) {
			struct pollfd p = { .fd = udev_monitor_get_fd(monitor),
					    .events = POLLIN };
			unsigned drained = 0;
			double wait;

			/*
			 * Bounded until we have spoken, or a wait long enough
			 * to need explaining would sit here silently for all
			 * of it: nothing wakes this poll when the device
			 * simply isn't coming.
			 */
			if (announced)
				wait = (double)remaining;
			else
				wait = min((double)remaining, (double)QUIET_WAIT);

			if (wait >= 1) {
				struct udev_device *d;
				time_t secs = (time_t)wait;
				struct timespec ts = {
					.tv_sec	 = secs,
					.tv_nsec = (long)((wait - secs) * 1e9),
				};
				int pr = ppoll(&p, 1, &ts, NULL);

				if (pr < 0) {
					if (errno == EINTR)
						continue;
					ret = -errno;
					goto out;
				}

				if (p.revents & (POLLERR | POLLNVAL)) {
					dscan_warn("error on udev socket fd");
					ret = -EIO;
					goto out;
				}

				/*
				 * Nothing drained means the poll timed out,
				 * and a device arriving is only a hint, never a
				 * substitute for looking: rescan and let the
				 * scan decide.
				 */
				if (p.revents & POLLIN)
					while ((d = udev_monitor_receive_device(monitor))) {
						udev_device_unref(d);
						drained++;
					}
			}

			if (drained) {
				ret = scan(arg, out);
				if (ret)
					goto out;
			}
		} else {
			struct timespec ts = {
				.tv_sec = min((double)remaining,
					      (double)NO_UDEV_RESCAN_INTERVAL),
			};

			if (nanosleep(&ts, NULL) < 0 && errno != EINTR) {
				ret = -errno;
				goto out;
			}
			ret = scan(arg, out);
			if (ret)
				goto out;
		}
	}

	/*
	 * info!, not warn!: nothing is wrong by the time we get here, and
	 * warning about the good outcome is how a log teaches people to skip
	 * warnings.
	 *
	 * The default filter is Warn, so this is only visible to someone who
	 * asked with -v - which is the right trade, because the warning above
	 * does not need closing. If the devices never turned up, degraded.rs
	 * says so at length; if they did, the mount simply works.
	 */
	if (announced) {
		struct timespec now;
		double elapsed;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (double)(now.tv_sec - start.tv_sec) +
			  (now.tv_nsec - start.tv_nsec) / 1e9;
		dscan_info(cli_opts, "all %u devices found after %.1fs",
			   bch2_scanned_expected_devices(out), elapsed);
	}

	ret = 0;
out:
	if (monitor)
		udev_monitor_unref(monitor);
	return ret;
}

static int scan_by_uuid(void *arg, bch_scanned_sbs *out)
{
	struct scan_closures *ctx = arg;

	return bch2_get_devices_by_uuid(ctx->u.uuid, &ctx->opts,
					ctx->use_udev, out);
}

static int scan_by_label(void *arg, bch_scanned_sbs *out)
{
	struct scan_closures *ctx = arg;

	return bch2_get_devices_by_label(ctx->l.label, &ctx->opts,
					 ctx->use_udev, out);
}

static int search(const struct bch_opts *opts, bool wait,
		  scan_fn scan, void *arg, bch_scanned_sbs *out)
{
	if (wait)
		return scan_waiting_for_devices(opts, scan, arg, out);
	return scan(arg, out);
}

static int uuids_cmp(const void *a, const void *b)
{
	return memcmp(a, b, sizeof(uuid_t));
}

int bch2_get_devices_by_label(const char *label, struct bch_opts *opts,
			      bool use_udev, bch_scanned_sbs *out)
{
	bch_scanned_sbs sbs = { 0 };
	uuid_t *uuids = NULL;
	unsigned n_uuids = 0;
	struct bch_scanned_sb *i;
	int ret = 0;

	if (use_udev) {
		darray_str devs = { 0 };
		int err = get_bcachefs_devnodes_udev(&devs);

		if (err)
			return err;
		if (devs.nr) {
			ret = read_sbs_matching_label(label, devs, false, opts, &sbs);
			darray_str_exit(&devs);
			if (ret)
				return ret;
		} else {
			darray_str_exit(&devs);
		}
	}

	if (!sbs.nr) {
		darray_str all_devs = { 0 };

		ret = get_all_block_devnodes(&all_devs);
		if (ret)
			return ret;
		ret = read_sbs_matching_label(label, all_devs, true, opts, &sbs);
		darray_str_exit(&all_devs);
		if (ret)
			return ret;
	}

	darray_for_each(sbs, i) {
		unsigned j;

		for (j = 0; j < n_uuids; j++)
			if (!uuid_compare(uuids[j], i->sb.sb->user_uuid.b))
				break;
		if (j == n_uuids) {
			uuids = xrealloc(uuids, (n_uuids + 1) * sizeof(uuid_t));
			uuid_copy(uuids[n_uuids], i->sb.sb->user_uuid.b);
			n_uuids++;
		}
	}

	qsort(uuids, n_uuids, sizeof(uuid_t), uuids_cmp);
	bch2_scanned_sbs_exit(&sbs);

	switch (n_uuids) {
	case 0:
		ret = 0;
		break;
	case 1:
		{
			struct scan_closures c = { .opts = *opts,
						    .use_udev = use_udev };

			uuid_copy(c.u.uuid, uuids[0]);
			ret = search(opts, false, scan_by_uuid, &c, out);
		}
		break;
	default:
		fprintf(stderr, "bcachefs: multiple bcachefs filesystems "
			"found with label '%s'\n", label);
		ret = -EINVAL;
		break;
	}

	free(uuids);
	return ret;
}

static int devs_str_sbs_from_device(const char *device, struct bch_opts *opts,
				    bool use_udev, bool wait,
				    bch_scanned_sbs *out)
{
	struct stat st;
	char *mpath;

	if (stat(device, &st) == 0 && S_ISDIR(st.st_mode)) {
		fprintf(stderr, "bcachefs: '%s' is a directory, not a block device\n",
			device);
		return -EINVAL;
	}

	/*
	 * Honor explicit user-supplied paths, but warn when a path appears to
	 * be a multipath component because that is typically unintended.
	 */
	if ((mpath = find_multipath_holder(device))) {
		warn_multipath_component(device, mpath);
		free(mpath);
	}

	{
		struct bch_sb_handle dev_sb = { 0 };
		int err = bch2_read_super_silent_opts(device, opts, &dev_sb);

		if (err)
			return err;

		if (bch2_sb_nr_devices(dev_sb.sb) == 1) {
			darray_push(out, ((struct bch_scanned_sb){
						 .path = strdup(device), .sb = dev_sb }));
			return 0;
		}

		{
			struct scan_closures c = { .opts = *opts,
						    .use_udev = use_udev };

			uuid_copy(c.u.uuid, dev_sb.sb->user_uuid.b);
			bch2_free_super(&dev_sb);

			/*
			 * This is the path a multi-device root actually takes:
			 * mount(8) resolves an fstab UUID= to a devnode itself
			 * and execs us with a single path, so it never reaches
			 * the UUID= branch. Unlike that branch we have already
			 * read a superblock, so we know how many members to
			 * expect from the first iteration rather than falling
			 * back to the built-in timeout.
			 */
			return search(opts, wait, scan_by_uuid, &c, out);
		}
	}
}

/* Returns 1 on UUID= match, 0 on no match, negative on invalid uuid. */
static int parse_uuid_equals(const char *s, uuid_t uuid)
{
	const char *uuid_str = NULL;

	if (!strncmp(s, "UUID=", 5))
		uuid_str = s + 5;
	else if (!strncmp(s, "OLD_BLKID_UUID=", 15))
		uuid_str = s + 15;
	else
		return 0;

	if (uuid_parse(uuid_str, uuid))
		return -EINVAL;
	return 1;
}

static const char *parse_label_equals(const char *s)
{
	if (!strncmp(s, "LABEL=", 6))
		return s + 6;
	return NULL;
}

static int scan_sbs_maybe_waiting(const char *device, struct bch_opts *opts,
				  bool wait, bch_scanned_sbs *out)
{
	bool use_udev = opt_get(*opts, mount_trusts_udev) != 0;
	uuid_t uuid;
	const char *label;
	int r = parse_uuid_equals(device, uuid);

	if (r > 0) {
		struct scan_closures c = { .opts = *opts, .use_udev = use_udev };

		uuid_copy(c.u.uuid, uuid);
		return search(opts, wait, scan_by_uuid, &c, out);
	}
	if (r < 0)
		return r;

	if ((label = parse_label_equals(device))) {
		struct scan_closures c = { .opts = *opts, .use_udev = use_udev,
					   .l.label = label };

		return search(opts, wait, scan_by_label, &c, out);
	}

	if (strchr(device, ':')) {
		struct bch_opts scan_opts = *opts;
		char *copy = strdup(device), *p = copy, *tok;
		int ret = 0;

		opt_set(scan_opts, noexcl, 1);
		opt_set(scan_opts, nochanges, 1);
		opt_set(scan_opts, no_version_check, 1);

		/*
		 * If the device string contains ":" we will assume the user
		 * knows the entire list. If they supply a single device it
		 * could be either the FS only has 1 device or it's only 1 of a
		 * number of devices which are part of the FS. This appears to
		 * be the case when we get called during fstab mount processing
		 * and the fstab specifies a UUID.
		 */
		while ((tok = strsep(&p, ":"))) {
			struct bch_sb_handle h = { 0 };
			char *mpath;

			if ((mpath = find_multipath_holder(tok))) {
				warn_multipath_component(tok, mpath);
				free(mpath);
			}
			ret = bch2_read_super(tok, &scan_opts, &h);
			if (ret) {
				while (out->nr) {
					struct bch_scanned_sb *e = &out->data[out->nr - 1];

					free(e->path);
					bch2_free_super(&e->sb);
					out->nr--;
				}
				free(copy);
				return ret;
			}
			darray_push(out, ((struct bch_scanned_sb){
						 .path = strdup(tok), .sb = h }));
		}
		free(copy);
		return 0;
	}

	return devs_str_sbs_from_device(device, opts, use_udev, wait, out);
}

/*
 * Find a filesystem's members, without waiting for any that are absent.
 */
int bch2_scan_sbs(const char *device, struct bch_opts *opts,
		  bch_scanned_sbs *out)
{
	return scan_sbs_maybe_waiting(device, opts, false, out);
}

/*
 * The same, but wait for members that have not enumerated yet.
 *
 * Only mount wants this. Every other command that resolves a filesystem is
 * being run by someone at a prompt who already knows what is plugged in -
 * `bcachefs device remove` on a dead disk should not sit for
 * missing_dev_timeout before doing the thing it was asked to do.
 */
int bch2_scan_sbs_for_mount(const char *device, struct bch_opts *opts,
			    bch_scanned_sbs *out)
{
	return scan_sbs_maybe_waiting(device, opts, true, out);
}

/*
 * A udev monitor, and the question "have the missing members turned up?"
 *
 * The degraded prompt uses this so it can stop asking when the answer arrives
 * as hardware rather than as a keystroke. Someone who is asked whether to
 * mount without a disk, and responds by plugging the disk in, has answered.
 */
struct bch_device_watch {
	struct udev_monitor	*monitor;
	uuid_t			uuid;
	struct bch_opts		opts;
	bool			use_udev;
};

/*
 * `None` when there is no way to watch: no udev to tell us about arrivals.
 * Polling for a disk on a timer while a question is on screen is not worth
 * the code.
 */
struct bch_device_watch *bch2_device_watch_new(uuid_t uuid,
					       const struct bch_opts *opts,
					       bool use_udev)
{
	struct bch_device_watch *w;
	struct udev_monitor *m = block_device_monitor();

	if (!m)
		return NULL;
	w = xmalloc(sizeof(*w));
	w->monitor	= m;
	uuid_copy(w->uuid, uuid);
	w->opts		= *opts;
	w->use_udev	= use_udev;
	return w;
}

void bch2_device_watch_free(struct bch_device_watch *w)
{
	if (!w)
		return;
	udev_monitor_unref(w->monitor);
	free(w);
}

int bch2_device_watch_fd(struct bch_device_watch *w)
{
	return udev_monitor_get_fd(w->monitor);
}

/*
 * Drain what woke us and look again. True once every member is present.
 *
 * Rescans rather than trusting the devnode in the event, for the same reason
 * scan_waiting_for_devices() does: an arriving block device only means look
 * again, and the scan knows how - including the fallback for members udev has
 * not tagged yet.
 */
bool bch2_device_watch_every_member_present(struct bch_device_watch *w)
{
	struct udev_device *d;
	unsigned drained = 0;
	bch_scanned_sbs sbs = { 0 };
	bool have;
	int ret;

	while ((d = udev_monitor_receive_device(w->monitor))) {
		udev_device_unref(d);
		drained++;
	}
	if (!drained)
		return false;

	ret = bch2_get_devices_by_uuid(w->uuid, &w->opts, w->use_udev, &sbs);
	have = !ret && have_every_device(&sbs);
	bch2_scanned_sbs_exit(&sbs);
	return have;
}

/* The Rust module also carries open_online_or_offline()/open_scan(), which
 * need BcachefsHandle and Fs::open; those are part of the command-layer port
 * and live here when it lands. bch2_scan_devices() stays in commands/device.c
 * for the same reason. */