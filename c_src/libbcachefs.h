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
int sysfs_write_str(int, const char *, const char *);

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
	/*
	 * Free space by replica count: free[n - 1] is what could be granted at n
	 * replicas, cumulative and non-increasing. Invalid on a kernel too old
	 * for the v2 ioctl - free space is a vector we simply can't see there,
	 * which is different from it being zero, so callers print nothing
	 * rather than guess.
	 */
	bool free_valid;
	u64 free[BCH_IOCTL_QUERY_ACCOUNTING_FREE_NR];
	u64 free_now[BCH_IOCTL_QUERY_ACCOUNTING_FREE_NR];
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

const char *bch_splines_reticulate(u64);

enum bch_prompt_kind {
	BCH_PROMPT_NONE,
	BCH_PROMPT_AGENT,
	BCH_PROMPT_TERMINAL,
};

enum bch_prompt_stirred {
	BCH_PROMPT_STIR_NOTHING,
	BCH_PROMPT_STIR_MOOT,
	BCH_PROMPT_STIR_ANSWERED,
};

/*
 * Something that can settle a question by happening.
 *
 * Someone asked whether to mount without a disk, who responds by plugging the
 * disk in, has answered - and so has someone who supplies the passphrase over
 * a socket while the terminal prompt is up. The first makes the question moot,
 * the second answers it; bch_prompt_stirred is the difference.
 */
struct bch_prompt_watch {
	/*
	 * Readiness alone is not an answer, so stirred() decides whether *this*
	 * wakeup settled anything.
	 */
	int (*raw_fd)(void *);
	enum bch_prompt_stirred (*stirred)(void *);
	void *ctx;
};

/*
 * The only place an answer is written down: both rendered forms and the parse
 * derive from this, so a question cannot offer a letter it will not accept.
 */
struct bch_prompt_choice {
	char			 key;	/* matched case-insensitively; the aliases too */
	const char *const	*aliases; /* NULL-terminated; may be NULL */
	const char		*short_name; /* for the bracketed summary; "" shows the bare key */
	const char		*blurb;	 /* for a terminal, which has room for a sentence */
	long			 answer;
};

/*
 * Answers live per question rather than in one shared parser, so that two
 * questions cannot cross-parse each other's vocabulary.
 */
struct bch_prompt_question {
	const char	*prompt;	/* one line: the agent protocol's `Message=` is one line */
	const struct bch_prompt_choice *choices;
	unsigned	nr_choices;
	/*
	 * Also what a bare Enter and a timed-out boot prompt mean. Shown
	 * capitalised in the summary.
	 */
	long		 silence;
	/*
	 * This question is about losing data, and the prompt says so. Drawn in
	 * red where there is a terminal to draw it on; everywhere else the words
	 * have to carry it alone, which is why they are in @prompt and not in a
	 * decoration of their own.
	 */
	bool		 alarm;
	const char	*uuid;
	/*
	 * 0 waits indefinitely - what `--timeout=0` means to
	 * systemd-ask-password.
	 */
	u64		 timeout_secs;
};

/* What ended a bch_prompt_wait(). */
enum bch_prompt_waited {
	BCH_PROMPT_WAITED_READABLE,	/* the descriptor being polled has something for us */
	BCH_PROMPT_WAITED_ANSWERED,	/* a watch answered; it is holding the answer */
	BCH_PROMPT_WAITED_MOOT,		/* a watch says the question stopped applying */
	BCH_PROMPT_WAITED_TIMEOUT,	/* the deadline passed with none of the above */
};

enum bch_prompt_kind bch_prompt_detect(void);
int bch_prompt_put(enum bch_prompt_kind, const struct bch_prompt_question *,
		   const struct bch_prompt_watch *, long *);
const char *bch_prompt_fs_name(struct bch_sb *, char *, size_t);
bool bch_prompt_stdin_is_dev_null(void);
extern const char bch_prompt_no_one_to_ask[];

/* plymouth.c */
void bch_plymouth_send(const char *);
bool bch_plymouth_active(void);

/*
 * A block of text kept below the kernel conversation and refreshed as it runs.
 * See thread_with_file.c. The relay owns the cursor: it takes the block down
 * before anything the filesystem says is written, so the message lands on a
 * clean line and ends up above the block rather than through it, and puts the
 * block back afterwards.
 */
struct bch_status_display {
	/* How long to wait for either end to speak before redrawing anyway. */
	u64 (*interval_ms)(void *);
	/* Take the block down, leaving the cursor where it started. */
	int (*erase)(void *);
	/* Put it back, with current contents. */
	int (*draw)(void *);
	void *ctx;
};

/* thread_with_file.c */
int bch_thread_relay(int, int, struct bch_status_display *);

/* device_scan.c */
struct bch_scanned_sb {
	char			*path;
	struct bch_sb_handle	sb;
};
typedef DARRAY(struct bch_scanned_sb) bch_scanned_sbs;

void bch2_scanned_sbs_exit(bch_scanned_sbs *);
int bch2_read_super_silent_opts(const char *, struct bch_opts *,
				struct bch_sb_handle *);
unsigned bch2_scanned_expected_devices(const bch_scanned_sbs *);
unsigned bch2_scanned_present_devices(const bch_scanned_sbs *);
int bch2_get_devices_by_uuid(uuid_t, struct bch_opts *, bool, bch_scanned_sbs *);
int bch2_get_devices_by_label(const char *, struct bch_opts *, bool, bch_scanned_sbs *);
int bch2_scan_sbs(const char *, struct bch_opts *, bch_scanned_sbs *);
int bch2_scan_sbs_for_mount(const char *, struct bch_opts *, bch_scanned_sbs *);
int bch2_devices_from_superblocks(const bch_scanned_sbs *, dev_names *);

struct bch_device_watch;
struct bch_device_watch *bch2_device_watch_new(uuid_t, const struct bch_opts *, bool);
void bch2_device_watch_free(struct bch_device_watch *);
int bch2_device_watch_fd(struct bch_device_watch *);
bool bch2_device_watch_every_member_present(struct bch_device_watch *);

/* splitbrain.c */
struct bch_divergent {
	char	*path;		/* path, owned */
	u8	dev_idx;
	u64	seq;
	u64	write_time;
	/* What the authoritative superblock last recorded for this device, or 0. */
	u64	expected_seq;
};
typedef DARRAY(struct bch_divergent) bch_divergents;
void bch2_splitbrain_divergents_exit(bch_divergents *);

void bch2_splitbrain_find(const bch_scanned_sbs *, const struct bch_opts *,
			  bch_divergents *);
char *bch2_splitbrain_report(const bch_scanned_sbs *, const bch_divergents *);
bool bch2_splitbrain_ask(struct bch_sb_handle *);

/* degraded.c */
enum bch_degraded_answer {
	BCH_DEGRADED_ANSWER_NO,
	BCH_DEGRADED_ANSWER_READ_ONLY,
	BCH_DEGRADED_ANSWER_YES,
};

struct bch_degraded_outcome {
	enum {
		BCH_DEGRADED_MOUNT,
		BCH_DEGRADED_RESCAN,
		BCH_DEGRADED_NO,
	} kind;
	const char	*fs_opt;	/* "degraded=yes" / "degraded=very" */
	bool		read_only;
};

struct bch_degraded_ask;
struct bch_degraded_ask *bch2_degraded_ask_new(const bch_scanned_sbs *,
					       const struct bch_opts *);
void bch2_degraded_ask_free(struct bch_degraded_ask *);
int bch2_degraded_ask_put(const struct bch_degraded_ask *, int err,
			  struct bch_degraded_outcome *);
char *bch2_degraded_append_opt(char *fs_opts, const char *opt);

/* fs_context.c */
#define BCH_FS_CONTEXT_PARAM_VALUE_MAX	255

enum bch_fs_context_level {
	BCH_FS_CONTEXT_ERROR,
	BCH_FS_CONTEXT_WARNING,
	BCH_FS_CONTEXT_NOTICE,
};

struct bch_fs_context_msg {
	enum bch_fs_context_level	level;
	char				*text;
};

u32 bch_fs_context_mount_attrs(unsigned long);
unsigned bch_fs_context_sb_flag_params(unsigned long, const char **);
int bch_fs_context_open(const char *);
int bch_fs_context_set(int, const char *, const char *);
int bch_fs_context_status_fd(int);
int bch_fs_context_create(int);
int bch_fs_context_fsmount(int, u32);
struct bch_fs_context_msg *bch_fs_context_drain_log(int);
void bch_fs_context_msgs_free(struct bch_fs_context_msg *);
int bch_fs_context_move_mount(int, const char *);

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
