/*
 * Generic filesystem accounting query wrapper.
 *
 * Ported from src/wrappers/accounting.rs.
 */

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdlib.h>

#include "libbcachefs.h"
#include "alloc/accounting.h"

void bchu_accounting_result_free(struct accounting_result *res)
{
	darray_for_each(res->entries, e)
	{
		free(*e);
	}
	darray_exit(&res->entries);
}

/*
 * Query filesystem accounting data via BCH_IOCTL_QUERY_ACCOUNTING.
 *
 * BCH_IOCTL_QUERY_ACCOUNTING is _IOW(0xbc, 21, struct bch_ioctl_query_accounting).
 * The struct has a flex array, so the kernel uses the header size for the
 * ioctl number calculation in some contexts.
 */
struct accounting_result bchu_fs_accounting_query(struct bchfs_handle fs,
						  unsigned typemask)
{
	struct accounting_result res = { 0 };
	unsigned accounting_u64s = 128;
	struct bch_ioctl_query_accounting *a = NULL;
	u64 kernel_version = bcachefs_kernel_version();

	/*
	 * Early versions of disk accounting used big-endian for bpos.
	 */
	bool need_swab =
		kernel_version > 0 &&
		kernel_version <
			bcachefs_metadata_version_disk_accounting_big_endian;

	while (1) {
		a = xrealloc(a, sizeof(*a) + accounting_u64s * sizeof(u64));
		memset(a, 0, sizeof(*a));
		a->accounting_u64s = accounting_u64s;
		a->accounting_types_mask = typemask;

		if (ioctl(fs.ioctl_fd, BCH_IOCTL_QUERY_ACCOUNTING, a)) {
			if (errno == ERANGE) {
				accounting_u64s *= 2;
				continue;
			}
			fprintf(stderr, "error: query_accounting ioctl failed (kernel too old?): %s\n",
				strerror(errno));
			free(a);
			return res;
		}
		break;
	}

	res.capacity = a->capacity;
	res.used = a->used;
	res.online_reserved = a->online_reserved;

	/*
	 * Robust parsing loop for query_accounting ioctl buffer - strictly avoids
	 * kernel bkey macros that contain BUG_ON assertions.
	 *
	 * Each entry starts with a `struct bkey` header (5 u64s = 40 bytes),
	 * followed by counters. The `bkey.u64s` field gives the total size
	 * of key + value in u64s.
	 *
	 * On little-endian: bkey layout is [u64s(1B), format:nw(1B), type(1B), pad(1B),
	 *                                   bversion(12B), size(4B), bpos(20B)]
	 * bpos is the last 20 bytes of the header.
	 */
	u64 *p = (u64 *)a->accounting;
	u64 *end = p + a->accounting_u64s;

	while (p < end) {
		struct bkey *k = (struct bkey *)p;
		if (k->u64s == 0 || k->u64s < BKEY_U64s)
			break;
		if (p + k->u64s > end)
			break;

		if (k->type == KEY_TYPE_accounting) {
			struct accounting_entry *e = xcalloc(1, sizeof(*e));
			struct bpos pos = k->p;
			if (need_swab)
				bch2_bpos_swab(&pos);

			bpos_to_disk_accounting_pos(&e->pos, pos);

			/*
			 * Counters start after the bkey header (bch_accounting.d[])
			 * bch_accounting has just a bch_val (0 bytes), then d[]
			 * So counters start at u64 offset BKEY_U64s
			 */
			unsigned nr = k->u64s - BKEY_U64s;
			e->nr_counters =
				min(nr, (unsigned)BCH_ACCOUNTING_MAX_COUNTERS);
			for (unsigned j = 0; j < e->nr_counters; j++)
				e->counters[j] = p[BKEY_U64s + j];

			darray_push(&res.entries, e);
		}
		p += k->u64s;
	}

	free(a);
	return res;
}
