/*
 * Superblock display with device names.
 *
 * Ported from src/wrappers/sb_display.rs.
 */

/*
 * Superblock display with device names — Rust replacement for the C
 * bch2_sb_to_text_with_names() in rust_shims.c.
 *
 * The C version called bch2_scan_device_sbs (Rust FFI) which returned
 * Vec-allocated memory via forget(), then freed it with darray_exit
 * (kvfree) — allocator mismatch causing heap corruption. This version
 * keeps everything in C (ported from Rust) so we avoid FFI boundary issues.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uuid/uuid.h>

#include "libbcachefs.h"
#include "sb/io.h"
#include "sb/members.h"

struct scanned_sb {
	const char *path;
	struct bch_sb_handle h;
};

typedef DARRAY(struct scanned_sb) scanned_sbs;

/*
 * Print one member device's info: name, model, and detailed member text.
 */
static void print_one_member(struct printbuf *out, scanned_sbs *sbs,
			     struct bch_fs *c, struct bch_sb *sb,
			     struct bch_sb_field_disk_groups *gi,
			     struct bch_member *m, unsigned idx)
{
	if (!bch2_member_alive(m))
		return;

	const char *path = "(not found)";
	struct bch_sb_handle *m_h = NULL;

	darray_for_each(*sbs, s) {
		if (s->h.sb->dev_idx == idx &&
		    !uuid_compare(s->h.sb->user_uuid.b, sb->user_uuid.b)) {
			path = s->path;
			m_h = &s->h;
			break;
		}
	}

	prt_printf(out, "Device %u:\t%s\t", idx, path);
	if (m_h) {
		char *model = fd_to_dev_model(m_h->bdev->bd_fd);
		if (model) {
			prt_printf(out, "%s", model);
			free(model);
		}
		char *serial = fd_to_dev_serial(m_h->bdev->bd_fd);
		if (serial) {
			prt_printf(out, "\tS/N: %s", serial);
			free(serial);
		}
	}
	prt_newline(out);

	printbuf_indent_add(out, 2);
	bch2_member_to_text(out, m, gi, sb, idx);
	printbuf_indent_sub(out, 2);
}

/*
 * Print superblock contents with device names.
 *
 * Scans for devices matching the superblock's UUID, then prints
 * superblock fields and per-member details with device paths and
 * hardware model names.
 */
void bch2_sb_to_text_with_names(struct printbuf *out, struct bch_fs *c,
				struct bch_sb *sb, bool print_layout,
				unsigned fields, int field_only)
{
	if (field_only >= 0) {
		struct bch_sb_field *f = bch2_sb_field_get_id(
			sb, (enum bch_sb_field_type)field_only);
		if (f)
			__bch2_sb_field_to_text(out, c, sb, f);
		return;
	}

	printbuf_tabstop_push(out, 44);

	unsigned member_mask = (1u << BCH_SB_FIELD_members_v1) |
			       (1u << BCH_SB_FIELD_members_v2);

	bch2_sb_to_text(out, c, sb, print_layout, fields & ~member_mask);

	char uuid_str[40];
	uuid_unparse(sb->user_uuid.b, uuid_str);
	char *device_str = mprintf("UUID=%s", uuid_str);

	darray_const_str scanned_paths = {};
	char *scanned_raw = bch2_scan_devices(device_str);
	if (scanned_raw)
		bch2_split_devs(scanned_raw, &scanned_paths);

	scanned_sbs sbs = {};
	darray_for_each(scanned_paths, p) {
		struct bch_opts opts = bch2_opts_empty();
		opt_set(opts, noexcl, true);
		opt_set(opts, nochanges, true);

		struct bch_sb_handle m_h;
		if (!bch2_read_super(*p, &opts, &m_h)) {
			struct scanned_sb s = { .path = *p, .h = m_h };
			darray_push(&sbs, s);
		}
	}

	struct bch_sb_field_disk_groups *gi =
		bch2_sb_field_get(sb, disk_groups);

	if (fields & (1u << BCH_SB_FIELD_members_v1)) {
		struct bch_sb_field_members_v1 *mi =
			bch2_sb_field_get(sb, members_v1);
		if (mi) {
			for (unsigned i = 0; i < sb->nr_devices; i++) {
				struct bch_member m =
					bch2_members_v1_get(mi, i);
				print_one_member(out, &sbs, c, sb, gi, &m, i);
			}
		}
	}

	if (fields & (1u << BCH_SB_FIELD_members_v2)) {
		struct bch_sb_field_members_v2 *mi =
			bch2_sb_field_get(sb, members_v2);
		if (mi) {
			for (unsigned i = 0; i < sb->nr_devices; i++) {
				struct bch_member m =
					bch2_members_v2_get(mi, i);
				print_one_member(out, &sbs, c, sb, gi, &m, i);
			}
		}
	}

	darray_for_each(sbs, s)
		bch2_free_super(&s->h);
	darray_exit(&sbs);
	darray_exit(&scanned_paths);
	free(scanned_raw);
	free(device_str);
}
