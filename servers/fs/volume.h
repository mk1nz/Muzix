#ifndef MUZIX_FS_VOLUME_H
#define MUZIX_FS_VOLUME_H

#include "allocator_store.h"
#include "directory_store.h"
#include "inode_store.h"
#include "superblock.h"

typedef struct {
    muzix_block_device_t *device;
    muzix_fs_block_cache_t cache;
    muzix_fs_superblock_state_t superblock;
    muzix_fs_zone_allocator_t allocator;
    muzix_fs_inode_table_t inodes;
    muzix_fs_directory_t root_directory;
    uint8_t mounted;
} muzix_fs_volume_t;

void muzix_fs_volume_init(muzix_fs_volume_t *volume,
                          muzix_block_device_t *device);
int muzix_fs_volume_format(muzix_fs_volume_t *volume,
                           uint16_t inode_count,
                           uint16_t zone_count,
                           uint16_t first_data_zone);
int muzix_fs_volume_mount(muzix_fs_volume_t *volume);
int muzix_fs_volume_flush(muzix_fs_volume_t *volume);

#endif
