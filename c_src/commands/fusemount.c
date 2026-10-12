#ifdef BCACHEFS_FUSE

#include <fuse_lowlevel.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/file.h>
#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/dcache.h>

#include "libbcachefs.h"
#include "btree/iter.h"
#include "fs/dirent.h"
#include "fs/namei.h"
#include "fs/inode.h"
#include "fs/xattr.h"
#include "alloc/accounting.h"
#include "alloc/buckets.h"
#include "alloc/foreground.h"
#include "data/read.h"
#include "data/write.h"
#include "init/fs.h"
#include "cmds.h"

/* Kernel mount flags the local libc's <sys/mount.h> may not have: */
#ifndef MS_NODIRATIME
#define MS_NODIRATIME	(1 << 11)
#endif
#ifndef MS_STRICTATIME
#define MS_STRICTATIME	(1 << 24)
#endif
#ifndef MS_LAZYTIME
#define MS_LAZYTIME	(1 << 25)
#endif
#ifndef MS_RELATIME
#define MS_RELATIME	(1 << 12)
#endif
#ifndef MS_MANDLOCK
#define MS_MANDLOCK	(1 << 6)
#endif

/* Forward declarations */
/* How the FUSE mount is made: see "Unmount" in the notes at the top. */
enum fuse_mount_kind {
	/* Plain fuse via libfuse, for image files: */
	FUSE_MOUNT_PLAIN,
	/* fuseblk, mounted by us, for block devices: */
	FUSE_MOUNT_FUSEBLK,
};

struct fuse_mount_info {
	enum fuse_mount_kind	kind;
	const char		*source;	/* fuseblk only */
	unsigned long		ms_flags;
};


static void parse_fusemount_options(const char *options,
				    struct printbuf *fs_opts,
				    struct printbuf *fuse_opts,
				    unsigned long *ms_flags,
				    bool *got_nodev, bool *got_nosuid);

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

/* ---- atime policy (Rust AtimeOpts) ---- */

/*
 * fuse marks every inode S_NOATIME when it has no writeback cache (we don't
 * use it), so the kernel never updates atime: it's ours, on the reads and
 * readdirs that reach us, by the rules of the kernel's
 * atime_needs_update(). A read the kernel serves from its page cache never
 * reaches us, so atime can lag behind those.
 */
enum atime_mode { ATIME_NEVER, ATIME_RELATIVE, ATIME_STRICT };

struct atime_opts {
	enum atime_mode	mode;
	bool		nodiratime;
};

struct bcachefs_fuse {
	struct bch_fs *c;
	int signal_fd;
	struct atime_opts atime;
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

/* ---- Node ID management (Rust NodeMap) ---- */

/*
 * A node ID the kernel holds: the inode it stands for, and FUSE's lookup
 * count - one per entry we hand out, dropped by forget.
 */
struct fuse_node {
	subvol_inum		inum;
	u64			node;
	u64			nlookup;
	/* Open files on it: open() and create() to release(). An unlinked
	 * inode is deleted when the last link and the last open are both
	 * gone, as the kernel's iput() does - not when the kernel gets round
	 * to forgetting it. */
	u64			opens;
	/* Deleted while the kernel still holds the node: forget mustn't
	 * evict it again, and the inode number is free to be reused. */
	bool			deleted;
	struct fuse_node	*hash_next;
	struct fuse_node	*inum_next;
};

/*
 * Where node IDs start for inodes that can't be their own inode number.
 * We only serve the root subvolume (map_root_ino()), so an alternate is
 * only needed when an inode number is taken by a still-live node - a
 * deleted-then-recreated inode, whose old node the kernel still holds.
 */
#define ALT_NODE_BASE	(1ULL << 63)

#define NODE_HASH_BITS	10
#define NODE_HASH_SIZE	(1U << NODE_HASH_BITS)

struct node_map {
	struct fuse_node	*by_node[NODE_HASH_SIZE];
	struct fuse_node	*by_inum[NODE_HASH_SIZE];
	u64			next_alt;
};

static struct node_map nodes = { .next_alt = ALT_NODE_BASE };

static unsigned node_hash_node(u64 node)
{
	return (u32)((node * 0x9E3779B97F4A7C15ULL) >> (64 - NODE_HASH_BITS)) &
	       (NODE_HASH_SIZE - 1);
}

static unsigned node_hash_inum(subvol_inum inum)
{
	u64 h = inum.subvol * 0x9E3779B97F4A7C15ULL + inum.inum;

	return node_hash_node(h);
}

static bool is_alt_node(u64 node)
{
	return node >= ALT_NODE_BASE && node < nodes.next_alt;
}

static struct fuse_node *node_by_node(u64 node)
{
	for (struct fuse_node *n = nodes.by_node[node_hash_node(node)]; n;
	     n = n->hash_next)
		if (n->node == node)
			return n;
	return NULL;
}

static struct fuse_node *node_by_inum(subvol_inum inum)
{
	for (struct fuse_node *n = nodes.by_inum[node_hash_inum(inum)]; n;
	     n = n->inum_next)
		if (n->inum.subvol == inum.subvol && n->inum.inum == inum.inum)
			return n;
	return NULL;
}

static void node_insert(struct fuse_node *n)
{
	unsigned h = node_hash_node(n->node);

	n->hash_next = nodes.by_node[h];
	nodes.by_node[h] = n;

	h = node_hash_inum(n->inum);
	n->inum_next = nodes.by_inum[h];
	nodes.by_inum[h] = n;
}

static void node_remove(struct fuse_node *n)
{
	struct fuse_node **pp = &nodes.by_node[node_hash_node(n->node)];

	while (*pp && *pp != n)
		pp = &(*pp)->hash_next;
	if (*pp)
		*pp = n->hash_next;

	pp = &nodes.by_inum[node_hash_inum(n->inum)];
	while (*pp && *pp != n)
		pp = &(*pp)->inum_next;
	if (*pp)
		*pp = n->inum_next;

	free(n);
}

/* @inum's own inode number as its node ID, if it can have it. */
static u64 own_node(subvol_inum inum)
{
	if (inum.subvol != BCACHEFS_ROOT_SUBVOL)
		return 0;

	u64 node = unmap_root_ino(inum.inum);

	/* Still a live node for an inode deleted under it, whose number
	 * this one now has. */
	if (is_alt_node(node) || node_by_node(node))
		return 0;

	return node;
}

static u64 alloc_alt_node(void)
{
	return nodes.next_alt++;
}

/* A reference to @inum's node ID, counted until forget. Taken before the
 * reply goes out, so the forget can't arrive first. */
static u64 node_get(subvol_inum inum)
{
	struct fuse_node *n = node_by_inum(inum);

	if (!n) {
		n = xmalloc(sizeof(*n));
		n->inum = inum;
		n->node = own_node(inum) ?: alloc_alt_node();
		n->nlookup = 0;
		n->opens = 0;
		n->deleted = false;
		node_insert(n);
	}

	n->nlookup++;
	return n->node;
}

/* The inode @node stands for, or -ESTALE. */
static int node_resolve(u64 node, subvol_inum *out)
{
	struct fuse_node *n = node_by_node(node);

	if (n) {
		*out = n->inum;
		return 0;
	}

	if (is_alt_node(node))
		return -ESTALE;

	*out = map_root_ino(node);
	return 0;
}

/* Drop @nlookup references to @node; with the last, return the inode it
 * stood for (to evict), NULL if references remain or the inode is gone. */
static subvol_inum *node_put(u64 node, u64 nlookup)
{
	static subvol_inum evict;

	struct fuse_node *n = node_by_node(node);

	if (!n)
		return NULL;

	n->nlookup = n->nlookup > nlookup ? n->nlookup - nlookup : 0;
	if (n->nlookup)
		return NULL;

	evict = n->inum;
	node_remove(n);
	return &evict;
}

/* An open file on @node, counted until release(). False if its inode has
 * been deleted - unlinked between the open's FUSE_LOOKUP and its
 * FUSE_OPEN. */
static bool node_open(u64 node)
{
	struct fuse_node *n = node_by_node(node);

	if (n && n->deleted)
		return false;

	if (n)
		n->opens++;
	return true;
}

/* An open file on @node closed: its inode, if that was the last. */
static subvol_inum *node_release(u64 node)
{
	static subvol_inum last;

	struct fuse_node *n = node_by_node(node);

	if (!n)
		return NULL;

	n->opens = n->opens > 0 ? n->opens - 1 : 0;
	if (n->opens || n->deleted)
		return NULL;

	last = n->inum;
	return &last;
}

/* Claims the deletion of @inum, which has no links left: false if it's
 * still open, or if a racing unlink or release claimed it first - two
 * bch2_inode_rm()s of one inode, run together, both find it and the second
 * finds it gone, which is an inconsistency. */
static bool node_delete(subvol_inum inum)
{
	struct fuse_node *n = node_by_inum(inum);

	if (!n || n->opens)
		return false;

	n->deleted = true;
	return true;
}

/* Forget everything: the inodes still referenced (for destroy). */
static void node_drain(subvol_inum **out, size_t *nr)
{
	size_t n = 0;

	*out = NULL;

	for (unsigned i = 0; i < NODE_HASH_SIZE; i++) {
		struct fuse_node *it = nodes.by_node[i];

		while (it) {
			struct fuse_node *next = it->hash_next;

			if (!it->deleted) {
				*out = xrealloc(*out, (n + 1) * sizeof(**out));
				(*out)[n++] = it->inum;
			}
			free(it);
			it = next;
		}
		nodes.by_node[i] = NULL;
		nodes.by_inum[i] = NULL;
	}
	*nr = n;
	memset(&nodes, 0, sizeof(nodes));
	nodes.next_alt = ALT_NODE_BASE;
}

/*
 * bch2_evict_inode(): nothing references the inode any more, so if it has
 * no links left, delete it. A subvolume root with no links is the subvolume
 * deletion path's to delete, not ours.
 */
static void inode_evict(struct bcachefs_fuse *bf, subvol_inum inum)
{
	struct bch_inode_unpacked bi;
	int ret = bch2_inode_find_by_inum(bf->c, inum, &bi);

	if (ret) {
		fprintf(stderr,
			"bcachefs fuse: evicting inode %llu: lookup error %d\n",
			(unsigned long long)inum.inum, ret);
		return;
	}

	if (bch2_inode_nlink_get(&bi) || bch2_inode_is_subvolume_root(&bi))
		return;

	ret = bch2_inode_rm(bf->c, inum);
	if (ret)
		fprintf(stderr,
			"bcachefs fuse: deleting unlinked inode %llu: %d\n",
			(unsigned long long)inum.inum, ret);
}

/*
 * The kernel's iput() at the end of an unlink, or of the last close: an
 * inode with no links left and no open files is deleted now, so its space
 * is back before the syscall returns. Leaving it to forget left it to
 * whenever the kernel sent that (generic/015: rm; sync; df). @bi is the
 * inode as the caller has it. Not directories - an rmdir'd one is empty,
 * and we don't count open directory handles.
 */
static void inode_unlinked(struct bcachefs_fuse *bf, subvol_inum inum,
			   struct bch_inode_unpacked *bi)
{
	if (bch2_inode_nlink_get(bi) ||
	    S_ISDIR(bi->bi_mode) ||
	    bch2_inode_is_subvolume_root(bi))
		return;

	if (node_delete(inum))
		inode_evict(bf, inum);
}

/* Whether @name exists in @dir (Rust dirent_exists). */
static int dirent_exists(struct bch_fs *c, subvol_inum dir, struct qstr *name)
{
	struct bch_inode_unpacked dir_u;
	struct bch_hash_info hash_info;
	subvol_inum inum;
	int ret = bch2_inode_find_by_inum(c, dir, &dir_u);

	if (ret)
		return ret;

	ret = bch2_hash_info_init(c, &dir_u, &hash_info);
	if (ret)
		return ret;

	ret = bch2_dirent_lookup(c, dir, &hash_info, name, &inum);

	if (bch2_err_matches(ret, ENOENT))
		return 0;
	return ret ? ret : 1;
}

/*
 * Shrinking a file: zero the rest of the block the new EOF falls in, then
 * drop everything past it. The kernel's own truncate zeroes that tail in
 * the page cache; without it, truncating down and back up reads the old
 * bytes again (xfstests generic/029). Growing needs only the new i_size,
 * which the setattr transaction writes.
 */
static int fuse_truncate(struct bch_fs *c, subvol_inum inum, u64 new_size)
{
	struct bch_inode_unpacked bi;
	int ret = bch2_inode_find_by_inum(c, inum, &bi);
	if (ret)
		return ret;

	if (new_size >= bi.bi_size)
		return 0;

	size_t block_size = block_bytes(c);
	u64 tail = new_size & (block_size - 1);

	if (tail) {
		u64 block_start = new_size - tail;

		void *buf = xaligned_alloc(block_size, block_size);
		memset(buf, 0, block_size);

		unsigned nr_vecs = (block_size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
		struct bch_read_bio *rbio =
			xcalloc(1, sizeof(*rbio) +
				       sizeof(struct bio_vec) * nr_vecs);
		rbio->c = c;
		bio_init(&rbio->bio, NULL, bio_inline_vecs(&rbio->bio),
			 nr_vecs, 0);
		bch2_bio_map(&rbio->bio, buf, block_size);
		struct bvec_iter iter = rbio->bio.bi_iter;
		iter.bi_sector = block_start >> 9;

		ret = bch2_trans_do(c,
			bch2_read(trans, rbio, iter, inum, NULL, NULL,
				  BCH_READ_retry_if_stale|
				  BCH_READ_may_promote|
				  BCH_READ_user_mapped));
		free(rbio);
		if (ret) {
			free(buf);
			return ret;
		}

		bool dirty = false;
		for (u64 i = tail; i < block_size; i++)
			if (((unsigned char *)buf)[i]) {
				dirty = true;
				break;
			}

		if (dirty) {
			memset((char *)buf + tail, 0, block_size - tail);

			struct bch_inode_opts io_opts;
			bch2_inode_opts_get(c, &io_opts, false);

			struct bch_write_op op;
			bch2_write_op_init(&op, c, io_opts);
			op.write_point = writepoint_hashed(0);
			op.pos = SPOS(bi.bi_inum, block_start >> 9, U32_MAX);
			op.subvol = inum.subvol;
			op.nr_replicas = max(1U, io_opts.data_replicas);
			op.new_i_size = new_size;
			op.flags |= BCH_WRITE_sync|BCH_WRITE_only_specified_devs;

			closure_init_stack(&op.cl);

			struct bio *wbio = &op.wbio.bio;
			struct bio_vec *bv = xcalloc(nr_vecs,
						     sizeof(struct bio_vec));
			bio_init(wbio, NULL, bv, nr_vecs, 0);
			bch2_bio_map(wbio, buf, block_size);
			bio_set_op_attrs(wbio, REQ_OP_WRITE, REQ_SYNC);

			ret = bch2_disk_reservation_add(c, &op.res,
							block_size >> 9,
							op.nr_replicas, 0);
			if (ret) {
				free(buf);
				free(bv);
				return -ENOSPC;
			}

			closure_call(&op.cl, (closure_fn *)bch2_write, NULL,
				     NULL);
			closure_sync(&op.cl);
			ret = op.error;
			free(bv);
			if (ret) {
				free(buf);
				return ret;
			}
		}
		free(buf);
	}

	return bch2_truncate(c, inum, new_size, NULL);
}

/*
 * What bch2_fsync() does once the page cache is written back, which here it
 * already is - writes complete before we reply to them. The kernel flushes
 * the journal only as far as the inode's last update; we don't track that,
 * so we flush all of it: more than the minimum, never less.
 */
static int fuse_fsync(struct bch_fs *c)
{
	if (c->opts.journal_flush_disabled)
		return 0;

	return bch2_journal_flush(&c->journal);
}

/* ---- xattrs (user., trusted., security.) ---- */

/*
 * xattr.c's get and list take VFS objects (bch_inode_info, dentry), and for
 * the kernel the VFS does the prefix to type mapping. These do the inode
 * lookup and that mapping, then what xattr.c does (mirrors the rust_fuse_
 * xattr_*() shims, which are only compiled into the Rust binary).
 *
 * Only user., trusted. and security. are served. system.posix_acl_* is
 * stored in bcachefs's own ACL format and bcachefs.* names inode options
 * rather than xattrs - both need conversions this doesn't do.
 */
static const char * const fuse_xattr_prefixes[] = {
	[KEY_TYPE_XATTR_INDEX_USER]	= XATTR_USER_PREFIX,
	[KEY_TYPE_XATTR_INDEX_TRUSTED]	= XATTR_TRUSTED_PREFIX,
	[KEY_TYPE_XATTR_INDEX_SECURITY]	= XATTR_SECURITY_PREFIX,
};

/* Strips the prefix from *name; returns the xattr type, or -EOPNOTSUPP. */
static int fuse_xattr_type(const char **name)
{
	for (unsigned i = 0; i < ARRAY_SIZE(fuse_xattr_prefixes); i++) {
		const char *prefix = fuse_xattr_prefixes[i];

		if (prefix && !strncmp(*name, prefix, strlen(prefix))) {
			*name += strlen(prefix);
			return i;
		}
	}
	return -EOPNOTSUPP;
}

static int __fuse_xattr_get(struct btree_trans *trans, subvol_inum inum,
			    int type, const char *name, void *buf, size_t size)
{
	struct bch_inode_unpacked inode_u;
	try(bch2_inode_find_by_inum_trans(trans, inum, &inode_u));

	struct bch_hash_info hash;
	try(bch2_hash_info_init(trans->c, &inode_u, &hash));

	struct xattr_search_key search = X_SEARCH(type, name, strlen(name));
	CLASS(btree_iter_uninit, iter)(trans);
	struct bkey_s_c k = bkey_try(bch2_hash_lookup(trans, &iter,
					bch2_xattr_hash_desc, &hash, inum,
					&search, 0));

	struct bkey_s_c_xattr xattr = bkey_s_c_to_xattr(k);
	int len = le16_to_cpu(xattr.v->x_val_len);
	if (buf) {
		if (len > size)
			return -ERANGE;
		memcpy(buf, xattr_val(xattr.v), len);
	}
	return len;
}

/* The value's length; with @buf NULL, just the length. */
static int fuse_xattr_get(struct bch_fs *c, subvol_inum inum, const char *name,
			  void *buf, size_t size)
{
	int type = fuse_xattr_type(&name);
	if (type < 0)
		return type;

	CLASS(btree_trans, trans)(c);
	int ret = lockrestart_do(trans, __fuse_xattr_get(trans, inum, type,
							 name, buf, size));

	/* as bch2_xattr_get_handler(): a missing xattr is ENODATA */
	return bch2_err_matches(ret, ENOENT) ? -ENODATA : ret;
}

static int fuse_xattr_emit(char *buf, size_t size, size_t *used,
			   const char *prefix, const char *name,
			   unsigned name_len)
{
	size_t len = strlen(prefix) + name_len + 1;

	if (buf) {
		if (*used + len > size)
			return -ERANGE;
		memcpy(buf + *used, prefix, strlen(prefix));
		memcpy(buf + *used + strlen(prefix), name, name_len);
		buf[*used + len - 1] = '\0';
	}
	*used += len;
	return 0;
}

/*
 * NUL separated full names, as listxattr(2); with @buf NULL, just the
 * length. trusted.* only with @show_trusted - the kernel lists those only
 * to CAP_SYS_ADMIN, and decides that from the caller, which we can't see.
 */
static int fuse_xattr_list(struct bch_fs *c, subvol_inum inum, char *buf,
			   size_t size, bool show_trusted)
{
	CLASS(btree_trans, trans)(c);
	size_t used = 0;

	int ret = for_each_btree_key_in_subvolume_max(trans, iter, BTREE_ID_xattrs,
				POS(inum.inum, 0), POS(inum.inum, U64_MAX),
				inum.subvol, 0, k, ({
		if (k.k->type != KEY_TYPE_xattr)
			continue;

		struct bkey_s_c_xattr x = bkey_s_c_to_xattr(k);
		unsigned type = x.v->x_type;
		const char *prefix = type < ARRAY_SIZE(fuse_xattr_prefixes)
			? fuse_xattr_prefixes[type] : NULL;

		if (!prefix || (type == KEY_TYPE_XATTR_INDEX_TRUSTED &&
				!show_trusted))
			continue;

		fuse_xattr_emit(buf, size, &used, prefix,
				x.v->x_name_and_value, x.v->x_name_len);
	}));

	return ret ?: used;
}

/*
 * Set, or with @value NULL remove - bch2_xattr_set(), which also updates
 * ctime. @flags: XATTR_CREATE, XATTR_REPLACE; removing passes
 * XATTR_REPLACE, as the VFS does, so that a missing xattr is ENODATA.
 */
static int fuse_xattr_set(struct bch_fs *c, subvol_inum inum, const char *name,
			  const void *value, size_t size, int flags)
{
	int type = fuse_xattr_type(&name);
	if (type < 0)
		return type;

	struct bch_inode_unpacked inode_u;
	CLASS(btree_trans, trans)(c);
	return commit_do(trans, NULL, NULL, 0,
		bch2_xattr_set(trans, inum, &inode_u, name, value, size,
			       type, flags));
}

/* relatime unless told otherwise, as for a kernel mount: */
static struct atime_opts atime_from_ms_flags(unsigned long ms_flags)
{
	struct atime_opts o;

	if (ms_flags & (MS_NOATIME | MS_RDONLY))
		o.mode = ATIME_NEVER;
	else if (ms_flags & MS_STRICTATIME)
		o.mode = ATIME_STRICT;
	else
		o.mode = ATIME_RELATIVE;
	o.nodiratime = ms_flags & MS_NODIRATIME;
	return o;
}

/*
 * atime_needs_update() and relatime_need_update(): with relatime, only when
 * atime isn't already after the last change, or is a day old.
 */
static bool atime_needs_update(struct bch_fs *c, const struct atime_opts *o,
			       const struct bch_inode_unpacked *bi, u64 now)
{
	if ((bi->bi_mode & S_IFMT) == S_IFDIR && o->nodiratime)
		return false;

	s64 atime = bi->bi_atime;
	s64 now_s = now;

	switch (o->mode) {
	case ATIME_NEVER:
		return false;
	case ATIME_STRICT:
		return atime != now_s;
	case ATIME_RELATIVE:
		return atime <= (s64)bi->bi_mtime ||
		       atime <= (s64)bi->bi_ctime ||
		       bch2_time_to_timespec(c, now_s).tv_sec -
			       bch2_time_to_timespec(c, atime).tv_sec >=
			       24 * 60 * 60;
	}
	return false;
}

/*
 * touch_atime(), for a read or readdir that reached us. @bi is the inode as
 * the caller already has it, so that the usual case - no update due - costs
 * no transaction; the decision is made again on the current inode inside it.
 * Failure doesn't fail the read, any more than it does in the kernel.
 */
static void fuse_touch_atime(struct bch_fs *c, const struct atime_opts *o,
			     subvol_inum inum, struct bch_inode_unpacked *bi)
{
	if (!atime_needs_update(c, o, bi, bch2_current_time(c)))
		return;

	int ret = bch2_trans_commit_do(c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
			u64 now = bch2_current_time(c);
			struct btree_iter iter;
			struct bch_inode_unpacked inode_u;
			int ret2 = bch2_inode_peek(trans, &iter, &inode_u, inum,
						   BTREE_ITER_intent);
			if (!ret2) {
				if (atime_needs_update(c, o, &inode_u, now)) {
					inode_u.bi_atime = now;
					ret2 = bch2_inode_write(trans, &iter,
								&inode_u);
				}
				bch2_trans_iter_exit(&iter);
			}
			ret2;
		}));

	if (ret)
		fprintf(stderr,
			"fusemount: updating atime of inode %llu: %d\n",
			(unsigned long long)inum.inum, ret);
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

	/* The inodes still referenced: the kernel is going away, nothing
	 * will ever send their forgets (Rust NodeMap::drain). */
	subvol_inum *referenced;
	size_t nr;

	node_drain(&referenced, &nr);
	for (size_t i = 0; i < nr; i++)
		inode_evict(bf, referenced[i]);
	free(referenced);

	bch2_fs_exit(bf->c);
}

static void bcachefs_fuse_lookup(fuse_req_t req, fuse_ino_t parent,
				 const char *name)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum dir;
	struct bch_inode_unpacked dir_u, bi;
	struct bch_hash_info hash_info;
	struct qstr qname = QSTR(name);
	subvol_inum inum;
	int ret;

	ret = node_resolve(parent, &dir);
	if (ret)
		goto err;

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
		/* Counted, until forget: the kernel's reference to this
		 * dentry's inode. */
		.ino = node_get(inum),
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
	subvol_inum inum;
	int resolve_ret = node_resolve(ino, &inum);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
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
	subvol_inum inum;
	int resolve_ret = node_resolve(ino, &inum);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked bi;
	int ret;

	/*
	 * A size change truncates the file, like the kernel's VFS op: asked
	 * before the setattr transaction, which already changes mtime
	 * (Rust fuse_truncate). Growing needs only the new i_size, written
	 * by the transaction below.
	 */
	bool size_changed = false;

	if (to_set & FUSE_SET_ATTR_SIZE) {
		struct bch_inode_unpacked cur;

		ret = bch2_inode_find_by_inum(bf->c, inum, &cur);
		if (ret) {
			fuse_reply_err(req, -ret);
			return;
		}

		size_changed = cur.bi_size != (u64)attr->st_size;

		if (size_changed) {
			ret = fuse_truncate(bf->c, inum, attr->st_size);
			if (ret) {
				fuse_reply_err(req, -ret);
				return;
			}
		}
	}

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
			struct btree_iter iter;
			u64 now = bch2_current_time(bf->c);
			int ret2 = bch2_inode_peek(trans, &iter, &bi, inum,
						   BTREE_ITER_intent);
			if (!ret2) {
				/*
				 * truncate(2) sends only the size and leaves
				 * mtime to us, as it does to a kernel
				 * filesystem; and every attribute change is
				 * a status change - the kernel only sends a
				 * ctime with the writeback cache, so it's
				 * ours to set.
				 */
				int mtime_flag = 0;

				if (to_set & FUSE_SET_ATTR_MTIME)
					mtime_flag = 1;
				else if (to_set & FUSE_SET_ATTR_MTIME_NOW)
					mtime_flag = 2;
				else if (size_changed)
					mtime_flag = 2;

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
				if (mtime_flag == 1)
					bi.bi_mtime = timespec_to_bch2_time(
						bf->c, attr->st_mtim);
				else if (mtime_flag == 2)
					bi.bi_mtime = now;
				bi.bi_ctime = now;

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
	subvol_inum inum;
	int resolve_ret = node_resolve(ino, &inum);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
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
			    bch2_read(trans, rbio, iter, inum, NULL, NULL,
				      BCH_READ_retry_if_stale|
				      BCH_READ_may_promote|
				      BCH_READ_user_mapped));

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
	subvol_inum dir;
	int resolve_ret = node_resolve(parent, &dir);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);
	int ret;

	bch2_inode_init_early(bf->c, &bi);
	struct bch_subvolume new_subvol;
	const struct fuse_ctx *ctx = fuse_req_ctx(req);

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_create_trans(trans, dir, &dir_u, &bi, &new_subvol, &qname,
				  ctx->uid, ctx->gid, mode, rdev, NULL, NULL,
				  (subvol_inum){ 0 }, 0));

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct fuse_entry_param e = {
		.ino = node_get((subvol_inum){ dir.subvol, bi.bi_inum }),
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
	subvol_inum dir;
	int resolve_ret = node_resolve(parent, &dir);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);

	int ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
		bch2_unlink_trans(trans, dir, &dir_u, (subvol_inum){}, &bi,
				  &qname, false));

	/* The kernel's iput() at the end of the unlink: no links and no open
	 * files means the inode goes now, not whenever forget arrives. */
	if (!ret)
		inode_unlinked(bf, (subvol_inum){ dir.subvol, bi.bi_inum },
			       &bi);

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
	subvol_inum dir;
	int resolve_ret = node_resolve(parent, &dir);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);
	int ret;

	bch2_inode_init_early(bf->c, &bi);
	struct bch_subvolume new_subvol;

	const struct fuse_ctx *ctx = fuse_req_ctx(req);

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_create_trans(trans, dir, &dir_u, &bi, &new_subvol,
				  &qname, ctx->uid, ctx->gid,
				  S_IFLNK | 0777, 0, NULL, NULL,
				  (subvol_inum){ 0 }, 0));
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	size_t link_len = strlen(link);
	size_t block_size = block_bytes(bf->c);
	size_t aligned_size = round_up(link_len, block_size);

	void *buf = xaligned_alloc(block_size, aligned_size);
	memset(buf, 0, aligned_size);
	memcpy(buf, link, link_len);

	/*
	 * The target is written without a NUL and i_size is its length, as
	 * the kernel's page_symlink() does: the data ends at i_size, rounded
	 * up to a block - one block more when the target's length is a
	 * multiple of the block size is an extent past the end of the
	 * inode, which fsck deletes. Replicas hardcoded 1 (rust).
	 */
	struct bch_write_op op;
	{
		struct bch_inode_opts io_opts;
		bch2_inode_opts_get(bf->c, &io_opts, false);

		bch2_write_op_init(&op, bf->c, io_opts);
		op.write_point = writepoint_hashed(0);
		op.pos = SPOS(bi.bi_inum, 0, U32_MAX);
		op.subvol = dir.subvol;
		op.nr_replicas = 1;
		op.new_i_size = link_len;
		op.flags |= BCH_WRITE_sync|BCH_WRITE_only_specified_devs;
	}

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

	subvol_inum bi_inum = { dir.subvol, bi.bi_inum };

	/* Re-read the inode to get updated state (rust does the same; the
	 * create transaction already set the times) */
	bch2_inode_find_by_inum(bf->c, bi_inum, &bi);

	struct fuse_entry_param e = {
		.ino = node_get(bi_inum),
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
	subvol_inum src_dir;
	subvol_inum dst_dir;
	int resolve_ret;

	resolve_ret = node_resolve(parent, &src_dir);
	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	resolve_ret = node_resolve(newparent, &dst_dir);
	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked src_dir_u, dst_dir_u, src_bi, dst_bi;
	struct qstr src_qname = QSTR(name);
	struct qstr dst_qname = QSTR(newname);
	struct inode_opt_change src_opt_change = {}, dst_opt_change = {};

	if (flags & RENAME_WHITEOUT) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	/*
	 * The mode, chosen as bch2_rename2() chooses it. BCH_RENAME means
	 * "there is no target" and isn't checked: renaming onto an existing
	 * name with it inserts a second dirent of that name. The kernel
	 * holds both directories locked for the whole request, so the
	 * target can't appear or vanish between this lookup and the rename.
	 */
	int dst_exists = dirent_exists(bf->c, dst_dir, &dst_qname);
	if (dst_exists < 0) {
		fuse_reply_err(req, -dst_exists);
		return;
	}

	enum bch_rename_mode mode;

	if (flags & RENAME_EXCHANGE) {
		if (!dst_exists) {
			fuse_reply_err(req, ENOENT);
			return;
		}
		mode = BCH_RENAME_EXCHANGE;
	} else if (dst_exists) {
		if (flags & RENAME_NOREPLACE) {
			fuse_reply_err(req, EEXIST);
			return;
		}
		mode = BCH_RENAME_OVERWRITE;
	} else {
		mode = BCH_RENAME;
	}

	int ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_rename_trans(trans, src_dir, &src_dir_u, dst_dir,
				  &dst_dir_u, &src_bi, &dst_bi, &src_qname,
				  &dst_qname, mode,
				  &src_opt_change, &dst_opt_change));

	/* The opt changes are finished after the commit: see rename_trans(). */
	if (!ret) {
		CLASS(btree_trans, trans)(bf->c);
		ret = bch2_inode_opt_change_finish(trans, &src_opt_change) ?:
		      bch2_inode_opt_change_finish(trans, &dst_opt_change);
	}

	if (!ret && mode == BCH_RENAME_OVERWRITE)
		inode_unlinked(bf, (subvol_inum){ dst_dir.subvol, dst_bi.bi_inum },
			       &dst_bi);

	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_link(fuse_req_t req, fuse_ino_t ino,
			       fuse_ino_t newparent, const char *newname)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum;
	int resolve_ret = node_resolve(ino, &inum);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	subvol_inum dir;
	resolve_ret = node_resolve(newparent, &dir);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
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
		.ino = node_get((subvol_inum){ dir.subvol, bi.bi_inum }),
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
	struct bcachefs_fuse *bf = fuse_req_userdata(req);

	/*
	 * A read the kernel serves from its page cache never reaches us to
	 * update atime (see fuse_touch_atime()): a strictatime mount asks
	 * for every access, so there let the kernel drop the cache at open
	 * - at least each open's reads do.
	 */
	fi->keep_cache = bf->atime.mode != ATIME_STRICT;

	/*
	 * Counted, so an unlink doesn't delete the inode under the open
	 * file: the last close of an unlinked inode is when it goes.
	 */
	if (!node_open(ino)) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	fuse_reply_open(req, fi);
}

static void bcachefs_fuse_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			       off_t offset, struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum;
	int resolve_ret = node_resolve(ino, &inum);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, inum, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	fuse_touch_atime(bf->c, &bf->atime, inum, &bi);

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
			    bch2_read(trans, rbio, iter, inum, NULL, NULL,
				      BCH_READ_retry_if_stale|
				      BCH_READ_may_promote|
				      BCH_READ_user_mapped));

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
	subvol_inum inum;
	int resolve_ret = node_resolve(ino, &inum);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
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

	/* Global fs opts, not per-inode: rust_write_submit() - the path all
	 * fuse writes go through - resolves them from the filesystem, and
	 * per-inode overrides (compression etc) are not applied to fuse
	 * writes */
	struct bch_inode_opts io_opts;
	bch2_inode_opts_get(bf->c, &io_opts, false);

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
	op.write_point = writepoint_hashed(0);
	op.pos = SPOS(bi.bi_inum, aligned_start >> 9, U32_MAX);
	op.subvol = inum.subvol;
	op.nr_replicas = max(1U, io_opts.data_replicas);
	op.new_i_size = max((u64)bi.bi_size, (u64)(offset + size));
	op.flags |= BCH_WRITE_sync|BCH_WRITE_only_specified_devs;

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
	subvol_inum dir;
	int resolve_ret = node_resolve(ino, &dir);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}

	struct bch_inode_unpacked bi;
	int ret;

	ret = bch2_inode_find_by_inum(bf->c, dir, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	fuse_touch_atime(bf->c, &bf->atime, dir, &bi);

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

	/* usage is in 512 byte sectors - as bch2_statfs() does: */
	u64 shift = bf->c->block_bits - 9;

	u64 nr_inodes = 0;
	struct disk_accounting_pos k;
	disk_accounting_key_init(k, nr_inodes);
	bch2_accounting_mem_read(bf->c, disk_accounting_pos_to_bpos(&k),
				 &nr_inodes, 1);

	/* As bch2_statfs(): inodes have no fixed limit, so count the free
	 * space as room for ~256 byte inodes. (u64 -1 here reads as
	 * "unknown", and df -i showed "-" - which xfstests generic/273 then
	 * fed to arithmetic.) */
	u64 avail_inodes = (usage.capacity - usage.used) << 1;

	struct statvfs st = {
		.f_bsize = block_size,
		.f_frsize = block_size,
		.f_blocks = usage.capacity >> shift,
		.f_bfree = usage.free >> shift,
		.f_bavail = (usage.capacity - usage.used) >> shift,
		.f_files = nr_inodes + avail_inodes,
		.f_ffree = avail_inodes,
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
	subvol_inum dir;
	int resolve_ret = node_resolve(parent, &dir);

	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}
	struct bch_inode_unpacked dir_u, bi;
	struct qstr qname = QSTR(name);
	int ret;

	bch2_inode_init_early(bf->c, &bi);
	struct bch_subvolume new_subvol;
	const struct fuse_ctx *ctx = fuse_req_ctx(req);

	ret = bch2_trans_commit_do(
		bf->c, NULL, NULL, 0,
		bch2_create_trans(trans, dir, &dir_u, &bi, &new_subvol, &qname,
				  ctx->uid, ctx->gid, mode, 0, NULL, NULL,
				  (subvol_inum){ 0 }, 0));
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	struct fuse_entry_param e = {
		.ino = node_get((subvol_inum){ dir.subvol, bi.bi_inum }),
		.generation = bi.bi_generation,
		.attr_timeout = TTL,
		.entry_timeout = TTL,
	};
	inode_to_stat(bf->c, &bi, &e.attr);
	fi->keep_cache = 1;

	/* Like open(): counted, so an unlink doesn't delete it under the
	 * open file. */
	if (!node_open(e.ino)) {
		fuse_reply_err(req, ENOENT);
		return;
	}

	fuse_reply_create(req, &e, fi);
}

static void bcachefs_fuse_forget(fuse_req_t req, fuse_ino_t ino,
				 unsigned long nlookup)
{
	ensure_thread_init();

	/* Drop @nlookup references; with the last, the inode is unreferenced
	 * and - no links left - deleted (Rust node_put -> inode_evict). */
	subvol_inum *evicted = node_put(ino, nlookup);

	if (evicted)
		inode_evict(fuse_req_userdata(req), *evicted);
}

static void bcachefs_fuse_release(fuse_req_t req, fuse_ino_t ino,
				  struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);

	/* The last open of an inode with no links left: it goes now. */
	subvol_inum *last = node_release(ino);

	if (last) {
		struct bch_inode_unpacked bi;

		if (!bch2_inode_find_by_inum(bf->c, *last, &bi))
			inode_unlinked(bf, *last, &bi);
	}

	fuse_reply_err(req, 0);
}

/*
 * Unimplemented, these would answer ENOSYS - which the kernel takes to mean
 * the filesystem needs no fsync: it stops asking, and every fsync after that
 * returns success with nothing made durable (xfstests generic/034, files
 * missing after a simulated power failure).
 */
static void bcachefs_fuse_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
				struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);

	int ret = fuse_fsync(bf->c);

	fuse_reply_err(req, ret ? -ret : 0);
}

/* sync(2), syncfs(2): bch2_sync_fs(). Answering ENOSYS here would make the
 * kernel stop asking for the rest of the mount. */
static void bcachefs_fuse_fsyncdir(fuse_req_t req, fuse_ino_t ino,
				   int datasync, struct fuse_file_info *fi)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);

	int ret = fuse_fsync(bf->c);

	fuse_reply_err(req, ret ? -ret : 0);
}

static void bcachefs_fuse_syncfs(fuse_req_t req, fuse_ino_t ino)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);

	int ret = fuse_fsync(bf->c);

	fuse_reply_err(req, ret ? -ret : 0);
}

static void bcachefs_fuse_getxattr(fuse_req_t req, fuse_ino_t ino,
				   const char *name, size_t size)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum;

	int resolve_ret = node_resolve(ino, &inum);
	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}

	char *buf = size ? xmalloc(size) : NULL;
	int ret = fuse_xattr_get(bf->c, inum, name, buf, size);

	if (ret >= 0) {
		if (size == 0)
			fuse_reply_xattr(req, ret);
		else
			fuse_reply_buf(req, buf, ret);
	} else {
		fuse_reply_err(req, -ret);
	}
	free(buf);
}

static void bcachefs_fuse_listxattr(fuse_req_t req, fuse_ino_t ino,
				    size_t size)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum;

	int resolve_ret = node_resolve(ino, &inum);
	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}

	/* The kernel lists trusted.* only to CAP_SYS_ADMIN, and doesn't
	 * filter the list for FUSE; we only see the caller's uid. */
	bool show_trusted = fuse_req_uid(req) == 0;

	char *buf = size ? xmalloc(size) : NULL;
	int ret = fuse_xattr_list(bf->c, inum, buf, size, show_trusted);

	if (ret >= 0) {
		if (size == 0)
			fuse_reply_xattr(req, ret);
		else
			fuse_reply_buf(req, buf, ret);
	} else {
		fuse_reply_err(req, -ret);
	}
	free(buf);
}

static void bcachefs_fuse_setxattr(fuse_req_t req, fuse_ino_t ino,
				   const char *name, const char *value,
				   size_t size, int flags)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum;

	int resolve_ret = node_resolve(ino, &inum);
	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}

	int ret = fuse_xattr_set(bf->c, inum, name, value, size, flags);
	fuse_reply_err(req, ret ? -ret : 0);
}

static void bcachefs_fuse_removexattr(fuse_req_t req, fuse_ino_t ino,
				      const char *name)
{
	ensure_thread_init();
	struct bcachefs_fuse *bf = fuse_req_userdata(req);
	subvol_inum inum;

	int resolve_ret = node_resolve(ino, &inum);
	if (resolve_ret) {
		fuse_reply_err(req, -resolve_ret);
		return;
	}

	/* As the VFS does: XATTR_REPLACE, so that a missing xattr is
	 * ENODATA. */
	int ret = fuse_xattr_set(bf->c, inum, name, NULL, 0, XATTR_REPLACE);

	fuse_reply_err(req, ret ? -ret : 0);
}

static const struct fuse_lowlevel_ops bcachefs_fuse_ops = {
	.init = bcachefs_fuse_init,
	.destroy = bcachefs_fuse_destroy,
	.lookup = bcachefs_fuse_lookup,
	.forget = bcachefs_fuse_forget,
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
	.release = bcachefs_fuse_release,
	.fsync = bcachefs_fuse_fsync,
	.fsyncdir = bcachefs_fuse_fsyncdir,
	.statfs = bcachefs_fuse_statfs,
	.readdir = bcachefs_fuse_readdir,
	.create = bcachefs_fuse_create,
	.getxattr = bcachefs_fuse_getxattr,
	.listxattr = bcachefs_fuse_listxattr,
	.setxattr = bcachefs_fuse_setxattr,
	.removexattr = bcachefs_fuse_removexattr,
};

/* ---- Command parsing and main ---- */

/* Parse mount options, splitting into:
 * - fs_opts: filesystem-specific options for bcachefs
 * - fuse_opts: options to pass to FUSE (filters out userspace-only fstab opts)
 */
/* ---- Device locking, fuseblk claim, systemd scope (Rust fusemount.rs) ---- */

/* Flock'd device handles, held for the life of the process: */
static DARRAY(int) held_fds;

/* Whether @dev is the source of a mounted FUSE filesystem: the same block
 * device, or the same image file. */
static bool fuse_mounted(const char *dev)
{
	struct stat want;

	if (stat(dev, &want))
		return false;

	FILE *f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return false;

	char *line = NULL;
	size_t n = 0;
	bool found = false;

	/* ... - fstype source superopts */
	while (!found && getline(&line, &n, f) >= 0) {
		char *sep = strstr(line, " - ");
		if (!sep)
			continue;

		char *fstype = strtok(sep + 3, " \t\n");
		char *source = strtok(NULL, " \t\n");
		if (!fstype || !source || strncmp(fstype, "fuse", 4))
			continue;

		struct stat m;

		if (stat(source, &m))
			continue;

		if (S_ISBLK(want.st_mode) && S_ISBLK(m.st_mode))
			found = want.st_rdev == m.st_rdev;
		else
			found = want.st_dev == m.st_dev &&
				want.st_ino == m.st_ino;
	}

	free(line);
	fclose(f);
	return found;
}

/*
 * Lock every device - block device or image file - against another
 * fusemount, for as long as this process lives. Taken before the filesystem
 * is opened.
 *
 * This is horrible, and it's the best there is. The kernel's claim on a
 * fuseblk mount's source is a real claim against kernel bcachefs mounts and
 * fsck/format (bd_may_claim()) - which is why fuse_claim() mounts before the
 * filesystem is started. But against another fuseblk mount it's nothing:
 * get_tree_bdev() finds the superblock already on that device and hands it
 * to the second mount (sget), so a second fusemount *succeeds* - and then
 * starts a second read-write filesystem on a device the first daemon was
 * serving. Nor can we just open O_EXCL ourselves: our claim and the
 * kernel's have different holders, so whichever came second would fail - we
 * couldn't mount our own device.
 *
 * So: flock(). Advisory - it binds only fusemount - and it locks an inode,
 * so it's per device node; device scanning resolves to the /dev nodes, so in
 * practice every fusemount locks the same one.
 *
 * A held lock whose device isn't mounted is a daemon shutting down - unmount
 * takes the mount away first, then waits for DESTROY - so wait for it, as a
 * kernel mount waits for a dying superblock (generic/604 races exactly
 * that).
 */
static void lock_devices(darray_const_str *devs)
{
	const u64 wait_max = 300;

	darray_for_each(*devs, i)
	{
		int fd = open(*i, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			die("%s: %m", *i);

		struct timespec start;
		clock_gettime(CLOCK_MONOTONIC, &start);
		bool said = false;

		while (flock(fd, LOCK_EX | LOCK_NB)) {
			u64 elapsed;

			if (errno != EWOULDBLOCK)
				die("%s: locking: %m", *i);

			if (fuse_mounted(*i))
				die("%s: already in use by another FUSE mount of bcachefs",
				    *i);

			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed = now.tv_sec - start.tv_sec;

			if (elapsed >= wait_max)
				die("%s: a FUSE mount of bcachefs that's no longer mounted still holds it after %llus - is its daemon stuck?",
				    *i, wait_max);

			if (!said && elapsed >= 1) {
				fprintf(stderr,
					"%s: waiting for the previous FUSE mount of it to finish shutting down\n",
					*i);
				said = true;
			}
			usleep(20 * 1000);
		}

		/* Held until exit, with the claims below. Across the fork,
		 * the child's copies keep them. */
		darray_push(&held_fds, fd);
	}
}

/*
 * Claim the devices the kernel doesn't (everything but the fuseblk source)
 * exclusively, for the life of the mount: we opened them noexcl so the
 * kernel's claim on the source doesn't collide with ours.
 */
static void claim_devices(darray_const_str *devs, size_t skip)
{
	for (size_t i = skip; i < devs->nr; i++) {
		struct stat st;

		if (stat(devs->data[i], &st) || !S_ISBLK(st.st_mode))
			continue;

		int fd = open(devs->data[i], O_RDONLY | O_EXCL);
		if (fd < 0)
			die("%s: device in use: %m", devs->data[i]);

		darray_push(&held_fds, fd);
	}
}

/*
 * Mount fuseblk on @mountpoint and return the /dev/fuse fd to serve it on
 * (>= 0), or a negative errno.
 *
 * The option string is what libfuse builds for plain fuse, plus blksize, so
 * the two kinds of mount behave the same apart from unmount. That includes
 * allow_other with default_permissions, and nodev/nosuid only when asked
 * for - see parse_fusemount_options().
 */
static int mount_fuseblk(const char *source, const char *mountpoint,
			 unsigned long ms_flags, unsigned blksize)
{
	int dev_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (dev_fd < 0)
		return -errno;

	struct stat st;
	if (stat(mountpoint, &st)) {
		int saved = errno;
		close(dev_fd);
		return -saved;
	}

	char data[512];
	snprintf(data, sizeof(data),
		 "fd=%i,rootmode=%o,user_id=%u,group_id=%u,blksize=%u,"
		 "subtype=bcachefs,allow_other,default_permissions",
		 dev_fd, st.st_mode & S_IFMT, getuid(), getgid(), blksize);

	if (mount(source, mountpoint, "fuseblk", ms_flags, data)) {
		int saved = errno;
		close(dev_fd);
		return -saved;
	}

	return dev_fd;
}

/*
 * For fuseblk, make the mount - before the filesystem is started.
 *
 * The kernel's exclusive claim on the source device, taken by this mount, is
 * what keeps a kernel bcachefs mount, or fsck/format, off a device we're
 * serving - and keeps us off one they have: whichever comes second gets
 * EBUSY. So it has to be in place before anything writes: started first, we
 * could go read-write on a device the kernel has mounted, and only then fail
 * here. It does nothing against another fusemount (see lock_devices()).
 *
 * Plain fuse has no such claim and mounts in fuse_serve(), via libfuse.
 *
 * Returns 0 on success, a negative errno on failure; @fuse_fd is set to the
 * /dev/fuse fd to serve on, or -1 for a plain fuse mount.
 */
static int fuse_claim(struct bch_fs *c, const char *mountpoint,
		      const struct fuse_mount_info *mount, int *fuse_fd)
{
	*fuse_fd = -1;

	if (mount->kind != FUSE_MOUNT_FUSEBLK)
		return 0;

	/* fuseblk wants a power of two from 512 to the page size: */
	unsigned blksize = block_bytes(c);
	long page = sysconf(_SC_PAGESIZE);

	if (blksize < 512)
		blksize = 512;
	if (page > 0 && blksize > (unsigned)page)
		blksize = (unsigned)page;

	*fuse_fd = mount_fuseblk(mount->source, mountpoint, mount->ms_flags,
				 blksize);
	return *fuse_fd < 0 ? *fuse_fd : 0;
}

/* Detach a fuseblk mount nothing is going to serve. */
static void fuse_unclaim(const char *mountpoint)
{
	umount2(mountpoint, MNT_DETACH);
}

/*
 * Serve until unmounted: on @fuse_fd from fuse_claim() (fuseblk), or, for
 * plain fuse, on a mount made here. Requests run on multiple worker threads,
 * each on its own cloned /dev/fuse fd - safe because the kernel serialises
 * what isn't: it holds the inode lock across a buffered write and across
 * setattr, so write()'s partial-block read-modify-write and fuse_truncate()
 * never race on one inode.
 */
static int fuse_serve(struct fuse_session *se, const char *mountpoint,
		      const struct fuse_mount_info *mount, int fuse_fd)
{
	if (fuse_fd < 0) {
		/* plain fuse: libfuse makes the mount */
		if (fuse_session_mount(se, mountpoint)) {
			int saved = errno;
			return saved ? -saved : -EIO;
		}
	} else {
		/*
		 * fuseblk: mounted by fuse_claim() before the filesystem was
		 * started. Hand libfuse the /dev/fuse fd through the
		 * /dev/fd/N mountpoint convention - it checks the fd is open
		 * and serves on it, without mounting. se->mountpoint stays
		 * NULL, so fuse_session_unmount() is a no-op and the
		 * detach below is ours to do.
		 */
		char devfd[32];

		snprintf(devfd, sizeof(devfd), "/dev/fd/%d", fuse_fd);
		if (fuse_session_mount(se, devfd)) {
			fuse_unclaim(mountpoint);
			int saved = errno;
			return saved ? -saved : -EIO;
		}
	}

	int res;

#if FUSE_VERSION >= 311
	/*
	 * libfuse >= 3.11: the loop config is opaque; thread count like
	 * Rust's (available_parallelism, min 16), and a separate /dev/fuse
	 * fd per worker.
	 */
	{
		struct fuse_loop_config *cfg = fuse_loop_config_create();

		if (cfg) {
			long n = sysconf(_SC_NPROCESSORS_ONLN);
			unsigned nthreads = n > 0 ? (unsigned)n : 4;

			if (nthreads < 1)
				nthreads = 1;
			if (nthreads > 16)
				nthreads = 16;

			fuse_loop_cfg_set_clone_fd(cfg, 1);
			fuse_loop_cfg_set_max_threads(cfg, nthreads);

			res = fuse_session_loop_mt(se, cfg);
			fuse_loop_config_destroy(cfg);
		} else {
			res = fuse_session_loop_mt(se, NULL);
		}
	}
#elif FUSE_VERSION == 310
	/* 3.10: public struct, no thread count control */
	{
		struct fuse_loop_config cfg = {
			.clone_fd = 1,
			.max_idle_threads = 10,
		};

		res = fuse_session_loop_mt(se, &cfg);
	}
#else
	/* 3.2 - 3.9: clone_fd as a plain argument */
	res = fuse_session_loop_mt(se, 1);
#endif

	fuse_session_unmount(se);

	/*
	 * For fuseblk the mount was made by us, and fuse_session_unmount()
	 * knows nothing of it: detach it when it's still up (a signal exit
	 * leaves it), a no-op after a real unmount. Plain fuse was just
	 * unmounted above.
	 */
	if (fuse_fd >= 0)
		fuse_unclaim(mountpoint);

	return res;
}

/*
 * Move the daemon into a transient systemd scope of its own.
 *
 * Why: the daemon has to live as long as the mount, but it starts life in
 * the cgroup of whoever ran mount(8), and systemd kills every process in a
 * unit when that unit stops - a login session ending, a service that mounted
 * something restarting, or xfstests, which runs each test in a scope and
 * stops it afterwards. The daemon dies, and what's left is worse than an
 * unmount: the mount stays, every access returns ENOTCONN, and the kernel
 * still holds the device, so fsck and a fresh mount get EBUSY until someone
 * unmounts the corpse.
 *
 * Without systemd there is no scope to leave. Failing isn't fatal to the
 * mount - the daemon then lives and dies with its caller's scope, as before
 * - so it's a warning.
 */
static void move_to_own_scope(pid_t pid, const char *device,
			      const char *mountpoint)
{
	struct stat st;

	if (stat("/run/systemd/system", &st))
		return;

	/*
	 * StartTransientUnit through sd-bus, dlopen()ed: libsystemd is there
	 * wherever systemd is, and this is the only thing we'd link it for.
	 */
	void *lib = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr,
			"warning: couldn't give the fuse daemon a systemd scope of its own (libsystemd.so.0 not found); it will exit with whatever scope ran the mount\n");
		return;
	}

	/* struct sd_bus_error's layout, spelled out - we don't include the
	 * systemd headers: */
	struct sd_bus_error_ {
		const char *name;
		const char *message;
		int _need_free;
	};

	typedef int (*bus_call_fn)(void *, const char *, const char *,
				   const char *, const char *, void *,
				   void **, const char *, ...);

	int (*bus_new)(void **) = dlsym(lib, "sd_bus_new");
	int (*bus_open_system)(void **) = dlsym(lib, "sd_bus_open_system");
	int (*bus_set_address)(void *, const char *) =
		dlsym(lib, "sd_bus_set_address");
	int (*bus_set_client)(void *, int) =
		dlsym(lib, "sd_bus_set_bus_client");
	int (*bus_start)(void *) = dlsym(lib, "sd_bus_start");
	void *(*bus_unref)(void *) = dlsym(lib, "sd_bus_flush_close_unref");
	void (*bus_error_free)(void *) = dlsym(lib, "sd_bus_error_free");
	int (*bus_call_method)(void *, const char *, const char *,
			       const char *, const char *, void *, void **,
			       const char *, ...) =
		dlsym(lib, "sd_bus_call_method");

	if (!bus_new || !bus_open_system || !bus_set_address ||
	    !bus_set_client || !bus_start || !bus_unref || !bus_error_free ||
	    !bus_call_method) {
		fprintf(stderr,
			"warning: couldn't give the fuse daemon a systemd scope of its own (libsystemd is missing symbols); it will exit with whatever scope ran the mount\n");
		dlclose(lib);
		return;
	}

	void *bus = NULL;
	int ret;

	/* As root, talk to systemd directly, as systemctl and systemd-run
	 * do: there may be no D-Bus daemon (minimal systems, test VMs). */
	if (!access("/run/systemd/private", F_OK)) {
		ret = bus_new(&bus);
		if (ret >= 0) {
			ret = bus_set_address(bus,
					      "unix:path=/run/systemd/private");
			if (ret >= 0)
				ret = bus_set_client(bus, 0);
			if (ret >= 0)
				ret = bus_start(bus);
		}
	} else {
		ret = bus_open_system(&bus);
	}

	if (ret < 0) {
		if (bus)
			bus_unref(bus);
		fprintf(stderr,
			"warning: couldn't give the fuse daemon a systemd scope of its own (connecting to systemd: %s); it will exit with whatever scope ran the mount\n",
			strerror(-ret));
		dlclose(lib);
		return;
	}

	char unit[64];
	char description[512];

	snprintf(unit, sizeof(unit), "bcachefs-fuse-%i.scope", (int)pid);
	snprintf(description, sizeof(description),
		 "bcachefs fuse daemon: %s on %s", device, mountpoint);

	/* Array lengths are ints to sd_bus_message_append(), as in
	 * systemd-run: */
	struct sd_bus_error_ error = { NULL, NULL, 0 };

	ret = bus_call_method(bus,
			      "org.freedesktop.systemd1",
			      "/org/freedesktop/systemd1",
			      "org.freedesktop.systemd1.Manager",
			      "StartTransientUnit",
			      &error, NULL,
			      "ssa(sv)a(sa(sv))",
			      unit, "fail",
			      3,
			      "PIDs", "au", 1, (unsigned)pid,
			      "Description", "s", description,
			      "CollectMode", "s", "inactive-or-failed",
			      0);

	if (ret < 0) {
		if (error.message)
			fprintf(stderr,
				"warning: couldn't give the fuse daemon a systemd scope of its own (%s); it will exit with whatever scope ran the mount\n",
				error.message);
		else
			fprintf(stderr,
				"warning: couldn't give the fuse daemon a systemd scope of its own (StartTransientUnit: %s); it will exit with whatever scope ran the mount\n",
				strerror(-ret));
	}

	bus_error_free(&error);
	bus_unref(bus);
	dlclose(lib);
}

static void parse_fusemount_options(const char *options,
				    struct printbuf *fs_opts,
				    struct printbuf *fuse_opts,
				    unsigned long *ms_flags,
				    bool *got_nodev, bool *got_nosuid)
{
	char *orig = options ? strdup(options) : NULL;
	char *p = orig;
	char *tok;

	while ((tok = strsep(&p, ","))) {
		if (!strcmp(tok, "ro")) {
			/* ro is both a FUSE option and sets bcachefs read_only.
			 * The bcachefs option is "read_only" - handing it
			 * "ro" got it silently dropped by the
			 * ignore-unknown parse, so the filesystem was
			 * actually opened read-write and -o ro only looked
			 * read-only (Rust opt_sets read_only from MS_RDONLY). */
			*ms_flags |= MS_RDONLY;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, "ro");
			if (fs_opts->pos)
				prt_char(fs_opts, ',');
			prt_str(fs_opts, "read_only=1");
		} else if (!strcmp(tok, "rw") || !*tok) {
			/* ignore */
		} else if (!strcmp(tok, "dirsync")) {
			*ms_flags |= MS_DIRSYNC;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "noatime")) {
			*ms_flags |= MS_NOATIME;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "nodev")) {
			*ms_flags |= MS_NODEV;
			*got_nodev = true;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "noexec")) {
			*ms_flags |= MS_NOEXEC;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "nosuid")) {
			*ms_flags |= MS_NOSUID;
			*got_nosuid = true;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "sync")) {
			*ms_flags |= MS_SYNCHRONOUS;
			if (fuse_opts->pos)
				prt_char(fuse_opts, ',');
			prt_str(fuse_opts, tok);
		} else if (!strcmp(tok, "lazytime")) {
			*ms_flags |= MS_LAZYTIME;
			/* kernel mount flag without a fuser equivalent */
		} else if (!strcmp(tok, "mand")) {
			*ms_flags |= MS_MANDLOCK;
			/* kernel mount flag without a fuser equivalent */
		} else if (!strcmp(tok, "nodiratime")) {
			*ms_flags |= MS_NODIRATIME;
			/* kernel mount flag without a fuser equivalent */
		} else if (!strcmp(tok, "relatime")) {
			*ms_flags |= MS_RELATIME;
			/* kernel mount flag without a fuser equivalent */
		} else if (!strcmp(tok, "remount")) {
			*ms_flags |= MS_REMOUNT;
			/* checked by the caller: it must fail, not carry on */
		} else if (!strcmp(tok, "strictatime")) {
			*ms_flags |= MS_STRICTATIME;
			/* kernel mount flag without a fuser equivalent */
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

	unsigned long ms_flags = 0;
	bool got_nodev = false, got_nosuid = false;

	struct bch_opts bch_opts = bch2_opts_empty();
	opt_set(bch_opts, nostart, 1);

	/*
	 * Parse -o options into filesystem-specific and FUSE-specific, and
	 * the kernel mount flags (they drive the atime policy and the
	 * fuseblk mount data):
	 */
	struct printbuf fs_opts = PRINTBUF;
	struct printbuf fuse_opts = PRINTBUF;
	if (options)
		parse_fusemount_options(options, &fs_opts, &fuse_opts,
					&ms_flags, &got_nodev, &got_nosuid);

	/* A remount has to reach the daemon already serving the mount;
	 * carrying on here would start a second filesystem on the same
	 * device instead. */
	if (ms_flags & MS_REMOUNT) {
		fprintf(stderr,
			"%s: remounting a FUSE mount of bcachefs isn't supported yet; unmount it and mount it again with the new options\n",
			mountpoint);
		return 1;
	}

	if (fs_opts.pos) {
		struct printbuf err = PRINTBUF;
		if (bch2_parse_mount_opts(NULL, &bch_opts, &err,
					  fs_opts.buf, true))
			die("invalid mount option: %s", err.buf);
		printbuf_exit(&err);
	}
	printbuf_exit(&fs_opts);
	opt_set(bch_opts, nostart, 1);

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

	/*
	 * Block devices are mounted by us, as fuseblk, before the
	 * filesystem is started - the kernel's exclusive claim on the
	 * source is the claim ordering that keeps us and a kernel mount
	 * from both writing (see fuse_claim()):
	 */
	struct fuse_mount_info mount = { .kind = FUSE_MOUNT_PLAIN,
					 .ms_flags = ms_flags };
	struct stat source_st;

	if (devices.nr && !stat(devices.data[0], &source_st) &&
	    S_ISBLK(source_st.st_mode)) {
		mount.kind = FUSE_MOUNT_FUSEBLK;
		/* Own the string: the devices list goes away before
		 * fuse_claim() uses it */
		mount.source = strdup(devices.data[0]);
		/* The fuseblk mount holds the source; opening it excl here
		 * too would collide with our own claim: */
		opt_set(bch_opts, noexcl, 1);
	}

	/* Held until destroy(), with the claims below: see lock_devices() */
	lock_devices(&devices);

	struct bch_fs *fs = bch2_fs_open(&devices, &bch_opts, NULL);
	if (IS_ERR(fs)) {
		fprintf(stderr, "Error opening filesystem: %s\n",
			bch2_err_str(PTR_ERR(fs)));
		free(devs_str);
		darray_exit(&devices);
		return 1;
	}

	if (mount.kind == FUSE_MOUNT_FUSEBLK)
		claim_devices(&devices, 1);

	free(devs_str);
	darray_exit(&devices);

	pthread_key_create(&rcu_key, rcu_thread_destructor);

	struct bcachefs_fuse bf = { .c = fs, .signal_fd = -1,
				    .atime = atime_from_ms_flags(ms_flags) };
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	fuse_opt_add_arg(&args, argv[0]);
	fuse_opt_add_arg(&args, "-o");
	char fsname_opt[1024];
	snprintf(fsname_opt, sizeof(fsname_opt), "fsname=%s,subtype=bcachefs",
		 device);
	fuse_opt_add_arg(&args, fsname_opt);
	if (fuse_opts.pos)
		fuse_opt_add_arg(&args, fuse_opts.buf);
	/*
	 * allow_other: a filesystem is for every user, not just whoever
	 * mounted it. Safe only because default_permissions is paired with
	 * it - the kernel checks access against the mode bits we report, as
	 * for any filesystem; we do no checking of our own, so allow_other
	 * without it would let anyone do anything.
	 */
	fuse_opt_add_arg(&args, "-o");
	fuse_opt_add_arg(&args, "allow_other,default_permissions");
	/*
	 * libfuse defaults to nodev,nosuid, as fusermount does for an
	 * unprivileged user mounting something untrusted. This is root
	 * mounting a block device, as trusted as a kernel mount - so like
	 * mount(8), only what -o asks for:
	 */
	if (!got_nodev || !got_nosuid) {
		fuse_opt_add_arg(&args, "-o");
		fuse_opt_add_arg(&args, got_nodev ? "suid" :
				 got_nosuid ? "dev" : "dev,suid");
	}
	printbuf_exit(&fuse_opts);

	if (foreground) {
		linux_shrinkers_init();

		int fuse_fd;
		int ret = fuse_claim(fs, mountpoint, &mount, &fuse_fd);
		if (ret) {
			bch2_fs_exit(fs);
			fuse_opt_free_args(&args);
			fprintf(stderr, "Error mounting filesystem: %s\n",
				strerror(-ret));
			return 1;
		}

		ret = bch2_fs_start(fs);
		if (ret) {
			if (fuse_fd >= 0)
				fuse_unclaim(mountpoint);
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

		int serve_ret = fuse_serve(se, mountpoint, &mount, fuse_fd);

		fuse_remove_signal_handlers(se);
		fuse_session_destroy(se);
		fuse_opt_free_args(&args);

		if (serve_ret < 0) {
			fprintf(stderr, "Error mounting filesystem: %s\n",
				strerror(-serve_ret));
			return 1;
		}
	} else {

		int pipe_fds[2];
		if (pipe(pipe_fds) != 0)
			return 1;
		pid_t pid = fork();
		if (pid < 0)
			return 1;
		if (pid > 0) {
			close(pipe_fds[1]);

			/*
			 * The child owns the filesystem now: the parent's
			 * copy must never shut it down, not even on the
			 * error paths below.
			 */

			move_to_own_scope(pid, device, mountpoint);

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
						"filesystem started but was never served: %s\n",
						reason);
				else
					fprintf(stderr,
						"filesystem started but was never served\n");
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
		/*
		 * Daemon mode must not hold on to the caller's stdio: a
		 * caller reading our output through a pipe - `mount ... |
		 * tee`, `$(mount ...)` - waits for EOF, and while the daemon
		 * has the write end it never comes (xfstests generic/067
		 * hung forever in tee). All three:
		 */
		int null_fd = open("/dev/null", O_RDWR);
		if (null_fd >= 0) {
			dup2(null_fd, STDIN_FILENO);
			dup2(null_fd, STDOUT_FILENO);
			dup2(null_fd, STDERR_FILENO);
			if (null_fd > STDERR_FILENO)
				close(null_fd);
		}

		bf.signal_fd = pipe_fds[1];
		linux_shrinkers_init();

		/*
		 * fork() must happen before spawning threads
		 * (linux_shrinkers_init, bch2_fs_start) because only the
		 * calling thread survives fork().
		 */

		int fuse_fd;
		int ret = fuse_claim(fs, mountpoint, &mount, &fuse_fd);
		if (ret) {
			signal_parent_err(pipe_fds[1], CHILD_ERR_MOUNT,
					  strerror(-ret));
			bch2_fs_exit(fs);
			close(pipe_fds[1]);
			exit(1);
		}

		ret = bch2_fs_start(fs);
		if (ret) {
			if (fuse_fd >= 0)
				fuse_unclaim(mountpoint);
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

		int serve_ret = fuse_serve(se, mountpoint, &mount, fuse_fd);

		fuse_remove_signal_handlers(se);
		fuse_session_destroy(se);
		close(pipe_fds[1]);

		if (serve_ret < 0)
			signal_parent_err(pipe_fds[1], CHILD_ERR_MOUNT,
					  strerror(-serve_ret));
	}

	return 0;
}

#endif
