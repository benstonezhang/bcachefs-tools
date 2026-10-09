/*
 * Copy a POSIX directory tree into a bcachefs filesystem.
 *
 * Supports two distinct modes:
 *  - Copy (format --source): Data is physically written to the new filesystem.
 *  - Migrate (bcachefs migrate): Data extents are remapped to point directly
 *    to existing device blocks, enabling in-place migration.
 *
 * Ported from src/copy_fs.rs.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/blkdev.h>

#include "libbcachefs.h"
#include "alloc/buckets.h"
#include "alloc/foreground.h"
#include "data/io_misc.h"
#include "data/read.h"
#include "data/write.h"
#include "fs/dirent.h"
#include "fs/inode.h"
#include "fs/namei.h"
#include "fs/str_hash.h"
#include "fs/xattr.h"
#include "cmds.h"

#define BCH_SUBVOLUME(x)                                     \
	(subvol_inum)                                        \
	{                                                    \
		.subvol = BCACHEFS_ROOT_SUBVOL, .inum = (x), \
	}

static void unlink_and_rm(struct bch_fs *c, subvol_inum dir_inum,
			  struct bch_inode_unpacked *dir, const char *name)
{
	struct qstr qstr = QSTR(name);
	struct bch_inode_unpacked child;
	int ret;

	ret = bch2_trans_commit_do(c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
				   bch2_unlink_trans(trans, dir_inum, dir,
						     (subvol_inum){ 0 }, &child,
						     &qstr, false));
	if (ret)
		die("error unlinking %s: %s", name, bch2_err_str(ret));

	if (!(child.bi_flags & BCH_INODE_unlinked))
		return;

	ret = bch2_inode_rm(c, (subvol_inum){ dir_inum.subvol, child.bi_inum });
	if (ret)
		die("error removing inode: %s", bch2_err_str(ret));
}

static void update_inode(struct bch_fs *c, struct bch_inode_unpacked *inode)
{
	struct bkey_inode_buf packed;
	int ret;

	bch2_inode_pack(c, &packed, inode);
	packed.inode.k.p.snapshot = U32_MAX;
	ret = bch2_btree_insert(c, BTREE_ID_inodes, &packed.inode.k_i, NULL, 0,
				BTREE_ITER_cached);
	if (ret)
		die("error updating inode: %s", bch2_err_str(ret));
}

static void create_or_update_link(struct bch_fs *c, subvol_inum dir_inum,
				  struct bch_inode_unpacked *dir,
				  const char *name, subvol_inum inum)
{
	struct bch_hash_info dir_hash;
	int ret = bch2_hash_info_init(c, dir, &dir_hash);
	if (ret)
		die("initialize hash_info error %d", ret);

	struct qstr qstr = QSTR(name);
	subvol_inum old_inum;
	ret = bch2_dirent_lookup(c, dir_inum, &dir_hash, &qstr, &old_inum);
	if (ret == 0) {
		if (subvol_inum_eq(inum, old_inum))
			return;
		unlink_and_rm(c, dir_inum, dir, name);
	} else if (ret != -BCH_ERR_ENOENT_str_hash_lookup) {
		die("error looking up %s: %s", name, bch2_err_str(ret));
	}

	struct bch_inode_unpacked dir_u, inode;
	ret = bch2_trans_commit_do(c, NULL, NULL, 0,
				   bch2_link_trans(trans, dir_inum, &dir_u,
						   inum, &inode, &qstr));
	if (ret)
		die("error creating hardlink %s: %s", name, bch2_err_str(ret));
}

static struct bch_inode_unpacked
create_or_update_file(struct bch_fs *c, subvol_inum dir_inum,
		      struct bch_inode_unpacked *dir, const char *name,
		      uid_t uid, gid_t gid, mode_t mode, dev_t rdev)
{
	struct bch_hash_info dir_hash;
	int ret = bch2_hash_info_init(c, dir, &dir_hash);
	if (ret)
		die("initialize hash_info error %d", ret);

	struct qstr qname = QSTR(name);
	subvol_inum child_inum;
	struct bch_inode_unpacked child_inode;
	struct bch_subvolume child_subvol;

	ret = bch2_dirent_lookup(c, dir_inum, &dir_hash, &qname, &child_inum);
	if (ret == 0) {
		// Already exists — update
		ret = bch2_inode_find_by_inum(c, child_inum, &child_inode);
		if (ret)
			die("error finding inode: %s", bch2_err_str(ret));

		child_inode.bi_mode = (u16)mode;
		child_inode.bi_uid = uid;
		child_inode.bi_gid = gid;
		child_inode.bi_dev = (u32)rdev;

		// bch2_fsck_write_inode has its own commit_do loop internally —
		// don't wrap it in trans_commit_do or it double-nests.
		CLASS(btree_trans, trans)(c);
		ret = bch2_fsck_write_inode(trans, &child_inode);
		if (ret)
			die("error updating inode %s: %s", name,
			    bch2_err_str(ret));
	} else {
		bch2_inode_init_early(c, &child_inode);

		ret = bch2_trans_commit_do(
			c, NULL, NULL, 0,
			bch2_create_trans(trans, dir_inum, dir, &child_inode,
					  &child_subvol, &qname, uid, gid,
					  (u16)mode, rdev, NULL, NULL,
					  (subvol_inum){ 0 }, 0));
		if (ret)
			die("error creating %s: %s", name, bch2_err_str(ret));
	}

	return child_inode;
}

/*
 * Resolve xattr name prefix to bcachefs xattr index.
 * Returns prefix length, or -1 if unsupported.
 */
static int xattr_resolve_name(const char *name, int *type)
{
	if (!strncmp(name, "user.", 5)) {
		*type = KEY_TYPE_XATTR_INDEX_USER;
		return 5;
	}
	if (!strncmp(name, "trusted.", 8)) {
		*type = KEY_TYPE_XATTR_INDEX_TRUSTED;
		return 8;
	}
	if (!strncmp(name, "security.", 9)) {
		*type = KEY_TYPE_XATTR_INDEX_SECURITY;
		return 9;
	}
	if (!strncmp(name, "system.posix_acl_access", 23)) {
		*type = KEY_TYPE_XATTR_INDEX_POSIX_ACL_ACCESS;
		return 23;
	}
	if (!strncmp(name, "system.posix_acl_default", 24)) {
		*type = KEY_TYPE_XATTR_INDEX_POSIX_ACL_DEFAULT;
		return 24;
	}
	return -1;
}

static void copy_times(struct bch_fs *c, struct bch_inode_unpacked *dst,
		       struct stat *src)
{
	dst->bi_atime = timespec_to_bch2_time(c, src->st_atim);
	dst->bi_mtime = timespec_to_bch2_time(c, src->st_mtim);
	dst->bi_ctime = timespec_to_bch2_time(c, src->st_ctim);
}

static void copy_xattrs(struct bch_fs *c, struct bch_inode_unpacked *dst,
			const char *src)
{
	char attrs[65536];
	ssize_t attrs_size = llistxattr(src, attrs, sizeof(attrs));

	if (attrs_size < 0)
		return; // silently skip if xattrs not supported

	char *next, *attr;
	int xattr_type;
	char val[65536];

	for (attr = attrs; attr < attrs + attrs_size; attr = next) {
		next = attr + strlen(attr) + 1;

		int prefix_len = xattr_resolve_name(attr, &xattr_type);
		if (prefix_len < 0)
			continue;

		ssize_t val_size = lgetxattr(src, attr, val, sizeof(val));
		if (val_size < 0)
			continue;

		int ret = bch2_trans_commit_do(
			c, NULL, NULL, 0,
			bch2_xattr_set(trans, BCH_SUBVOLUME(dst->bi_inum), dst,
				       attr + prefix_len, val, val_size,
				       xattr_type, 0));
		if (ret < 0)
			die("error creating xattr: %s", bch2_err_str(ret));
	}
}

static void write_data(struct bch_fs *c, struct bch_inode_unpacked *dst_inode,
		       u64 dst_offset, void *buf, size_t len)
{
	struct bch_write_op op;
	struct bio_vec bv[WRITE_DATA_BUF / PAGE_SIZE];

	int ret = bch2_write_submit(c, &op, bv, ARRAY_SIZE(bv), buf, len,
				    dst_inode->bi_inum, dst_offset,
				    BCACHEFS_ROOT_SUBVOL, 1, U64_MAX);
	if (ret)
		die("error reserving space in new filesystem: %s",
		    bch2_err_str(ret));

	dst_inode->bi_sectors += op.i_sectors_delta;

	if (op.error)
		die("write error: %s", bch2_err_str(op.error));
}

static void copy_data(struct bch_fs *c, struct bch_inode_unpacked *dst_inode,
		      int src_fd, u64 start, u64 end)
{
	char *buf = xaligned_alloc(PAGE_SIZE, WRITE_DATA_BUF);
	while (start < end) {
		unsigned len = min_t(u64, end - start, WRITE_DATA_BUF);
		unsigned pad = round_up(len, block_bytes(c)) - len;

		xpread(src_fd, buf, len, start);
		if (pad)
			memset(buf + len, 0, pad);
		write_data(c, dst_inode, start, buf, len + pad);
		start += len;
	}
	free(buf);
}

static void link_data(struct bch_fs *c, struct bch_inode_unpacked *dst,
		      u64 logical, u64 physical, u64 length)
{
	s64 sectors_delta;
	int ret = bch2_link_data(c, dst->bi_inum, &sectors_delta, logical,
				 physical, length);
	if (ret)
		die("btree link_data error %s", bch2_err_str(ret));

	dst->bi_sectors += sectors_delta;
}

static void copy_link(struct bch_fs *c, subvol_inum dst_inum,
		      struct bch_inode_unpacked *dst, const char *src)
{
	s64 i_sectors_delta = 0;
	int ret = bch2_fpunch(c, dst_inum, 0, U64_MAX, &i_sectors_delta);
	if (ret)
		die("bch2_fpunch error: %d", ret);

	dst->bi_sectors += i_sectors_delta;

	unsigned len = round_up(PATH_MAX, block_bytes(c));
	char *buf = xaligned_alloc(PAGE_SIZE, len);

	ssize_t rlen = readlink(src, buf, len);
	if (rlen < 0)
		die("readlink error: %m");

	memset(buf + rlen, 0, len - rlen);
	write_data(c, dst, 0, buf, len);
	free(buf);
}

static void link_file_data(struct bch_fs *c, struct copy_fs_state *s,
			   struct bch_inode_unpacked *dst, int src_fd,
			   const char *src_path, u64 src_size)
{
	struct fiemap_iter iter;
	struct fiemap_extent e;

	// First pass: check for unknown extents and fsync if found
	fiemap_for_each(src_fd, iter, e)
		if (e.fe_flags & FIEMAP_EXTENT_UNKNOWN) {
			fsync(src_fd);
			break;
		}
	fiemap_iter_exit(&iter);

	// Second pass: link or copy extents
	fiemap_for_each(src_fd, iter, e) {
		s->total_input += e.fe_length;

		u64 src_max = round_up(src_size, block_bytes(c));
		u64 length = min(e.fe_length, src_max - e.fe_logical);
		unsigned visible_len = min(src_size - e.fe_logical, length);

		if ((e.fe_logical & (block_bytes(c) - 1)) ||
		    (length & (block_bytes(c) - 1)))
			die("Unaligned extent in %s - can't handle", src_path);

		bool needs_copy = s->type == BCH_MIGRATE_copy ||
				  (e.fe_flags & (FIEMAP_EXTENT_UNKNOWN |
						 FIEMAP_EXTENT_ENCODED |
						 FIEMAP_EXTENT_NOT_ALIGNED |
						 FIEMAP_EXTENT_DATA_INLINE));

		if (needs_copy) {
			copy_data(c, dst, src_fd, e.fe_logical,
				  e.fe_logical + visible_len);
			s->total_wrote += visible_len;
			continue;
		}

		/* If the data is in bcachefs's superblock region, copy it: */
		if (e.fe_physical < s->reserve_start) {
			copy_data(c, dst, src_fd, e.fe_logical,
				  e.fe_logical + visible_len);
			s->total_wrote += visible_len;
			continue;
		}

		if ((e.fe_physical & (block_bytes(c) - 1)))
			die("Unaligned extent in %s - can't handle", src_path);

		range_add(&s->extents, e.fe_physical, length);
		link_data(c, dst, e.fe_logical, e.fe_physical, length);
		s->total_linked += length;
	}
	fiemap_iter_exit(&iter);
}

static struct range align_range(struct range r, u64 bs)
{
	return (struct range){
		.start = r.start / bs * bs,
		.end = round_up(r.end, bs),
	};
}

static struct range seek_data(int fd, u64 i_size, u64 offset)
{
	off_t s = lseek(fd, offset, SEEK_DATA);
	if (s < 0)
		return (struct range){ 0, 0 };

	off_t e = lseek(fd, s, SEEK_HOLE);
	if (e < 0)
		e = i_size;

	return (struct range){ (u64)s, (u64)e };
}

static struct range seek_data_aligned(int fd, u64 i_size, u64 offset, u64 bs)
{
	struct range r = align_range(seek_data(fd, i_size, offset), bs);

	if (r.end == 0)
		return r;

	while (1) {
		struct range n = align_range(seek_data(fd, i_size, r.end), bs);
		if (n.end == 0 || r.end < n.start)
			break;
		r.end = n.end;
	}
	return r;
}

static struct range seek_mismatch(const u8 *buf1, const u8 *buf2, size_t offset,
				  size_t len)
{
	while (offset < len && buf1[offset] == buf2[offset])
		offset++;

	if (offset == len)
		return (struct range){ 0, 0 };

	size_t start = offset;

	while (offset < len && buf1[offset] != buf2[offset])
		offset++;

	return (struct range){ (u64)start, (u64)offset };
}

static struct range seek_mismatch_aligned(const u8 *buf1, const u8 *buf2,
					  size_t offset, size_t len, u64 bs)
{
	struct range r =
		align_range(seek_mismatch(buf1, buf2, offset, len), bs);

	if (r.end != 0) {
		while (1) {
			struct range n = align_range(
				seek_mismatch(buf1, buf2, r.end, len), bs);
			if (n.end == 0 || r.end < n.start)
				break;
			r.end = n.end;
		}
	}
	return r;
}

static void copy_sync_file_range(struct bch_fs *c, struct copy_fs_state *s,
				 subvol_inum dst_inum,
				 struct bch_inode_unpacked *dst, int src_fd,
				 u64 src_size, struct range range)
{
	u64 block_size = block_bytes(c);
	u64 start = range.start;

	char *src_buf = xaligned_alloc(PAGE_SIZE, WRITE_DATA_BUF);
	char *dst_buf = xaligned_alloc(PAGE_SIZE, WRITE_DATA_BUF);

	while (start != range.end) {
		unsigned b = min_t(u64, range.end - start, WRITE_DATA_BUF);
		unsigned read_len = min_t(u64, b, src_size - start);

		xpread(src_fd, src_buf, read_len, start);
		if (b != read_len)
			memset(src_buf + read_len, 0, b - read_len);

		bchu_fs_read(c, dst_inum, start, dst, dst_buf, b);

		struct range m = { 0, 0 };

		while (1) {
			m = seek_mismatch_aligned(src_buf, dst_buf, m.end, b,
						  block_size);
			if (m.end == 0)
				break;
			write_data(c, dst, start + m.start, src_buf + m.start,
				   m.end - m.start);
			s->total_wrote += m.end - m.start;
		}

		start += b;
	}

	free(src_buf);
	free(dst_buf);
}

static void copy_sync_file_data(struct bch_fs *c, struct copy_fs_state *s,
				subvol_inum dst_inum,
				struct bch_inode_unpacked *dst, int src_fd,
				u64 src_size)
{
	s64 i_sectors_delta = 0;
	struct range prev = { 0, 0 };
	int ret;

	while (1) {
		struct range next = seek_data_aligned(src_fd, src_size,
						      prev.end, block_bytes(c));
		if (!next.end)
			break;

		if (next.start) {
			ret = bch2_fpunch(c, dst_inum, prev.end >> 9,
					  next.start >> 9, &i_sectors_delta);
			if (ret)
				die("bch2_fpunch error: %d", ret);
		}

		copy_sync_file_range(c, s, dst_inum, dst, src_fd, src_size,
				     next);
		s->total_input += next.end - next.start;
		prev = next;
	}

	ret = bch2_fpunch(c, dst_inum, prev.end >> 9, U64_MAX,
			  &i_sectors_delta);
	if (ret)
		die("bch2_fpunch error: %d", ret);
}

// Directory entry from bcachefs readdir.
struct dir_entry {
	u64 inum;
	u8 dtype;
	char *name;
	struct stat stat;
};

typedef DARRAY(struct dir_entry) dir_entries;

// Trampoline for bch2_readdir callback.
struct readdir_ctx {
	struct dir_context ctx;
	dir_entries *entries;
};

static int readdir_actor(struct dir_context *ctx, const char *name, int len,
			 loff_t pos, u64 inum, unsigned dtype)
{
	struct readdir_ctx *rctx = container_of(ctx, struct readdir_ctx, ctx);
	struct dir_entry de = {
		.inum = inum,
		.dtype = dtype,
		.name = xstrndup(name, len),
	};
	darray_push(rctx->entries, de);
	return 0;
}

static int dirent_cmp(const void *_a, const void *_b)
{
	const struct dir_entry *a = _a;
	const struct dir_entry *b = _b;

	if (a->dtype != b->dtype)
		return (int)a->dtype - (int)b->dtype;
	return strcmp(a->name, b->name);
}

static int simple_readdir(struct bch_fs *c, subvol_inum inum,
			  struct bch_inode_unpacked *inode,
			  dir_entries *entries)
{
	struct bch_hash_info hash;
	int ret = bch2_hash_info_init(c, inode, &hash);
	if (ret)
		die("initialize hash_info error %d", ret);

	struct readdir_ctx rctx = {
		.ctx.actor = readdir_actor,
		.entries = entries,
	};

	ret = bch2_readdir(c, inum, &hash, &rctx.ctx);
	if (ret)
		return ret;

	qsort(entries->data, entries->nr, sizeof(struct dir_entry), dirent_cmp);
	return 0;
}

static void recursive_remove(struct bch_fs *c, subvol_inum dir_inum,
			     struct bch_inode_unpacked *dir,
			     struct dir_entry *d)
{
	subvol_inum child_inum = { .subvol = dir_inum.subvol, .inum = d->inum };
	struct bch_inode_unpacked child;
	int ret;

	ret = bch2_inode_find_by_inum(c, child_inum, &child);
	if (ret)
		die("error finding inode: %s", bch2_err_str(ret));

	if (S_ISDIR(child.bi_mode)) {
		dir_entries child_dirents = { 0 };
		ret = simple_readdir(c, child_inum, &child, &child_dirents);
		if (ret)
			die("error reading directory: %s", bch2_err_str(ret));

		darray_for_each(child_dirents, entry)
			recursive_remove(c, child_inum, &child, entry);
		darray_for_each(child_dirents, entry) free(entry->name);
		darray_exit(&child_dirents);
	}

	unlink_and_rm(c, dir_inum, dir, d->name);
}

static void delete_non_matching_dirents(struct bch_fs *c,
					struct copy_fs_state *s,
					subvol_inum dst_dir_inum,
					struct bch_inode_unpacked *dst_dir,
					dir_entries *src_dirents)
{
	dir_entries dst_dirents = { 0 };
	int ret = simple_readdir(c, dst_dir_inum, dst_dir, &dst_dirents);
	if (ret)
		die("error reading directory: %s", bch2_err_str(ret));

	size_t src_idx = 0;

	darray_for_each(dst_dirents, dst_d)
	{
		while (src_idx < src_dirents->nr &&
		       dirent_cmp(&src_dirents->data[src_idx], dst_d) < 0)
			src_idx++;

		bool matches = src_idx < src_dirents->nr &&
			       dirent_cmp(&src_dirents->data[src_idx], dst_d) ==
				       0;

		if (!matches) {
			if (subvol_inum_eq(dst_dir_inum,
					   BCACHEFS_ROOT_SUBVOL_INUM) &&
			    !strcmp(dst_d->name, "lost+found"))
				continue;

			if (s->verbosity > 1)
				printf("deleting %s\n", dst_d->name);

			recursive_remove(c, dst_dir_inum, dst_dir, dst_d);
		}
	}

	darray_for_each(dst_dirents, entry) free(entry->name);
	darray_exit(&dst_dirents);
}

static void copy_dir(struct bch_fs *c, struct copy_fs_state *s,
		     struct bch_inode_unpacked *dst, int src_fd,
		     const char *src_path)
{
	// creating an internal dup for iteration,
	// so src_fd remains available for fstatat/openat/fchdir below.
	DIR *dir = fdopendir(dup(src_fd));
	if (!dir)
		die("error opening directory: %m");

	dir_entries dirents = { 0 };
	struct dirent *d;

	while (1) {
		errno = 0;
		d = readdir(dir);
		if (d == NULL) {
			if (errno)
				die("readdir failed: %m");
			break;
		}
		struct stat stat;
		if (fstatat(src_fd, d->d_name, &stat, AT_SYMLINK_NOFOLLOW))
			continue;

		struct dir_entry de = { .inum = d->d_ino,
					.dtype = d->d_type,
					.name = xstrdup(d->d_name),
					.stat = stat };
		darray_push(&dirents, de);
	}

	// Sort by (type, name)
	qsort(dirents.data, dirents.nr, sizeof(struct dir_entry), dirent_cmp);

	subvol_inum dir_inum = BCH_SUBVOLUME(dst->bi_inum);
	delete_non_matching_dirents(c, s, dir_inum, dst, &dirents);

	darray_for_each(dirents, entry)
	{
		if (fchdir(src_fd))
			continue;

		if (!strcmp(entry->name, ".") || !strcmp(entry->name, "..") ||
		    !strcmp(entry->name, "lost+found"))
			continue;

		if (s->type == BCH_MIGRATE_migrate &&
		    entry->stat.st_ino == s->bcachefs_inum)
			continue;

		s->total_files++;

		char *child_path = mprintf("%s/%s", src_path, entry->name);

		if (s->type == BCH_MIGRATE_migrate &&
		    entry->stat.st_dev != s->dev)
			die("%s does not have correct st_dev!", child_path);

		// Hardlink handling
		if (S_ISREG(entry->stat.st_mode) && entry->stat.st_nlink > 1) {
			u64 *dst_ino =
				genradix_ptr(&s->hardlinks, entry->stat.st_ino);
			if (dst_ino && *dst_ino) {
				create_or_update_link(c, dir_inum, dst,
						      entry->name,
						      BCH_SUBVOLUME(*dst_ino));
				free(child_path);
				continue;
			}
		}

		struct bch_inode_unpacked inode = create_or_update_file(
			c, dir_inum, dst, entry->name, entry->stat.st_uid,
			entry->stat.st_gid, entry->stat.st_mode,
			entry->stat.st_rdev);

		// Record hardlink destination
		if (S_ISREG(entry->stat.st_mode) && entry->stat.st_nlink > 1) {
			u64 *dst_ino = genradix_ptr_alloc(
				&s->hardlinks, entry->stat.st_ino, GFP_KERNEL);
			*dst_ino = inode.bi_inum;
		}

		copy_xattrs(c, &inode, entry->name);

		if (S_ISDIR(entry->stat.st_mode)) {
			int fd = xopenat(src_fd, entry->name,
					 O_RDONLY | O_NOATIME);
			copy_dir(c, s, &inode, fd, child_path);
			close(fd);
		} else if (S_ISREG(entry->stat.st_mode)) {
			inode.bi_size = entry->stat.st_size;

			int fd = xopenat(src_fd, entry->name,
					 O_RDONLY | O_NOATIME);
			if (s->type == BCH_MIGRATE_migrate) {
				link_file_data(c, s, &inode, fd, child_path,
					       entry->stat.st_size);
			} else {
				copy_sync_file_data(
					c, s, BCH_SUBVOLUME(inode.bi_inum),
					&inode, fd, entry->stat.st_size);
			}
			close(fd);
		} else if (S_ISLNK(entry->stat.st_mode)) {
			inode.bi_size = entry->stat.st_size;
			copy_link(c, BCH_SUBVOLUME(inode.bi_inum), &inode,
				  entry->name);
		}

		copy_times(c, &inode, &entry->stat);
		update_inode(c, &inode);
		free(child_path);
	}

	darray_for_each(dirents, entry) free(entry->name);
	darray_exit(&dirents);
	closedir(dir);
}

static void reserve_old_fs_space(struct bch_fs *c,
				 struct bch_inode_unpacked *root_inode,
				 ranges *extents, u64 reserve_start)
{
	struct bch_dev *ca = c->devs[0];
	struct bch_inode_unpacked dst;
	struct hole_iter iter;
	struct range i;
	sector_t total_sectors = bucket_to_sector(ca, ca->mi.nbuckets);

	dst = create_or_update_file(c, BCH_SUBVOLUME(root_inode->bi_inum),
				    root_inode, "old_migrated_filesystem", 0, 0,
				    S_IFREG | 0400, 0);
	dst.bi_size = total_sectors << 9;

	ranges_sort_merge(extents);

	for_each_hole(iter, *extents, total_sectors << 9, i)
	{
		if (i.end <= reserve_start)
			continue;

		u64 start = max(i.start, reserve_start);
		link_data(c, &dst, start, start, i.end - start);
	}

	update_inode(c, &dst);
}

// Copy a POSIX directory tree into a bcachefs filesystem.
//
// Entry point for both `bcachefs format --source` and `bcachefs migrate`.
void copy_fs(struct bch_fs *c, int src_fd, const char *src_path,
	     struct copy_fs_state *s)
{
	struct stat stat = xfstat(src_fd);
	if (!S_ISDIR(stat.st_mode))
		die("%s is not a directory", src_path);

	if (s->type == BCH_MIGRATE_migrate)
		syncfs(src_fd);

	struct bch_inode_unpacked root_inode;
	int ret = bch2_inode_find_by_inum(c, BCACHEFS_ROOT_SUBVOL_INUM,
					  &root_inode);
	if (ret)
		die("error looking up root directory: %s", bch2_err_str(ret));

	if (fchdir(src_fd))
		die("fchdir error: %m");

	copy_times(c, &root_inode, &stat);
	copy_xattrs(c, &root_inode, ".");

	/* now, copy: */
	copy_dir(c, s, &root_inode, src_fd, src_path);

	if (s->type == BCH_MIGRATE_migrate)
		reserve_old_fs_space(c, &root_inode, &s->extents,
				     s->reserve_start);

	update_inode(c, &root_inode);

	printf("Total files:\t%llu\n", s->total_files);
	printf("Total input:\t%s\n", fmt_bytes_human(s->total_input));

	if (s->total_wrote > 0)
		printf("Wrote:\t\t%s\n", fmt_bytes_human(s->total_wrote));

	if (s->total_linked > 0)
		printf("Linked:\t\t%s\n", fmt_bytes_human(s->total_linked));

	darray_exit(&s->extents);
	genradix_free(&s->hardlinks);
}
