#ifdef BCACHEFS_FUSE

#include <fuse_lowlevel.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "libbcachefs.h"
#include "btree/iter.h"
#include "fs/dirent.h"
#include "fs/namei.h"
#include "fs/inode.h"
#include "alloc/accounting.h"
#include "alloc/buckets.h"
#include "alloc/foreground.h"
#include "data/read.h"
#include "data/write.h"
#include "init/fs.h"
#include "cmds.h"

/* Forward declarations */
static void parse_fusemount_options(const char *options,
				    struct printbuf *fs_opts,
				    struct printbuf *fuse_opts);

/* ---- Thread initialization and RCU ---- */

static __thread bool thread_initialized = false;
static pthread_key_t rcu_key;

static void rcu_thread_destructor(void *ptr)
{
	rcu_unregister_thread();
}

static void ensure_thread_init(void)
{
	if (!thread_initialized) {
		if (!current) {
			struct task_struct *p = xcalloc(1, sizeof(*p));
			p->state = TASK_RUNNING;
			atomic_set(&p->usage, 1);
			init_completion(&p->exited);
			current = p;
		}
		rcu_register_thread();
		bch_percpu_thread_init();
		pthread_setspecific(rcu_key, (void *)1);
		thread_initialized = true;
	}
}

/* ---- Helper structures and constants ---- */

struct bcachefs_fuse {
	struct bch_fs *c;
	int signal_fd;
};

/*
 * Bytes the daemonising child sends its parent over the sync pipe.
 *
 * The parent cannot see the child's stderr -- daemon mode sends it to
 * /dev/null, deliberately -- so anything it is to report has to come through
 * here. Reporting every failure as a FUSE problem sends people looking in the
 * wrong place.
 */
#define CHILD_OK		0
#define CHILD_ERR_FS_START	1
#define CHILD_ERR_MOUNT		2
/* Between the two: the filesystem is up but we never reached mount. */
#define CHILD_ERR_SETUP		3

/* Bounded well under a pipe buffer so the child never blocks writing it, even
 * if the parent is slow to read. */
#define CHILD_MSG_MAX		512

#define TTL 1e18

static inline subvol_inum map_root_ino(fuse_ino_t ino)
{
	return (subvol_inum){ 1, ino == 1 ? BCACHEFS_ROOT_INO : (u64)ino };
}

static inline fuse_ino_t unmap_root_ino(u64 inum)
{
	return inum == BCACHEFS_ROOT_INO ? 1 : (fuse_ino_t)inum;
}

static void inode_to_stat(struct bch_fs *c, struct bch_inode_unpacked *bi,
			  struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = unmap_root_ino(bi->bi_inum);
	st->st_size = bi->bi_size;
	st->st_blocks = bi->bi_sectors;
	st->st_mode = bi->bi_mode;
	st->st_nlink = bch2_inode_nlink_get(bi);
	st->st_uid = bi->bi_uid;
	st->st_gid = bi->bi_gid;
	st->st_rdev = bi->bi_dev;
	st->st_blksize = block_bytes(c);

	struct timespec64 ts_a = bch2_time_to_timespec(c, bi->bi_atime);
	struct timespec64 ts_m = bch2_time_to_timespec(c, bi->bi_mtime);
	struct timespec64 ts_c = bch2_time_to_timespec(c, bi->bi_ctime);

	st->st_atim.tv_sec = ts_a.tv_sec;
	st->st_atim.tv_nsec = ts_a.tv_nsec;
	st->st_mtim.tv_sec = ts_m.tv_sec;
	st->st_mtim.tv_nsec = ts_m.tv_nsec;
	st->st_ctim.tv_sec = ts_c.tv_sec;
	st->st_ctim.tv_nsec = ts_c.tv_nsec;
}

/* ---- FUSE operations ---- */

static void signal_parent(int fd, uint8_t byte)
{
	if (write(fd, &byte, 1) != 1)
		perror("write to signal pipe");
}

/* Report a failure stage and why, in one write. */
static void signal_parent_err(int fd, uint8_t byte, const char *reason)
{
	size_t len = strlen(reason);

	if (len > CHILD_MSG_MAX)
		len = CHILD_MSG_MAX;

	char buf[CHILD_MSG_MAX + 1];
	buf[0] = byte;
	memcpy(buf + 1, reason, len);

	/* Single write: bounded well under a pipe buffer so the child never
	 * blocks writing it, even if the parent is slow to read. */
	if (write(fd, buf, len + 1) != (ssize_t)(len + 1))
		perror("write to signal pipe");
}

static void bcachefs_fuse_init(void *userdata, struct fuse_conn_info *conn)
{
	struct bcachefs_fuse *bf = userdata;
	if (bf->signal_fd != -1) {
		signal_parent(bf->signal_fd, CHILD_OK);
		close(bf->signal_fd);
		bf->signal_fd = -1;
	}
}

static void bcachefs_fuse_destroy(void *userdata)
{
	struct bcachefs_fuse *bf = userdata;
	bch2_fs_exit(bf->c);
}

static void bcachefs_fuse_lookup(fuse_req_t req, fuse_ino_t parent,
				 const char *name)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir = map_root_ino(parent);
	struct bch_inode_unpacked dir_u, bi;
	struct bch_hash_info hash_info;
	struct qstr qname = QSTR(name);
	subvol_inum inum;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, dir, &dir_u);
	if (ret)
		goto err;

	ret = bch2_hash_info_init(bf->c, &dir_u, &hash_info);
	if (ret)
		goto err;

	ret = bch2_dirent_lookup(bf->c, dir, &hash_info, &qname, &inum);
	if (ret) {
		if (ret == -ENOENT) {
			struct fuse_entry_param e = {
				.ino = 0,
				.attr_timeout = TTL,
				.entry_timeout = TTL,
			};
			fuse_reply_entry(req, &e);
		} else {
			fuse_reply_err(req, -ret);
		}
		return;
	}

	ret = bch2_inode_find_by_inum(bf->c, inum, &bi);
	if (ret)
		goto err;

	struct fuse_entry_param e = {
		.ino = unmap_root_ino(bi.bi_inum),
		.generation = bi.bi_generation,
		.attr_timeout = TTL,
		.entry_timeout = TTL,
	};
	inode_to_stat(bf->c, &bi, &e.attr);
	fuse_reply_entry(req, &e);
	return;

err:
	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_getattr(fuse_req_t req, fuse_ino_t ino,
				  struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum = map_root_ino(ino);
	struct bch_inode_unpacked bi;

	int ret = bch2_inode_find_by_inum(bf->c, inum, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct stat st;
	inode_to_stat(bf->c, &bi, &st);
	fuse_reply_attr(req, &st, TTL);
}

static void bcachefs_fuse_setattr(fuse_req_t req, fuse_ino_t ino,
				  struct stat *attr, int to_set,
				  struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum = map_root_ino(ino);
	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
			struct btree_iter iter;
			u64 now = bch2_current_time(bf->c);
			int ret2 = bch2_inode_peek(trans, &iter, &bi, inum,
						   BTREE_ITER_intent);
			if (!ret2) {
				if (to_set & FUSE_SET_ATTR_MODE)
					bi.bi_mode = attr->st_mode;
				if (to_set & FUSE_SET_ATTR_UID)
					bi.bi_uid = attr->st_uid;
				if (to_set & FUSE_SET_ATTR_GID)
					bi.bi_gid = attr->st_gid;
				if (to_set & FUSE_SET_ATTR_SIZE)
					bi.bi_size = attr->st_size;
				if (to_set & FUSE_SET_ATTR_ATIME)
					bi.bi_atime = timespec_to_bch2_time(
						bf->c, attr->st_atim);
				else if (to_set & FUSE_SET_ATTR_ATIME_NOW)
					bi.bi_atime = now;
				if (to_set & FUSE_SET_ATTR_MTIME)
					bi.bi_mtime = timespec_to_bch2_time(
						bf->c, attr->st_mtim);
				else if (to_set & FUSE_SET_ATTR_MTIME_NOW)
					bi.bi_mtime = now;

				ret2 = bch2_inode_write(trans, &iter, &bi);
				bch2_trans_iter_exit(&iter);
			}
			ret2;
		}));

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct stat st;
	inode_to_stat(bf->c, &bi, &st);
	fuse_reply_attr(req, &st, TTL);
}

static void bcachefs_fuse_readlink(fuse_req_t req, fuse_ino_t ino)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum = map_root_ino(ino);
	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, inum, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	size_t size = bi.bi_size;
	size_t block_size = block_bytes(bf->c);
	size_t aligned_size = round_up(size, block_size);

	void *buf = xaligned_alloc(block_size, aligned_size);

	struct bch_read_bio *rbio;
	unsigned nr_vecs = (aligned_size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
	rbio = xcalloc(1, sizeof(*rbio) + sizeof(struct bio_vec) * nr_vecs);
	rbio->c = bf->c;
	bio_init(&rbio->bio, NULL, bio_inline_vecs(&rbio->bio), nr_vecs, 0);
	bch2_bio_map(&rbio->bio, buf, aligned_size);
	struct bvec_iter iter = rbio->bio.bi_iter;

	ret = bch2_trans_do(bf->c,
			    bch2_read(trans, rbio, iter, inum, NULL, NULL, 0));

	if (ret) {
		free(buf);
		free(rbio);
		fuse_reply_err(req, -ret);
		return;
	}

	char *link = buf;
	size_t link_len = size;
	for (size_t i = 0; i < size; i++) {
		if (link[i] == '\0') {
			link_len = i;
			break;
		}
	}
	fuse_reply_buf(req, link, link_len);
	free(buf);
	free(rbio);
}

static void bcachefs_fuse_mknod(fuse_req_t req, fuse_ino_t parent,
				const char *name, mode_t mode, dev_t rdev)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir = map_root_ino(parent);
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);
	int ret;

	bch2_inode_init_early(bf->c, &bi);
	struct bch_subvolume new_subvol;

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_create_trans(trans, dir, &dir_u, &bi, &new_subvol, &qname,
				  0, 0, mode, rdev, NULL, NULL,
				  (subvol_inum){ 0 }, 0));

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct fuse_entry_param e = {
		.ino = unmap_root_ino(bi.bi_inum),
		.generation = bi.bi_generation,
		.attr_timeout = TTL,
		.entry_timeout = TTL,
	};
	inode_to_stat(bf->c, &bi, &e.attr);
	fuse_reply_entry(req, &e);
}

static void bcachefs_fuse_mkdir(fuse_req_t req, fuse_ino_t parent,
				const char *name, mode_t mode)
{
	bcachefs_fuse_mknod(req, parent, name, mode | S_IFDIR, 0);
}

static void bcachefs_fuse_unlink(fuse_req_t req, fuse_ino_t parent,
				 const char *name)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir = map_root_ino(parent);
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);

	int ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
		bch2_unlink_trans(trans, dir, &dir_u, (subvol_inum){}, &bi,
				  &qname, false));

	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_rmdir(fuse_req_t req, fuse_ino_t parent,
				const char *name)
{
	bcachefs_fuse_unlink(req, parent, name);
}

static void bcachefs_fuse_symlink(fuse_req_t req, const char *link,
				  fuse_ino_t parent, const char *name)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir = map_root_ino(parent);
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);
	int ret;

	bch2_inode_init_early(bf->c, &bi);
	struct bch_subvolume new_subvol;

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_create_trans(trans, dir, &dir_u, &bi, &new_subvol,
				  &qname, 0, 0,
				  S_IFLNK | 0777, 0, NULL, NULL,
				  (subvol_inum){ 0 }, 0));
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	size_t link_len = strlen(link) + 1;
	size_t block_size = block_bytes(bf->c);
	size_t aligned_size = round_up(link_len, block_size);

	void *buf = xaligned_alloc(block_size, aligned_size);
	memset(buf, 0, aligned_size);
	memcpy(buf, link, strlen(link));

	struct bch_inode_opts io_opts;
	bch2_inode_opts_get_inode(bf->c, &bi, &io_opts);

	struct bch_write_op op;
	bch2_write_op_init(&op, bf->c, io_opts);
	op.pos = POS(bi.bi_inum, 0);
	op.subvol = dir.subvol;
	op.nr_replicas = max(1U, io_opts.data_replicas);
	op.new_i_size = link_len;
	op.flags |= BCH_WRITE_sync;

	closure_init_stack(&op.cl);

	struct bio *bio = &op.wbio.bio;
	struct bio_vec bv[1];
	bio_init(bio, NULL, bv, 1, 0);
	bch2_bio_map(bio, buf, aligned_size);
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_SYNC);

	if (bch2_disk_reservation_add(bf->c, &op.res, aligned_size >> 9,
				      op.nr_replicas, 0)) {
		free(buf);
		fuse_reply_err(req, ENOSPC);
		return;
	}

	closure_call(&op.cl, (closure_fn *)bch2_write, NULL, NULL);
	closure_sync(&op.cl);

	ret = op.error;
	free(buf);

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	/* Update times */
	subvol_inum bi_inum = { dir.subvol, bi.bi_inum };
	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
			struct btree_iter iter;
			u64 now = bch2_current_time(bf->c);
			int ret2 = bch2_inode_peek(trans, &iter, &bi, bi_inum,
						   BTREE_ITER_intent);
			if (!ret2) {
				bi.bi_mtime = bi.bi_ctime = now;
				ret2 = bch2_inode_write(trans, &iter, &bi);
				bch2_trans_iter_exit(&iter);
			}
			ret2;
		}));
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	bch2_inode_find_by_inum(bf->c, bi_inum, &bi);

	struct fuse_entry_param e = {
		.ino = unmap_root_ino(bi.bi_inum),
		.generation = bi.bi_generation,
		.attr_timeout = TTL,
		.entry_timeout = TTL,
	};
	inode_to_stat(bf->c, &bi, &e.attr);
	fuse_reply_entry(req, &e);
}

static void bcachefs_fuse_rename(fuse_req_t req, fuse_ino_t parent,
				 const char *name, fuse_ino_t newparent,
				 const char *newname, unsigned int flags)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum src_dir = map_root_ino(parent);
	subvol_inum dst_dir = map_root_ino(newparent);
	struct bch_inode_unpacked src_dir_u, dst_dir_u, src_bi, dst_bi;
	struct qstr src_qname = QSTR(name);
	struct qstr dst_qname = QSTR(newname);
	struct inode_opt_change src_opt_change = {}, dst_opt_change = {};

	int ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_rename_trans(trans, src_dir, &src_dir_u, dst_dir,
				  &dst_dir_u, &src_bi, &dst_bi, &src_qname,
				  &dst_qname, BCH_RENAME,
				  &src_opt_change, &dst_opt_change));

	/* The opt changes are finished after the commit: see rename_trans(). */
	if (!ret) {
		CLASS(btree_trans, trans)(bf->c);
		ret = bch2_inode_opt_change_finish(trans, &src_opt_change) ?:
		      bch2_inode_opt_change_finish(trans, &dst_opt_change);
	}

	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_link(fuse_req_t req, fuse_ino_t ino,
			       fuse_ino_t newparent, const char *newname)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum = map_root_ino(ino);
	subvol_inum dir = map_root_ino(newparent);
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(newname);

	int ret = bch2_trans_commit_do(bf->c, NULL, NULL, 0,
				       bch2_link_trans(trans, dir, &dir_u, inum,
						       &bi, &qname));

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct fuse_entry_param e = {
		.ino = unmap_root_ino(bi.bi_inum),
		.generation = bi.bi_generation,
		.attr_timeout = TTL,
		.entry_timeout = TTL,
	};
	inode_to_stat(bf->c, &bi, &e.attr);
	fuse_reply_entry(req, &e);
}

static void bcachefs_fuse_open(fuse_req_t req, fuse_ino_t ino,
			       struct fuse_file_info *fi)
{
	fi->keep_cache = 1;
	fuse_reply_open(req, fi);
}

static void bcachefs_fuse_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			       off_t offset, struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum = map_root_ino(ino);
	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, inum, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	u64 end = min((u64)bi.bi_size, (u64)(offset + size));
	if (end <= (u64)offset) {
		fuse_reply_buf(req, NULL, 0);
		return;
	}
	size_t read_size = end - offset;

	size_t block_size = block_bytes(bf->c);
	off_t aligned_start = offset & ~(block_size - 1);
	size_t pad_start = offset - aligned_start;
	off_t aligned_end = round_up(offset + read_size, block_size);
	size_t aligned_size = aligned_end - aligned_start;

	void *buf = xaligned_alloc(block_size, aligned_size);

	struct bch_read_bio *rbio;
	unsigned nr_vecs = (aligned_size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
	rbio = xcalloc(1, sizeof(*rbio) + sizeof(struct bio_vec) * nr_vecs);
	rbio->c = bf->c;
	bio_init(&rbio->bio, NULL, bio_inline_vecs(&rbio->bio), nr_vecs, 0);
	bch2_bio_map(&rbio->bio, buf, aligned_size);
	struct bvec_iter iter = rbio->bio.bi_iter;
	iter.bi_sector = aligned_start >> 9;

	ret = bch2_trans_do(bf->c,
			    bch2_read(trans, rbio, iter, inum, NULL, NULL, 0));

	if (ret) {
		free(buf);
		free(rbio);
		fuse_reply_err(req, -ret);
		return;
	}

	fuse_reply_buf(req, (const char *)buf + pad_start, read_size);
	free(buf);
	free(rbio);
}

static void bcachefs_fuse_write(fuse_req_t req, fuse_ino_t ino,
				const char *data, size_t size, off_t offset,
				struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum = map_root_ino(ino);
	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, inum, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	size_t block_size = block_bytes(bf->c);
	off_t aligned_start = offset & ~(block_size - 1);
	size_t pad_start = offset - aligned_start;
	off_t aligned_end = round_up(offset + size, block_size);
	size_t aligned_size = aligned_end - aligned_start;

	void *buf = xaligned_alloc(block_size, aligned_size);

	struct bch_inode_opts io_opts;
	bch2_inode_opts_get_inode(bf->c, &bi, &io_opts);

	// RMW
	if (pad_start > 0 || (aligned_size > size + pad_start)) {
		struct bch_read_bio *rbio;
		unsigned nr_vecs =
			(aligned_size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
		rbio = xcalloc(1, sizeof(*rbio) +
					  sizeof(struct bio_vec) * nr_vecs);
		rbio->c = bf->c;
		bio_init(&rbio->bio, NULL, bio_inline_vecs(&rbio->bio), nr_vecs,
			 0);
		bch2_bio_map(&rbio->bio, buf, aligned_size);
		struct bvec_iter iter = rbio->bio.bi_iter;
		iter.bi_sector = aligned_start >> 9;

		ret = bch2_trans_do(bf->c, bch2_read(trans, rbio, iter, inum,
						     NULL, NULL, 0));
		free(rbio);
		if (ret) {
			free(buf);
			fuse_reply_err(req, -ret);
			return;
		}
	}

	memcpy((char *)buf + pad_start, data, size);

	struct bch_write_op op;
	bch2_write_op_init(&op, bf->c, io_opts);
	op.pos = POS(bi.bi_inum, aligned_start >> 9);
	op.subvol = inum.subvol;
	op.nr_replicas = max(1U, io_opts.data_replicas);
	op.new_i_size = max((u64)bi.bi_size, (u64)(offset + size));
	op.flags |= BCH_WRITE_sync;

	closure_init_stack(&op.cl);

	struct bio *wbio = &op.wbio.bio;
	unsigned nr_vecs = (aligned_size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
	struct bio_vec *bv = xcalloc(nr_vecs, sizeof(struct bio_vec));
	bio_init(wbio, NULL, bv, nr_vecs, 0);
	bch2_bio_map(wbio, buf, aligned_size);
	bio_set_op_attrs(wbio, REQ_OP_WRITE, REQ_SYNC);

	if (bch2_disk_reservation_add(bf->c, &op.res, aligned_size >> 9,
				      op.nr_replicas, 0)) {
		free(buf);
		free(bv);
		fuse_reply_err(req, ENOSPC);
		return;
	}

	closure_call(&op.cl, (closure_fn *)bch2_write, NULL, NULL);
	closure_sync(&op.cl);

	ret = op.error;
	free(buf);
	free(bv);

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
			struct btree_iter iter;
			u64 now = bch2_current_time(bf->c);
			int ret2 = bch2_inode_peek(trans, &iter, &bi, inum,
						   BTREE_ITER_intent);
			if (!ret2) {
				bi.bi_mtime = bi.bi_ctime = now;
				ret2 = bch2_inode_write(trans, &iter, &bi);
				bch2_trans_iter_exit(&iter);
			}
			ret2;
		}));
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	fuse_reply_write(req, size);
}

struct readdir_ctx {
	struct dir_context ctx;
	fuse_req_t req;
	char *buf;
	size_t size;
	size_t used;
};

static int readdir_actor(struct dir_context *_ctx, const char *name,
			 int namelen, loff_t pos, u64 ino, unsigned type)
{
	struct readdir_ctx *ctx = container_of(_ctx, struct readdir_ctx, ctx);
	struct stat st = { .st_ino = unmap_root_ino(ino),
			   .st_mode = type << 12 };
	char *name_nul = xstrndup(name, namelen);
	size_t entsize = fuse_add_direntry(ctx->req, ctx->buf + ctx->used,
					   ctx->size - ctx->used, name_nul, &st,
					   pos + 1);
	free(name_nul);

	if (entsize > ctx->size - ctx->used)
		return -1;
	ctx->used += entsize;
	return 0;
}

static void bcachefs_fuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
				  off_t offset, struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir = map_root_ino(ino);
	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, dir, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct bch_hash_info hash_info;
	ret = bch2_hash_info_init(bf->c, &bi, &hash_info);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	char *buf = xcalloc(1, size);
	struct readdir_ctx ctx = { .ctx.actor = readdir_actor,
				   .ctx.pos = offset,
				   .req = req,
				   .buf = buf,
				   .size = size,
				   .used = 0 };

	if (ctx.ctx.pos == 0) {
		struct stat st = { .st_ino = unmap_root_ino(dir.inum),
				   .st_mode = S_IFDIR };
		size_t entsize = fuse_add_direntry(req, buf + ctx.used,
						    size - ctx.used, ".", &st, 1);
		if (entsize > size - ctx.used) {
			fuse_reply_buf(req, buf, ctx.used);
			free(buf);
			return;
		}
		ctx.used += entsize;
		ctx.ctx.pos = 1;
	}
	if (ctx.ctx.pos == 1) {
		struct stat st = { .st_ino = 1, .st_mode = S_IFDIR };
		size_t entsize = fuse_add_direntry(req, buf + ctx.used,
						    size - ctx.used, "..", &st, 2);
		if (entsize > size - ctx.used) {
			fuse_reply_buf(req, buf, ctx.used);
			free(buf);
			return;
		}
		ctx.used += entsize;
		ctx.ctx.pos = 2;
	}

	ret = bch2_readdir(bf->c, dir, &hash_info, &ctx.ctx);
	if (ret && ret != -1) {
		free(buf);
		fuse_reply_err(req, -ret);
		return;
	}

	fuse_reply_buf(req, buf, ctx.used);
	free(buf);
}

static void bcachefs_fuse_statfs(fuse_req_t req, fuse_ino_t ino)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	struct bch_fs_usage_short usage = bch2_fs_usage_read_short(bf->c);
	u32 block_size = block_bytes(bf->c);
	u64 shift = bf->c->block_bits;

	u64 nr_inodes = 0;
	struct disk_accounting_pos k;
	disk_accounting_key_init(k, nr_inodes);
	bch2_accounting_mem_read(bf->c, disk_accounting_pos_to_bpos(&k),
				 &nr_inodes, 1);

	struct statvfs st = {
		.f_bsize = block_size,
		.f_frsize = block_size,
		.f_blocks = usage.capacity >> shift,
		.f_bfree = (usage.capacity - usage.used) >> shift,
		.f_bavail = (usage.capacity - usage.used) >> shift,
		.f_files = nr_inodes,
		.f_ffree = (u64)-1,
		.f_namemax = 255,
	};
	fuse_reply_statfs(req, &st);
}

static void bcachefs_fuse_create(fuse_req_t req, fuse_ino_t parent,
				 const char *name, mode_t mode,
				 struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir = map_root_ino(parent);
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);
	int ret;

	bch2_inode_init_early(bf->c, &bi);
	struct bch_subvolume new_subvol;

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_create_trans(trans, dir, &dir_u, &bi, &new_subvol, &qname,
				  0, 0, mode, 0, NULL, NULL,
				  (subvol_inum){ 0 }, 0));
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct fuse_entry_param e = {
		.ino = unmap_root_ino(bi.bi_inum),
		.generation = bi.bi_generation,
		.attr_timeout = TTL,
		.entry_timeout = TTL,
	};
	inode_to_stat(bf->c, &bi, &e.attr);
	fi->keep_cache = 1;
	fuse_reply_create(req, &e, fi);
}

static const struct fuse_lowlevel_ops bcachefs_fuse_ops = {
	.init = bcachefs_fuse_init,
	.destroy = bcachefs_fuse_destroy,
	.lookup = bcachefs_fuse_lookup,
	.getattr = bcachefs_fuse_getattr,
	.setattr = bcachefs_fuse_setattr,
	.readlink = bcachefs_fuse_readlink,
	.mknod = bcachefs_fuse_mknod,
	.mkdir = bcachefs_fuse_mkdir,
	.unlink = bcachefs_fuse_unlink,
	.rmdir = bcachefs_fuse_rmdir,
	.symlink = bcachefs_fuse_symlink,
	.rename = bcachefs_fuse_rename,
	.link = bcachefs_fuse_link,
	.open = bcachefs_fuse_open,
	.read = bcachefs_fuse_read,
	.write = bcachefs_fuse_write,
	.readdir = bcachefs_fuse_readdir,
	.statfs = bcachefs_fuse_statfs,
	.create = bcachefs_fuse_create,
};

/* ---- Command parsing and main ---- */

/* Parse mount options, splitting into:
 * - fs_opts: filesystem-specific options for bcachefs
 * - fuse_opts: options to pass to FUSE (filters out userspace-only fstab opts)
 */
static void parse_fusemount_options(const char *options,
				    struct printbuf *fs_opts,
				    struct printbuf *fuse_opts)
{
	char *orig = options ? strdup(options) : NULL;
	char *p = orig;
	char *tok;

	while ((tok = strsep(&p, ","))) {
		if (!strcmp(tok, "ro")) {
			/* ro is both a FUSE option and sets bcachefs read_only */
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, "ro");
			if (fs_opts->pos)
				prt_char(fs_opts, ',');
			prt_str(fs_opts, "ro");
		} else if (!strcmp(tok, "rw") || !*tok) {
			/* ignore */
		} else if (!strcmp(tok, "dirsync") ||
			   !strcmp(tok, "noatime") ||
			   !strcmp(tok, "nodev") ||
			   !strcmp(tok, "noexec") ||
			   !strcmp(tok, "nosuid") ||
			   !strcmp(tok, "sync")) {
			/* kernel mount flags with fuser MountOption equivalents */
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "lazytime") ||
			   !strcmp(tok, "mand") ||
			   !strcmp(tok, "nodiratime") ||
			   !strcmp(tok, "relatime") ||
			   !strcmp(tok, "remount") ||
			   !strcmp(tok, "strictatime")) {
			/* kernel mount flags without fuser equivalent — silently ignore */
		} else if (!strcmp(tok, "auto") ||
			   !strcmp(tok, "noauto") ||
			   !strcmp(tok, "nofail") ||
			   !strcmp(tok, "_netdev") ||
			   !strcmp(tok, "user") ||
			   !strcmp(tok, "nouser") ||
			   !strcmp(tok, "users") ||
			   !strcmp(tok, "group") ||
			   !strcmp(tok, "owner") ||
			   !strncmp(tok, "x-", 2) ||
			   !strncmp(tok, "comment=", 8)) {
			/* userspace-only fstab flags — skip for FUSE */
		} else {
			/* filesystem-specific option */
			if (fs_opts->pos)
				prt_char(fs_opts, ',');
			prt_str(fs_opts, tok);
		}
	}
	free(orig);
}

static void fusemount_usage(void)
{
	printf("Usage: bcachefs fusemount [options] <device> <mountpoint>\n"
	       "\n"
	       "Options:\n"
	       "  -o opt[,opt...]    Mount options\n"
	       "  -f                 Run in foreground\n"
	       "  -h, --help         Display this help and exit\n");
}

int cmd_fusemount(int argc, char *argv[])
{
	char *options = NULL;
	bool foreground = false;
	int c;

	while ((c = getopt(argc, argv, "o:fh")) != -1) {
		switch (c) {
		case 'o':
			options = optarg;
			break;
		case 'f':
			foreground = true;
			break;
		case 'h':
			fusemount_usage();
			return 0;
		default:
			fusemount_usage();
			return 1;
		}
	}

	if (argc - optind < 2) {
		fusemount_usage();
		return 1;
	}

	char *device = argv[optind];
	char *mountpoint = argv[optind + 1];

	char *devs_str = bch2_scan_devices(device);
	if (!devs_str) {
		fprintf(stderr, "no devices found for %s\n", device);
		return 1;
	}

	darray_const_str devices = { 0 };
	{
		char *p = devs_str, *s;
		while ((s = strsep(&p, ":")))
			darray_push(&devices, s);
	}

	struct bch_opts bch_opts = bch2_opts_empty();
	opt_set(bch_opts, nostart, 1);

	/* Parse -o options into filesystem-specific and FUSE-specific */
	struct printbuf fs_opts = PRINTBUF;
	struct printbuf fuse_opts = PRINTBUF;
	if (options)
		parse_fusemount_options(options, &fs_opts, &fuse_opts);

	if (fs_opts.pos) {
		struct printbuf err = PRINTBUF;
		if (bch2_parse_mount_opts(NULL, &bch_opts, &err,
					  fs_opts.buf, true))
			die("invalid mount option: %s", err.buf);
		printbuf_exit(&err);
	}
	printbuf_exit(&fs_opts);
	opt_set(bch_opts, nostart, 1);

	struct bch_fs *fs = bch2_fs_open(&devices, &bch_opts, NULL);
	if (IS_ERR(fs)) {
		fprintf(stderr, "Error opening filesystem: %s\n",
			bch2_err_str(PTR_ERR(fs)));
		free(devs_str);
		darray_exit(&devices);
		return 1;
	}
	free(devs_str);
	darray_exit(&devices);

	pthread_key_create(&rcu_key, rcu_thread_destructor);

	struct bcachefs_fuse bf = { .c = fs, .signal_fd = -1 };
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	fuse_opt_add_arg(&args, argv[0]);
	fuse_opt_add_arg(&args, "-o");
	char fsname_opt[1024];
	snprintf(fsname_opt, sizeof(fsname_opt), "fsname=%s,subtype=bcachefs",
		 device);
	fuse_opt_add_arg(&args, fsname_opt);
	if (fuse_opts.pos) {
		fuse_opt_add_arg(&args, "-o");
		fuse_opt_add_arg(&args, fuse_opts.buf);
	}
	printbuf_exit(&fuse_opts);

	if (foreground) {
		linux_shrinkers_init();
		int ret = bch2_fs_start(fs);
		if (ret) {
			bch2_fs_exit(fs);
			fuse_opt_free_args(&args);
			fprintf(stderr, "Error starting filesystem: %s\n",
				bch2_err_str(ret));
			return 1;
		}

		struct fuse_session *se =
			fuse_session_new(&args, &bcachefs_fuse_ops,
					 sizeof(bcachefs_fuse_ops), &bf);
		if (!se) {
			bch2_fs_exit(fs);
			fuse_opt_free_args(&args);
			return 1;
		}
		if (fuse_set_signal_handlers(se) != 0) {
			fuse_session_destroy(se);
			bch2_fs_exit(fs);
			fuse_opt_free_args(&args);
			return 1;
		}
		if (fuse_session_mount(se, mountpoint) != 0) {
			int saved_errno = errno;
			fuse_remove_signal_handlers(se);
			fuse_session_destroy(se);
			bch2_fs_exit(fs);
			fuse_opt_free_args(&args);
			fprintf(stderr, "Error mounting filesystem: %s\n",
				strerror(saved_errno));
			return 1;
		}
		fuse_session_loop(se);
		fuse_session_unmount(se);
		fuse_remove_signal_handlers(se);
		fuse_session_destroy(se);
		fuse_opt_free_args(&args);
	} else {
		int pipe_fds[2];
		if (pipe(pipe_fds) != 0)
			return 1;
		pid_t pid = fork();
		if (pid < 0)
			return 1;
		if (pid > 0) {
			close(pipe_fds[1]);
			uint8_t status = 0xff;
			ssize_t got = read(pipe_fds[0], &status, 1);

			if (got == 1 && status == CHILD_OK) {
				close(pipe_fds[0]);
				exit(0);
			}

			/* A failing child writes stage and reason in a single
			 * write and then exits, so the rest is already queued
			 * and EOF follows. */
			char reason[CHILD_MSG_MAX + 1];
			size_t reason_len = 0;
			if (got == 1) {
				ssize_t n = read(pipe_fds[0], reason,
						 sizeof(reason) - 1);
				if (n > 0)
					reason_len = n;
			}
			reason[reason_len] = '\0';
			close(pipe_fds[0]);

			int wstatus;
			waitpid(pid, &wstatus, 0);

			switch (status) {
			case CHILD_ERR_FS_START:
				if (reason_len)
					fprintf(stderr,
						"error starting filesystem: %s\n",
						reason);
				else
					fprintf(stderr,
						"error starting filesystem\n");
				break;
			case CHILD_ERR_MOUNT:
				if (reason_len)
					fprintf(stderr, "FUSE mount failed: %s\n",
						reason);
				else
					fprintf(stderr, "FUSE mount failed\n");
				break;
			case CHILD_ERR_SETUP:
				if (reason_len)
					fprintf(stderr,
						"filesystem started but the mount was never attempted: %s\n",
						reason);
				else
					fprintf(stderr,
						"filesystem started but the mount was never attempted\n");
				break;
			default:
				fprintf(stderr,
					"child exited without reporting a reason\n");
				break;
			}
			return 1;
		}
		close(pipe_fds[0]);
		setsid();
		/* Daemon mode: do not inherit stderr or write logs under /tmp. */
		int null_fd = open("/dev/null", O_WRONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDERR_FILENO);
			if (null_fd > STDERR_FILENO)
				close(null_fd);
		}

		bf.signal_fd = pipe_fds[1];
		linux_shrinkers_init();
		int ret = bch2_fs_start(fs);
		if (ret) {
			signal_parent_err(pipe_fds[1], CHILD_ERR_FS_START,
					  bch2_err_str(ret));
			bch2_fs_exit(fs);
			close(pipe_fds[1]);
			exit(1);
		}

		struct fuse_session *se =
			fuse_session_new(&args, &bcachefs_fuse_ops,
					 sizeof(bcachefs_fuse_ops), &bf);
		if (!se) {
			signal_parent_err(pipe_fds[1], CHILD_ERR_SETUP,
					  "fuse_session_new failed");
			bch2_fs_exit(fs);
			close(pipe_fds[1]);
			exit(1);
		}
		if (fuse_set_signal_handlers(se) != 0) {
			signal_parent_err(pipe_fds[1], CHILD_ERR_SETUP,
					  "fuse_set_signal_handlers failed");
			fuse_session_destroy(se);
			bch2_fs_exit(fs);
			close(pipe_fds[1]);
			exit(1);
		}
		if (fuse_session_mount(se, mountpoint) != 0) {
			signal_parent_err(pipe_fds[1], CHILD_ERR_MOUNT,
					  strerror(errno));
			fuse_remove_signal_handlers(se);
			fuse_session_destroy(se);
			bch2_fs_exit(fs);
			close(pipe_fds[1]);
			exit(1);
		}
		fuse_session_loop(se);
		fuse_session_unmount(se);
		fuse_remove_signal_handlers(se);
		fuse_session_destroy(se);
	}

	return 0;
}

#endif
