#ifndef MUZIX_FS_FILE_IO_H
#define MUZIX_FS_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

#include "block_cache.h"
#include "allocator.h"
#include "fs_types.h"

/* An inode addresses MUZIX_FS_DIRECT_ZONES blocks directly plus one indirect
 * block holding MUZIX_FS_BLOCK_SIZE/2 zone numbers: MUZIX_FS_ZONE_SLOTS in
 * total.  Nothing above that set exists, so a block index or file offset past
 * it has no zone to resolve to. */
#define MUZIX_FS_INDIRECT_ZONES (MUZIX_FS_BLOCK_SIZE / 2u)
#define MUZIX_FS_ZONE_SLOTS (MUZIX_FS_DIRECT_ZONES + MUZIX_FS_INDIRECT_ZONES)
#define MUZIX_FS_MAX_FILE_SIZE \
    ((uint32_t)MUZIX_FS_ZONE_SLOTS * (uint32_t)MUZIX_FS_BLOCK_SIZE)

int muzix_fs_file_read(muzix_fs_block_cache_t *cache,
                       const muzix_fs_inode_t *inode,
                       uint32_t position,
                       uint8_t *buffer,
                       size_t length,
                       size_t *result);
int muzix_fs_file_write(muzix_fs_block_cache_t *cache,
                        muzix_fs_inode_t *inode,
                        uint32_t position,
                        const uint8_t *buffer,
                        size_t length,
                        size_t *result);
int muzix_fs_file_write_alloc(muzix_fs_block_cache_t *cache,
                          muzix_fs_zone_allocator_t *allocator,
                          muzix_fs_inode_t *inode,
                          uint32_t position,
                          const uint8_t *buffer,
                          size_t length,
                          size_t *result);

#endif
