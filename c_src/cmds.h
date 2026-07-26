/*
 * Author: Kent Overstreet <kent.overstreet@gmail.com>
 *
 * GPLv2
 */

#ifndef _CMDS_H
#define _CMDS_H

int cmd_completions(int argc, char *argv[]);
int cmd_data_read(int argc, char *argv[]);
int cmd_device(int argc, char *argv[]);
int cmd_device_scan(int argc, char *argv[]);
int cmd_dump(int argc, char *argv[]);
int cmd_format(int argc, char *argv[]);
int cmd_fs(int argc, char *argv[]);
int cmd_fs_top(int argc, char *argv[]);
int cmd_fs_usage(int argc, char *argv[]);
int cmd_fsck(int argc, char *argv[]);
int cmd_image(int argc, char *argv[]);
int cmd_journal_rewind_info(int argc, char *argv[]);
int cmd_kill_btree_node(int argc, char *argv[]);
int cmd_list(int argc, char *argv[]);
int cmd_list_journal(int argc, char *argv[]);
int cmd_migrate(int argc, char *argv[]);
int cmd_migrate_superblock(int argc, char *argv[]);
int cmd_mount(int argc, char *argv[]);
int cmd_reconcile(int argc, char *argv[]);
int cmd_recover_super(int argc, char *argv[]);
int cmd_recovery_pass(int argc, char *argv[]);
int cmd_reflink_option_propagate(int argc, char *argv[]);
int cmd_remove_passphrase(int argc, char *argv[]);
int cmd_reset_counters(int argc, char *argv[]);
int cmd_scrub(int argc, char *argv[]);
int cmd_set_option(int argc, char *argv[]);
int cmd_set_passphrase(int argc, char *argv[]);
int cmd_setattr(int argc, char *argv[]);
int cmd_getattr(int argc, char *argv[]);
int cmd_show_super(int argc, char *argv[]);
int cmd_strip_alloc(int argc, char *argv[]);
int cmd_subvolume(int argc, char *argv[]);
int cmd_subvolume_create(int argc, char *argv[]);
int cmd_subvolume_delete(int argc, char *argv[]);
int cmd_subvolume_snapshot(int argc, char *argv[]);
int cmd_timestats(int argc, char *argv[]);
int cmd_undump(int argc, char *argv[]);
int cmd_unlock(int argc, char *argv[]);
int cmd_unpoison(int argc, char *argv[]);
int cmd_wait_devices(int argc, char *argv[]);

#ifdef BCACHEFS_FUSE
int cmd_fusemount(int argc, char *argv[]);
#endif

#endif /* _CMDS_H */
