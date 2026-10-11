/*
 * wait-devices - retained, and deliberately does nothing.
 *
 * Ported from src/commands/wait_devices.rs ("wait-devices: do nothing, and
 * say so").
 *
 * It existed for bcachefs-wait-devices@.service, which fstab entries could
 * depend on via x-systemd.requires= so that a multi-device mount was not
 * attempted before udev had probed every member. mount.bcachefs now waits
 * for its own members (device_scan.c and the missing_dev_timeout option),
 * so the race it closed is closed a second time, one layer down.
 *
 * Which would make it merely redundant. It is worse than that: the wait had
 * no deadline of its own - poll() with a -1 timeout - so a member that was
 * never coming back blocked until systemd's DefaultTimeoutStartSec killed
 * the unit and failed the mount job. A dependency meant to protect the boot
 * pre-empted the code that could have saved it.
 *
 * So the command stays and succeeds immediately. It is the command that has
 * to be the no-op rather than the unit: the unit ships as a template, and
 * anyone who copied it into /etc/systemd/system/ to edit owns their copy
 * forever - that copy still calls this. Removing the command instead would
 * turn their satisfied dependency into a failed one, which is the boot
 * failure we are trying to stop happening.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "cmds.h"

static void wait_devices_usage(void)
{
	puts("bcachefs wait-devices - does nothing; kept so dependent units still succeed\n"
	     "Usage: bcachefs wait-devices UUID=<uuid>\n"
	     "\n"
	     "Does nothing, and exits zero. mount.bcachefs waits for its own member\n"
	     "devices now, so this no longer has anything to do - and waiting here\n"
	     "was actively harmful, because it had no timeout and would fail the\n"
	     "mount job for a device that was never coming back.\n"
	     "\n"
	     "Options:\n"
	     "  -h, --help              Display this help and exit\n");
}

/* device_scan.rs parse_uuid_equals: UUID=<uuid> or OLD_BLKID_UUID=<uuid> */
static bool parse_uuid_equals(const char *s)
{
	const char *eq = strchr(s, '=');

	if (!eq)
		return false;

	if (strncmp(s, "UUID=", 5) && strncmp(s, "OLD_BLKID_UUID=", 15))
		return false;

	uuid_t uuid;

	return !uuid_parse(eq + 1, uuid);
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

	char *dev = arg_pop();
	if (!dev) {
		wait_devices_usage();
		return -EINVAL;
	}

	/*
	 * Zero even on a bad argument: this must never be why a boot stops,
	 * and the mount that follows reports the real problem with the real
	 * context.
	 */
	if (!parse_uuid_equals(dev))
		fprintf(stderr,
			"wait-devices: not a UUID=<uuid> device string: %s\n",
			dev);

	fprintf(stderr,
		"wait-devices does nothing now; mount waits for its own devices\n");
	return 0;
}
