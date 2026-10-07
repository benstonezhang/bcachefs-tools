#ifndef _TOOLS_UTIL_H
#define _TOOLS_UTIL_H

#include <errno.h>
#include <mntent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/bug.h>
#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/fiemap.h>

#include "bcachefs.h"
#include "btree/bbpos.h"

#define noreturn __attribute__((noreturn))

void die(const char *, ...) __attribute__((format(printf, 1, 2))) noreturn;

void bch2_install_fatal_signal_handlers(void);

char *vmprintf(const char *fmt, va_list args)
	__attribute__((format(printf, 1, 0)));

char *mprintf(const char *, ...) __attribute__((format(printf, 1, 2)));

void xpread(int, void *, size_t, off_t);
void xpwrite(int, const void *, size_t, off_t, const char *);
struct stat xfstatat(int, const char *, int);
struct stat xfstat(int);
struct stat xstat(const char *);

void *xmalloc(size_t);
void *xcalloc(size_t, size_t);
void *xrealloc(void *, size_t);
void xposix_memalign(void **, size_t, size_t);
void *xaligned_alloc(size_t, size_t);
char *xstrndup(const char *, size_t);
char *xstrdup(const char *);

#define xopenat(_dirfd, _path, ...)                               \
	({                                                        \
		int _fd = openat((_dirfd), (_path), __VA_ARGS__); \
		if (_fd < 0)                                      \
			die("Error opening %s: %m", (_path));     \
		_fd;                                              \
	})

#define xopen(...) xopenat(AT_FDCWD, __VA_ARGS__)

#define xioctl(_fd, _nr, ...)                                  \
	({                                                     \
		int _ret = ioctl((_fd), (_nr), ##__VA_ARGS__); \
		if (_ret < 0)                                  \
			die(#_nr " ioctl error: %m");          \
		_ret;                                          \
	})

#define xclose(_fd)                                                    \
	do {                                                           \
		if (close(_fd))                                        \
			die("error closing fd: %m at %s:%u", __FILE__, \
			    __LINE__);                                 \
	} while (0)

void write_file_str(int, const char *, const char *);
char *read_file_str(int, const char *);
u64 read_file_u64(int, const char *);

ssize_t read_string_list_or_die(const char *, const char *const[],
				const char *);
u64 read_flag_list_or_die(char *, const char *const[], const char *);

unsigned get_blocksize(int);

void blkid_check(int fd, const char *path, bool force);

bool ask_yn(void);

struct range {
	u64 start;
	u64 end;
};

typedef DARRAY(struct range) ranges;

static inline void range_add(ranges *data, u64 offset, u64 size)
{
	darray_push(data,
		    ((struct range){ .start = offset, .end = offset + size }));
}

void ranges_sort_merge(ranges *);
void ranges_roundup(ranges *, unsigned);
void ranges_rounddown(ranges *, unsigned);

struct hole_iter {
	ranges r;
	size_t idx;
	u64 end;
};

struct range hole_iter_next(struct hole_iter *);

#define for_each_hole(_iter, _ranges, _end, _i)                       \
	for (_iter = (struct hole_iter){ .r = _ranges, .end = _end }; \
	     (_iter.idx <= _iter.r.nr &&                              \
	      (_i = hole_iter_next(&_iter), true));)

struct fiemap_iter {
	struct fiemap *f;
	unsigned idx;
	int fd;
};

void fiemap_iter_init(struct fiemap_iter *, int);
struct fiemap_extent fiemap_iter_next(struct fiemap_iter *);
void fiemap_iter_exit(struct fiemap_iter *);

#define fiemap_for_each(fd, iter, extent) \
	for (fiemap_iter_init(&iter, fd); \
	     (extent = fiemap_iter_next(&iter)).fe_length;)

char *strcmp_prefix(char *, const char *);

/* Avoid conflicts with libblkid's crc32 function in static builds */
#define crc32c bch_crc32c
u32 crc32c(u32, const void *, size_t);

#define args_shift(_nr)                         \
	do {                                    \
		unsigned _n = min((_nr), argc); \
		argc -= _n;                     \
		argv += _n;                     \
	} while (0)

#define arg_pop()                                   \
	({                                          \
		char *_ret = argc ? argv[0] : NULL; \
		if (_ret)                           \
			args_shift(1);              \
		_ret;                               \
	})

struct bpos bpos_parse(char *);
struct bbpos bbpos_parse(char *);

struct bbpos_range {
	struct bbpos start;
	struct bbpos end;
};

struct bbpos_range bbpos_range_parse(char *);

unsigned version_parse(char *);

darray_const_str get_or_split_cmdline_devs(int argc, char *argv[]);

char *pop_cmd(int *argc, char *argv[]);

char *fmt_bytes_human(u64);
char *fmt_sectors_human(u64);
char *fmt_num_human(u64);
char *fmt_duration_human(u64);

int subvol_root(const char *, char **);

int stderr_unless_error(int (*fn)(void *), void *);

#endif /* _TOOLS_UTIL_H */
