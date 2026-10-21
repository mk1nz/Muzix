#ifndef MUZIX_INODE_TABLE_H
#define MUZIX_INODE_TABLE_H

#include <stdint.h>

#include "fs_types.h"

/*
 * Inode slots.  Slot 0 is never handed out and slot 1 is the root, so the
 * number of files the filesystem can hold is this minus two.
 *
 * Sixteen, because sixteen 32-byte records are exactly one 512-byte block and
 * the in-memory table is what muzix_fs_inode_table_load() and
 * muzix_fs_inode_table_save() read and write.  Eighteen did not fit: 18 * 32 is
 * 576 bytes, so slot 16 and slot 17 were serialised past the end of the block
 * and straight into the block cache's slot metadata - the block number, the
 * valid flag and the dirty flag that sit after the 512-byte buffer in
 * muzix_fs_cache_slot_t - which is metadata corruption rather than a failed
 * write.  Each extra slot is also a 32-byte record in _DATA, and the headroom
 * gate in tools/check_kernel_layout.rb stays authoritative when changing this
 * value.
 */
#define MUZIX_FS_INODE_SLOTS 20u

/*
 * The on-disk inode table is TWO blocks, 3 and 4, which is what
 * tools/make_minixfs.rb writes (its own comment: "blocks 3-4, 2 blocks = 1024
 * bytes") and what kernel/exec_loader.c reads, choosing the block with
 * `3 + (inode >> 4)`.  The library addresses the first of the two only, which
 * is MUZIX_FS_INODE_SLOTS records; the second block is the image's remaining
 * capacity and is reserved by muzix_fs_volume_reserve_metadata().
 *
 * muzix_fs_volume_mount() therefore checks the superblock's inode count
 * against this wider figure, the number of records the image can hold, and not
 * against the slot count: a tool-shaped image declares MAX_INODES = 18 so that
 * its 2 blocks can describe sixteen files, and refusing it for carrying more
 * inodes than one block holds would make every real image unmountable.
 */
#define MUZIX_FS_INODE_TABLE_BLOCKS 2u

/* Records the on-disk inode table can hold: one block's worth per block. */
#define MUZIX_FS_INODE_TABLE_RECORDS \
    (MUZIX_FS_INODE_TABLE_BLOCKS * MUZIX_FS_INODE_SLOTS)

typedef struct {
    muzix_fs_inode_t entries[MUZIX_FS_INODE_SLOTS];
} muzix_fs_inode_table_t;

void muzix_fs_inode_table_init(muzix_fs_inode_table_t *table);
int muzix_fs_inode_alloc(muzix_fs_inode_table_t *table, uint16_t *inode_nr);
int muzix_fs_inode_release(muzix_fs_inode_table_t *table, uint16_t inode_nr);
muzix_fs_inode_t *muzix_fs_inode_get(muzix_fs_inode_table_t *table,
                                     uint16_t inode_nr);

#endif
