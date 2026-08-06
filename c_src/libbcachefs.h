#ifndef _LIBBCACHE_H
#define _LIBBCACHE_H

#include <linux/uuid.h>
#include <stdbool.h>

#include "bcachefs.h"
#include "bcachefs_format.h"
#include "bcachefs_ioctl.h"
#include "data/read.h"
#include "data/write_types.h"
#include "fs/inode.h"
#include "util/darray.h"
#include "util/vstructs.h"
#include "tools-util.h"

#define SYSFS_BASE "/sys/fs/bcachefs/"

#define BCACHEFS_ROOT_INO 4096
#define BCACHEFS_ROOT_SUBVOL 1

#define SUPERBLOCK_SIZE_DEFAULT 2048 /* 1 MB */

#define WRITE_DATA_BUF (1 << 20)

/* option parsing */

struct bch_opt_strs {
	union {
		char *by_id[bch2_opts_nr];
		struct {
#define x(_name, ...) char *_name;
			BCH_OPTS()
#undef x
		};
	};
};

int bch2_opt_lookup_negated(const char *, bool *);
int parse_opt_val(const struct bch_option *, const char *, u64 *);
void bch2_opts_usage(unsigned, unsigned);
const struct bch_option *bch2_cmdline_opt_parse(int argc, char *argv[],
						unsigned opt_types);
struct bch_opt_strs bch2_cmdline_opts_get(int *, char *[], unsigned);
void bch2_opt_strs_free(struct bch_opt_strs *);
struct bch_opts bch2_parse_opts(struct bch_opt_strs);

struct dev_name {
	unsigned idx;
	char *dev;
	char *label;
	uuid_t uuid;
	unsigned durability;
	bool online;
	char *failure_domain;
};
typedef DARRAY(struct dev_name) dev_names;

enum device_name_mode {
	DEVICE_NAME_RAW = 0,
	DEVICE_NAME_MAPPER = 1,
};

char *dev_name_from_sysfs(const char *);
char *dev_display_name_from_sysfs(const char *, enum device_name_mode);
char *sysfs_path_from_fd(int);
u64 read_sysfs_u64(const char *);
char *read_sysfs_fd_str(int, const char *);
u64 bcachefs_kernel_version(void);
int dev_mounted(const char *);
void sysfs_write_str(int, const char *, const char *);

dev_names fs_get_devices(const char *, enum device_name_mode);
void dev_names_free(dev_names *);

bool is_multipath_dm_uuid(const char *);
char *find_multipath_holder(const char *);
char *preferred_multipath_devnode(const char *path);
char *preferred_multipath_devnode_for_block_name(const char *name);
void warn_multipath_component(const char *path, const char *mpath_dev);

void bch2_sb_layout_init(struct bch_sb_layout *, unsigned, unsigned, unsigned,
			 u64, u64, bool);
void bch2_super_write(int, struct bch_sb *);
struct bch_sb *__bch2_super_read(int, u64);

struct qcow2_image {
	int infd;
	int outfd;
	u32 block_size;
	u64 image_size;
	u64 *l1_table;
	u32 l1_nr;
	u32 l1_index;
	u64 *l2_table;
	u64 offset;
};

struct sanitize_opts;

typedef void (*qcow2_sanitize_fn)(struct bch_fs *, void *, size_t,
				  struct sanitize_opts *);

struct qcow2_image *qcow2_image_open(int infd, int outfd, unsigned block_size);
void qcow2_image_write_buf(struct qcow2_image *, const void *, size_t, u64);
void qcow2_image_write_ranges(struct qcow2_image *, ranges *);
void qcow2_image_close(struct qcow2_image *);

void qcow2_to_raw(int infd, int outfd);

/* Accounting query structures */
struct accounting_entry {
	struct disk_accounting_pos pos;
	u64 counters[BCH_ACCOUNTING_MAX_COUNTERS];
	unsigned nr_counters;
};

typedef DARRAY(struct accounting_entry *) darray_accounting_p;

struct accounting_result {
	u64 capacity;
	u64 used;
	u64 online_reserved;
	darray_accounting_p entries;
};

void bchu_accounting_result_free(struct accounting_result *);

/* Key management */

enum bch_keyring {
	BCH_KEYRING_session,
	BCH_KEYRING_user,
	BCH_KEYRING_user_session,
};

enum bch_unlock_policy {
	BCH_UNLOCK_POLICY_fail,
	BCH_UNLOCK_POLICY_wait,
	BCH_UNLOCK_POLICY_ask,
	BCH_UNLOCK_POLICY_stdin,
};

char *bch2_format_key_name(const __uuid_t *);
bool bch2_key_search(struct bch_sb *);
void bch2_wait_for_unlock(struct bch_sb *);
int bch2_key_handle_new(struct bch_sb *, const char *, enum bch_keyring);
int bch2_unlock_policy_apply(enum bch_unlock_policy, struct bch_sb_handle *,
			     const char *);

struct bchfs_handle {
	__uuid_t uuid;
	int ioctl_fd;
	int sysfs_fd;
	int dev_idx;
};

#define xbchu_ioctl(_fs, _nr_v2, _nr_v1, _arg_v2, _arg_v1)                  \
	do {                                                                \
		char _err_buf[8192] = { 0 };                                \
		(_arg_v2).err.msg_ptr = (unsigned long)_err_buf;            \
		(_arg_v2).err.msg_len = sizeof(_err_buf);                   \
                                                                            \
		int _ret = ioctl((_fs).ioctl_fd, (_nr_v2), &(_arg_v2));     \
		if (_ret < 0 && errno == ENOTTY)                            \
			_ret = ioctl((_fs).ioctl_fd, (_nr_v1), &(_arg_v1)); \
                                                                            \
		if (_ret < 0) {                                             \
			if (_err_buf[0])                                    \
				die(#_nr_v2 " ioctl error: %s", _err_buf);  \
			else                                                \
				die(#_nr_v2 " ioctl error: %m");            \
		}                                                           \
	} while (0)

struct accounting_result bchu_fs_accounting_query(struct bchfs_handle,
						  unsigned typemask);

u64 get_size(int);
u32 get_blocksize_physical_hint(int);
u32 get_blocksize_logical(int);
char *fd_to_dev_model(int);
char *fd_to_dev_serial(int);
char *fd_to_parent_disk_sysfs(int);
bool nonrot(int);

int open_device(const char *, unsigned);

/* ioctl interface: */

int bcachectl_open(void);
void bcache_fs_close(struct bchfs_handle);

int bcache_fs_open_fallible(const char *, struct bchfs_handle *);
int bcache_fs_open_if_mounted(const char *, struct bchfs_handle *);
int bcache_fs_open_if_mounted_any(const char **, unsigned, struct bchfs_handle *);
struct bchfs_handle bcache_fs_open(const char *);
struct bchfs_handle bchu_fs_open_by_dev(const char *, int *);

int bchu_dev_path_to_idx(struct bchfs_handle, const char *);

int bchu_data(struct bchfs_handle, struct bch_ioctl_data);

dev_names bchu_fs_get_devices(struct bchfs_handle);
dev_names bchu_fs_get_devices_mode(struct bchfs_handle, enum device_name_mode);
struct dev_name *dev_idx_to_name(dev_names *dev_names, unsigned idx);

void bchu_disk_add(struct bchfs_handle, const char *);
void bchu_disk_remove(struct bchfs_handle, unsigned, unsigned);
void bchu_disk_online(struct bchfs_handle, const char *);
void bchu_disk_offline(struct bchfs_handle, unsigned, unsigned);
void bchu_disk_set_state(struct bchfs_handle, unsigned, unsigned, unsigned);
void bchu_disk_resize(struct bchfs_handle, unsigned, u64);
void bchu_disk_resize_journal(struct bchfs_handle, unsigned, u64);

int bchu_subvolume_create(struct bchfs_handle, const char *);
int bchu_subvolume_destroy(struct bchfs_handle, const char *);
int bchu_subvolume_snapshot(struct bchfs_handle, const char *, const char *,
			    unsigned);

void *bchu_read_super(struct bchfs_handle, u64 *);
u16 bchu_sb_version(struct bchfs_handle);

struct bch_ioctl_dev_usage_v2 *bchu_dev_usage(struct bchfs_handle fs,
					      unsigned idx);

struct format_opts {
	char *label;
	__uuid_t uuid;
	unsigned version;
	unsigned superblock_size;
	bool encrypted;
	char *passphrase_file;
	char *passphrase;
	char *source;
	bool no_sb_at_end;
};

struct format_opts format_opts_default();

struct dev_opt_str {
	enum bch_opt_id id;
	char *str;
};

struct dev_opts {
	struct file *file;
	struct block_device *bdev;
	const char *path;

	u64 sb_offset;
	u64 sb_end;

	u64 nbuckets;
	u64 fs_size;

	/*
	 * Deferred device options (labels, failure domains): their values are
	 * resolved against the superblock being built, after it exists.
	 */
	DARRAY(struct dev_opt_str) opt_strs;

	struct bch_opts opts;
};

typedef DARRAY(struct dev_opts) dev_opts_list;

static inline struct dev_opts dev_opts_default()
{
	return (struct dev_opts){ .opts = bch2_opts_empty() };
}

static inline void dev_opt_str_push(struct dev_opts *dev, enum bch_opt_id id,
				    const char *str)
{
	darray_for_each(dev->opt_strs, e)
	{
		if (e->id == id) {
			free(e->str);
			e->str = strdup(str);
			return;
		}
	}
	darray_push(&dev->opt_strs,
		    ((struct dev_opt_str){ .id = id, .str = strdup(str) }));
}

static inline void dev_opts_opt_strs_clone(struct dev_opts *dst,
					   struct dev_opts *src)
{
	darray_for_each(src->opt_strs, e)
		darray_push(&dst->opt_strs,
			    ((struct dev_opt_str){ .id = e->id,
						   .str = strdup(e->str) }));
}

static inline void dev_opts_opt_strs_exit(struct dev_opts *dev)
{
	darray_for_each(dev->opt_strs, e)
		free(e->str);
	darray_exit(&dev->opt_strs);
}

struct bch_sb *bch2_format(struct bch_opt_strs, struct bch_opts,
			   struct format_opts, dev_opts_list);

int open_for_format(struct dev_opts *, blk_mode_t, bool);

int bch2_format_for_device_add(struct dev_opts *, unsigned, unsigned);

u32 bch2_pick_block_size(struct bch_opts, dev_opts_list);
u64 bch2_pick_bucket_size(struct bch_opts, dev_opts_list);
void bch2_check_bucket_size(struct bch_opts, struct dev_opts *);

enum bch_migrate_type {
	BCH_MIGRATE_copy,
	BCH_MIGRATE_migrate,
};

struct copy_fs_state {
	enum bch_migrate_type type;
	u64 bcachefs_inum;
	u64 dev;
	u64 reserve_start;
	ranges extents;
	unsigned verbosity;

	u64 total_files;
	u64 total_input;
	u64 total_wrote;
	u64 total_linked;

	GENRADIX(u64) hardlinks;
};

void copy_fs(struct bch_fs *c, int src_fd, const char *src_path,
	     struct copy_fs_state *s);

void strip_fs_alloc(struct bch_fs *c);
void strip_alloc_do(struct bch_fs *c);

void bch2_read_submit(struct bch_fs *c, struct bch_read_bio *rbio,
		      struct bio_vec *bvecs, unsigned nr_bvecs, void *buf,
		      size_t len, u64 offset, struct bch_inode_opts opts,
		      subvol_inum inum, bio_end_io_t endio);
int bch2_write_submit(struct bch_fs *c, struct bch_write_op *op,
		      struct bio_vec *bvecs, unsigned nr_bvecs, const void *buf,
		      size_t len, u64 inum, u64 offset, u32 subvol,
		      u32 replicas, u64 new_i_size);
int bch2_link_data(struct bch_fs *c, u64 dst_inum, s64 *sectors_delta,
		   u64 logical, u64 physical, u64 length);
void bchu_fs_read(struct bch_fs *c, subvol_inum inum, u64 offset,
		  struct bch_inode_unpacked *inode, void *buf, size_t len);

char *bch2_scan_devices(const char *);

void bch2_sb_to_text_with_names(struct printbuf *, struct bch_fs *,
				struct bch_sb *, bool, unsigned, int);

size_t bch2_demangle(const char *, char *, size_t);

#endif /* _LIBBCACHE_H */
