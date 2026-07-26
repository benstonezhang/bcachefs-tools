#include <sys/types.h>

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "libbcachefs.h"

#define QCOW_MAGIC (('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)
#define QCOW_VERSION 2
#define QCOW_OFLAG_COPIED (1ULL << 63)

struct qcow2_hdr {
	u32 magic;
	u32 version;

	u64 backing_file_offset;
	u32 backing_file_size;

	u32 block_bits;
	u64 size;
	u32 crypt_method;

	u32 l1_size;
	u64 l1_table_offset;

	u64 refcount_table_offset;
	u32 refcount_table_blocks;

	u32 nb_snapshots;
	u64 snapshots_offset;
};

static void flush_l2(struct qcow2_image *img)
{
	if (img->l1_index != (u32)-1) {
		unsigned l2_size = img->block_size / sizeof(u64);
		u64 *l2_be = xmalloc(img->block_size);

		for (unsigned i = 0; i < l2_size; i++)
			l2_be[i] = cpu_to_be64(img->l2_table[i]);

		img->l1_table[img->l1_index] = img->offset | QCOW_OFLAG_COPIED;
		xpwrite(img->outfd, l2_be, img->block_size, img->offset,
			"qcow2 l2 table");
		img->offset += img->block_size;

		memset(img->l2_table, 0, img->block_size);
		img->l1_index = (u32)-1;
		free(l2_be);
	}
}

static void add_l2(struct qcow2_image *img, u64 src_blk, u64 dst_offset)
{
	unsigned l2_size = img->block_size / sizeof(u64);
	u64 l1_index = src_blk / l2_size;
	u64 l2_index = src_blk & (l2_size - 1);

	if (img->l1_index != l1_index) {
		flush_l2(img);
		img->l1_index = l1_index;
	}

	img->l2_table[l2_index] = dst_offset | QCOW_OFLAG_COPIED;
}

struct qcow2_image *qcow2_image_open(int infd, int outfd, unsigned block_size)
{
	assert(is_power_of_2(block_size));

	u64 image_size = get_size(infd);
	unsigned l2_size = block_size / sizeof(u64);
	unsigned l1_size = DIV_ROUND_UP(image_size, (u64)block_size * l2_size);

	struct qcow2_image *img = xcalloc(1, sizeof(*img));
	img->infd = infd;
	img->outfd = outfd;
	img->block_size = block_size;
	img->image_size = image_size;
	img->l1_nr = l1_size;
	img->l1_table = xcalloc(l1_size, sizeof(u64));
	img->l2_table = xcalloc(l2_size, sizeof(u64));
	img->l1_index = (u32)-1;
	img->offset = round_up(72, block_size);

	return img;
}

void qcow2_image_write_buf(struct qcow2_image *img, const void *buf, size_t len,
			   u64 src_offset)
{
	assert(len % img->block_size == 0);

	u64 dst_offset = img->offset;
	xpwrite(img->outfd, buf, len, dst_offset, "qcow2 data");
	img->offset += len;

	for (size_t i = 0; i < len; i += img->block_size) {
		add_l2(img, (src_offset + i) / img->block_size, dst_offset + i);
	}
}

void qcow2_image_write_ranges(struct qcow2_image *img, ranges *data)
{
	void *buf = xmalloc(img->block_size);
	u64 src_offset;

	ranges_roundup(data, img->block_size);
	ranges_sort_merge(data);

	/* Write data: */
	darray_for_each(*data, r) for (src_offset = r->start;
				       src_offset < r->end;
				       src_offset += img->block_size)
	{
		xpread(img->infd, buf, img->block_size, src_offset);
		qcow2_image_write_buf(img, buf, img->block_size, src_offset);
	}

	free(buf);
}

void qcow2_image_close(struct qcow2_image *img)
{
	struct qcow2_hdr hdr = { 0 };
	u64 dst_offset;

	flush_l2(img);

	/* Write L1 table: */
	dst_offset = img->offset;
	img->offset += round_up(img->l1_nr * sizeof(u64), img->block_size);
	u64 *l1_be = xmalloc(img->l1_nr * sizeof(u64));
	for (unsigned i = 0; i < img->l1_nr; i++)
		l1_be[i] = cpu_to_be64(img->l1_table[i]);
	xpwrite(img->outfd, l1_be, img->l1_nr * sizeof(u64), dst_offset,
		"qcow2 l1 table");
	free(l1_be);

	/* Write header: */
	hdr.magic = cpu_to_be32(QCOW_MAGIC);
	hdr.version = cpu_to_be32(QCOW_VERSION);
	hdr.block_bits = cpu_to_be32(ilog2(img->block_size));
	hdr.size = cpu_to_be64(img->image_size);
	hdr.l1_size = cpu_to_be32(img->l1_nr);
	hdr.l1_table_offset = cpu_to_be64(dst_offset);

	void *buf = xcalloc(1, img->block_size);
	memcpy(buf, &hdr, sizeof(hdr));
	xpwrite(img->outfd, buf, img->block_size, 0, "qcow2 header");

	free(img->l2_table);
	free(img->l1_table);
	free(buf);
	free(img);
}

void qcow2_to_raw(int infd, int outfd)
{
	struct qcow2_hdr hdr;
	xpread(infd, &hdr, sizeof(hdr), 0);

	if (be32_to_cpu(hdr.magic) != QCOW_MAGIC)
		die("not a qcow2 image");
	if (be32_to_cpu(hdr.version) != QCOW_VERSION)
		die("incorrect qcow2 version");

	u64 size = be64_to_cpu(hdr.size);
	if (ftruncate(outfd, size))
		die("error truncating output file: %m");

	u32 block_size = 1U << be32_to_cpu(hdr.block_bits);
	u32 l1_size = be32_to_cpu(hdr.l1_size);
	u32 l2_size = block_size / 8;

	u64 l1_offset = be64_to_cpu(hdr.l1_table_offset);
	u64 *l1_table = xmalloc(l1_size * 8);
	xpread(infd, l1_table, l1_size * 8, l1_offset);

	void *l2_table = xmalloc(block_size);
	void *buf = xmalloc(block_size);

	for (unsigned i = 0; i < l1_size; i++) {
		u64 l1_entry = be64_to_cpu(l1_table[i]);
		if (!l1_entry)
			continue;

		xpread(infd, l2_table, block_size,
		       l1_entry & ~QCOW_OFLAG_COPIED);

		for (unsigned j = 0; j < l2_size; j++) {
			u64 l2_entry = be64_to_cpu(((u64 *)l2_table)[j]);
			u64 src_offset = l2_entry & ~QCOW_OFLAG_COPIED;
			if (!src_offset)
				continue;

			u64 dst_offset = ((u64)i * l2_size + j) * block_size;
			xpread(infd, buf, block_size, src_offset);
			xpwrite(outfd, buf, block_size, dst_offset, "raw data");
		}
	}

	free(l1_table);
	free(l2_table);
	free(buf);
}
