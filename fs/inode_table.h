#ifndef MUZIX_INODE_TABLE_H
#define MUZIX_INODE_TABLE_H

#include <stdint.h>

#include "fs_types.h"

/*
 * Inode slots.  Slot 0 is never handed out and slot 1 is the root, so the
 * number of files the filesystem can hold is this minus two.
 *
 * The value is 14 because the ROM filesystem carries twelve files, including
 * the `hwdiag` application, and slot 0 is reserved while slot 1 is the root.
 *
 * Each extra slot is a 32-byte record in _DATA. Keep the headroom gate in
 * tools/check_kernel_layout.rb authoritative when changing this value.
 * The on-disk inode block holds sixteen records and the root directory sixteen
 * entries, so both are a ceiling of 16, and a sixteenth slot would be another
 * 32 bytes against whatever the stack floor has left.
 */
#define MUZIX_FS_INODE_SLOTS 14u

typedef struct {
    muzix_fs_inode_t entries[MUZIX_FS_INODE_SLOTS];
} muzix_fs_inode_table_t;

void muzix_fs_inode_table_init(muzix_fs_inode_table_t *table);
int muzix_fs_inode_alloc(muzix_fs_inode_table_t *table, uint16_t *inode_nr);
int muzix_fs_inode_release(muzix_fs_inode_table_t *table, uint16_t inode_nr);
muzix_fs_inode_t *muzix_fs_inode_get(muzix_fs_inode_table_t *table,
                                     uint16_t inode_nr);

#endif
