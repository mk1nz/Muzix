#ifndef MUZIX_FS_SUPERBLOCK_H
#define MUZIX_FS_SUPERBLOCK_H

#include "block_cache.h"
#include "fs_types.h"

typedef struct {
    muzix_fs_block_cache_t *cache;
    muzix_fs_superblock_t value;
    uint8_t loaded;
} muzix_fs_superblock_state_t;

void muzix_fs_superblock_init(muzix_fs_superblock_state_t *state,
                              muzix_fs_block_cache_t *cache);
int muzix_fs_superblock_format(muzix_fs_superblock_state_t *state,
                               uint16_t inode_count,
                               uint16_t zone_count,
                               uint16_t first_data_zone);
int muzix_fs_superblock_load(muzix_fs_superblock_state_t *state);
int muzix_fs_superblock_valid(const muzix_fs_superblock_state_t *state);

#endif
