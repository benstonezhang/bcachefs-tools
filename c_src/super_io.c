/*
 * Superblock read/write operations.
 *
 * Ported from src/wrappers/super_io.rs.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "libbcachefs.h"
#include "sb/io.h"
#include "data/checksum.h"

/**
 * bch2_super_write - Write superblock to all layout locations on disk.
 *
 * @fd: File descriptor for the device.
 * @sb: Pointer to fully initialized bch_sb.
 *
 * Exits on I/O errors (matches C die() behavior).
 */
void bch2_super_write(int fd, struct bch_sb *sb)
{
	unsigned bs = get_blocksize_physical_hint(fd);

	for (unsigned i = 0; i < sb->layout.nr_superblocks; i++) {
		sb->offset = sb->layout.sb_offset[i];

		u64 offset_sectors = le64_to_cpu(sb->offset);

		sb->csum = csum_vstruct(NULL, BCH_SB_CSUM_TYPE(sb),
					__bch2_sb_key_nonce(sb), sb);

		size_t sb_bytes = vstruct_bytes(sb);

		if (offset_sectors == BCH_SB_SECTOR && bs > 4096) {
			/*
			 * Layout and superblock are in the same aligned block;
			 * write them together.
			 */
			size_t layout_offset = (size_t)BCH_SB_LAYOUT_SECTOR << 9;
			size_t layout_bytes = sizeof(struct bch_sb_layout);
			size_t sb_offset = (size_t)offset_sectors << 9;
			size_t write_len = round_up(sb_offset + sb_bytes, bs);
			void *buf = xcalloc(1, write_len);

			memcpy(buf + layout_offset, &sb->layout, layout_bytes);
			memcpy(buf + sb_offset, sb, sb_bytes);

			xpwrite(fd, buf, write_len, 0, "superblock");
			free(buf);
		} else {
			if (offset_sectors == BCH_SB_SECTOR) {
				/* Write backup layout in the block preceding the superblock */
				void *buf = xmalloc(bs);

				xpread(fd, buf, bs, 4096 - bs);

				size_t layout_bytes = sizeof(struct bch_sb_layout);
				memcpy(buf + bs - layout_bytes, &sb->layout, layout_bytes);

				xpwrite(fd, buf, bs, 4096 - bs, "backup layout");
				free(buf);
			}

			size_t write_len = round_up(sb_bytes, bs);
			void *buf = xcalloc(1, write_len);
			memcpy(buf, sb, sb_bytes);

			xpwrite(fd, buf, write_len, offset_sectors << 9, "superblock");
			free(buf);
		}
	}

	if (fsync(fd))
		die("fsync failed writing superblock: %m");
}

/**
 * __bch2_super_read - Read a superblock from disk at the given sector offset.
 *
 * Returns a malloc'd bch_sb pointer (caller must free).
 * Exits if the magic doesn't match or on I/O error.
 */
struct bch_sb *__bch2_super_read(int fd, u64 sector)
{
	__uuid_t bcache_magic = BCACHE_MAGIC;
	__uuid_t bchfs_magic = BCHFS_MAGIC;

	/* Read the fixed-size header first */
	struct bch_sb sb_header;
	xpread(fd, &sb_header, sizeof(sb_header), sector << 9);

	if (memcmp(sb_header.magic.b, bcache_magic.b, 16) &&
	    memcmp(sb_header.magic.b, bchfs_magic.b, 16))
		die("not a bcachefs superblock");

	size_t bytes = vstruct_bytes(&sb_header);
	struct bch_sb *sb = xmalloc(bytes);

	xpread(fd, sb, bytes, sector << 9);

	return sb;
}

/**
 * bch2_sb_layout_init - Initialize superblock layout with primary and backup positions.
 *
 * @l: Superblock layout to initialize.
 * @block_size: In bytes.
 * @bucket_size: In bytes.
 * @sb_size: In 512-byte sectors.
 * @sb_start: In 512-byte sectors.
 * @sb_end: In 512-byte sectors.
 * @no_sb_at_end: Disable creating backup superblock at the end of the disk.
 */
void bch2_sb_layout_init(struct bch_sb_layout *l, unsigned block_size,
			 unsigned bucket_size, unsigned sb_size, u64 sb_start,
			 u64 sb_end, bool no_sb_at_end)
{
	__uuid_t bchfs_magic = BCHFS_MAGIC;

	memset(l, 0, sizeof(*l));

	memcpy(l->magic.b, bchfs_magic.b, 16);
	l->layout_type = 0;
	l->nr_superblocks = 2;
	l->sb_max_size_bits = ilog2(sb_size);

	/* Create two superblocks in the allowed range */
	u64 sb_pos = sb_start;
	for (unsigned i = 0; i < l->nr_superblocks; i++) {
		if (sb_pos != BCH_SB_SECTOR) {
			u64 align = block_size >> 9;
			sb_pos = DIV_ROUND_UP(sb_pos, align) * align;
		}

		l->sb_offset[i] = cpu_to_le64(sb_pos);
		sb_pos += sb_size;
	}

	if (sb_pos > sb_end)
		die("insufficient space for superblocks: need %llu sectors but only %llu available",
		    sb_pos - sb_start, sb_end - sb_start);

	/*
	 * Also create a backup superblock at the end of the disk:
	 *
	 * If we're not creating a superblock at the default offset, it
	 * means we're being run from the migrate tool and we could be
	 * overwriting existing data if we write to the end of the disk
	 */
	if (sb_start == BCH_SB_SECTOR && !no_sb_at_end) {
		u64 sb_max_size = 1ULL << l->sb_max_size_bits;
		u64 bucket_sectors = bucket_size >> 9;
		u64 backup_sb = (sb_end - sb_max_size) / bucket_sectors *
				bucket_sectors;
		l->sb_offset[l->nr_superblocks++] = cpu_to_le64(backup_sb);
	}
}
