/*
 * Authors: Kent Overstreet <kent.overstreet@gmail.com>
 *	    Gabriel de Perthuis <g2p.code@gmail.com>
 *	    Jacob Malevich <jam@datera.io>
 *	    Benstone Zhang <benstonezhang@gmail.com>
 *
 * GPLv2
 */

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <raid/raid.h>

#include "libbcachefs.h"
#include "cmds.h"

void bcachefs_usage(void)
{
	puts("bcachefs - tool for managing bcachefs filesystems\n"
	     "usage: bcachefs <command> [<args>]\n"
	     "\n"
	     "Superblock commands:\n"
	     "    format                    Format a new filesystem\n"
	     "    show-super                Print superblock information\n"
	     "    recover-super             Recover damaged superblock\n"
	     "    set-fs-option             Set filesystem options\n"
	     "    reset-counters            Reset filesystem counters\n"
	     "    strip-alloc               Strip alloc info for read-only use\n"
	     "\n"
	     "Images:\n"
	     "    image create              Create a filesystem image\n"
	     "    image update              Update a filesystem image\n"
	     "\n"
	     "Mount:\n"
	     "    mount                     Mount a filesystem\n"
	     "    wait-devices              Wait for devices to appear\n"
#ifdef BCACHEFS_FUSE
	     "    fusemount                 FUSE mount\n"
#endif
	     "\n"
	     "Repair:\n"
	     "    fsck                      Check filesystem consistency\n"
	     "    journal-rewind-info       Show journal rewind candidates\n"
	     "    recovery-pass             Manage recovery passes\n"
	     "    damage                    Show recorded filesystem damage\n"
	     "\n"
	     "Running filesystem:\n"
	     "    fs usage                  Show filesystem disk usage\n"
	     "    fs failure-domains        Show failure domains and what losing each would cost\n"
	     "    fs top                    Show live performance counters\n"
	     "    fs timestats              Show operation latency statistics\n"
	     "\n"
	     "Devices:\n"
	     "    device add                Add a device to a filesystem\n"
	     "    device online             Bring a device online\n"
	     "    device offline            Take a device offline\n"
	     "    device remove             Remove a device\n"
	     "    device evacuate           Evacuate data from a device\n"
	     "    device set-state          Set device state\n"
	     "    device resize             Resize filesystem on a device\n"
	     "    device resize-journal     Resize journal on a device\n"
	     "\n"
	     "Subvolumes and snapshots:\n"
	     "    subvolume                 Manage subvolumes and snapshots\n"
	     "\n"
	     "Filesystem data:\n"
	     "    reconcile status          Show reconcile status\n"
	     "    reconcile wait            Wait for reconcile to complete\n"
	     "    scrub                     Verify data checksums\n"
	     "\n"
	     "Encryption:\n"
	     "    unlock                    Unlock an encrypted filesystem\n"
	     "    set-passphrase            Set or change passphrase\n"
	     "    remove-passphrase         Remove passphrase\n"
	     "\n"
	     "Migrate:\n"
	     "    migrate                   Migrate existing filesystem to bcachefs\n"
	     "    migrate-superblock        Move superblock to standard location\n"
	     "\n"
	     "File options:\n"
	     "    set-file-option           Set file-level options\n"
	     "    get-file-option           Show file-level options\n"
	     "    reflink-option-propagate  Propagate options to reflinked files\n"
	     "\n"
	     "Debug:\n"
	     "    dump                      Dump filesystem metadata\n"
	     "    undump                    Restore dumped metadata\n"
	     "    list                      List filesystem metadata\n"
	     "    list_journal              List journal entries\n"
	     "    kill_btree_node           Remove a btree node\n"
	     "    data-read                 Read data with extended error info\n"
	     "    unpoison                  Clear poison flags on file extents\n"
	     "\n"
	     "Miscellaneous:\n"
	     "    completions               Generate shell completions\n"
	     "    version                  Display version\n");
}

static inline int cmd_version(int argc, char *argv[])
{
	printf("%s\n", VERSION_STRING);
	return 0;
}

int main(int argc, char *argv[])
{
	raid_init();

	setvbuf(stdout, NULL, _IOLBF, 0);

	char *full_cmd = argv[0];
	char *cmd_name = NULL;

	/* Are we being called via a symlink? */

	if (strstr(full_cmd, "mkfs"))
		cmd_name = "format";
	else if (strstr(full_cmd, "fsck"))
		cmd_name = "fsck";
#ifdef BCACHEFS_FUSE
	else if (strstr(full_cmd, "mount.fuse"))
		cmd_name = "fusemount";
#endif
	else if (strstr(full_cmd, "mount"))
		cmd_name = "mount";

	if (cmd_name) {
		/*
		 * For symlink invocations, we don't pop_cmd because argv[0] is the
		 * command itself.
		 */
	} else {
		cmd_name = pop_cmd(&argc, argv);
		if (!cmd_name) {
			puts("missing command\n");
			goto usage;
		}
	}

	if (!strcmp(cmd_name, "--help") || !strcmp(cmd_name, "-h") ||
	    !strcmp(cmd_name, "help")) {
		bcachefs_usage();
		return 0;
	}

	/*
	 * fuse will call this after daemonizing, we can't create threads before
	 * note that mount may invoke fusemount, via -t bcachefs.fuse
	 */
	if (strcmp(cmd_name, "mount") && strcmp(cmd_name, "fusemount"))
		linux_shrinkers_init();

	/* these subcommands display usage when argc < 2 */
	if (!strcmp(cmd_name, "device"))
		return cmd_device(argc, argv);
	if (!strcmp(cmd_name, "fs"))
		return cmd_fs(argc, argv);
	if (!strcmp(cmd_name, "reconcile"))
		return cmd_reconcile(argc, argv);
	if (!strcmp(cmd_name, "subvolume"))
		return cmd_subvolume(argc, argv);
	if (!strcmp(cmd_name, "format"))
		return cmd_format(argc, argv);
	if (!strcmp(cmd_name, "fsck"))
		return cmd_fsck(argc, argv);
	if (!strcmp(cmd_name, "journal-rewind-info"))
		return cmd_journal_rewind_info(argc, argv);
	if (!strcmp(cmd_name, "version"))
		return cmd_version(argc, argv);
	if (!strcmp(cmd_name, "show-super"))
		return cmd_show_super(argc, argv);
	if (!strcmp(cmd_name, "recover-super"))
		return cmd_recover_super(argc, argv);
	if (!strcmp(cmd_name, "recovery-pass"))
		return cmd_recovery_pass(argc, argv);
	if (!strcmp(cmd_name, "damage"))
		return cmd_damage(argc, argv);
	if (!strcmp(cmd_name, "set-fs-option"))
		return cmd_set_option(argc, argv);
	if (!strcmp(cmd_name, "reset-counters"))
		return cmd_reset_counters(argc, argv);
	if (!strcmp(cmd_name, "strip-alloc"))
		return cmd_strip_alloc(argc, argv);

	if (!strcmp(cmd_name, "image"))
		return cmd_image(argc, argv);

	if (!strcmp(cmd_name, "unlock"))
		return cmd_unlock(argc, argv);
	if (!strcmp(cmd_name, "set-passphrase"))
		return cmd_set_passphrase(argc, argv);
	if (!strcmp(cmd_name, "remove-passphrase"))
		return cmd_remove_passphrase(argc, argv);

	if (!strcmp(cmd_name, "migrate"))
		return cmd_migrate(argc, argv);
	if (!strcmp(cmd_name, "migrate-superblock"))
		return cmd_migrate_superblock(argc, argv);

	if (!strcmp(cmd_name, "dump"))
		return cmd_dump(argc, argv);
	if (!strcmp(cmd_name, "undump"))
		return cmd_undump(argc, argv);
	if (!strcmp(cmd_name, "list"))
		return cmd_list(argc, argv);
	if (!strcmp(cmd_name, "list_journal"))
		return cmd_list_journal(argc, argv);
	if (!strcmp(cmd_name, "kill_btree_node"))
		return cmd_kill_btree_node(argc, argv);

	if (!strcmp(cmd_name, "setattr") ||
	    !strcmp(cmd_name, "set-file-option"))
		return cmd_setattr(argc, argv);
	if (!strcmp(cmd_name, "getattr") ||
	    !strcmp(cmd_name, "get-file-option"))
		return cmd_getattr(argc, argv);
	if (!strcmp(cmd_name, "unpoison"))
		return cmd_unpoison(argc, argv);
	if (!strcmp(cmd_name, "data-read"))
		return cmd_data_read(argc, argv);
	if (!strcmp(cmd_name, "reflink-option-propagate"))
		return cmd_reflink_option_propagate(argc, argv);

	if (!strcmp(cmd_name, "completions"))
		return cmd_completions(argc, argv);

#ifdef BCACHEFS_FUSE
	if (!strcmp(cmd_name, "fusemount"))
		return cmd_fusemount(argc, argv);
#endif

	if (!strcmp(cmd_name, "mount"))
		return cmd_mount(argc, argv);
	if (!strcmp(cmd_name, "wait-devices"))
		return cmd_wait_devices(argc, argv);
	if (!strcmp(cmd_name, "scrub"))
		return cmd_scrub(argc, argv);

	printf("Unknown command %s\n", cmd_name);
usage:
	bcachefs_usage();
	exit(EXIT_FAILURE);
}
