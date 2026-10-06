#include "allocator_store.h"

#include <string.h>

int muzix_fs_allocator_load(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            muzix_fs_zone_allocator_t *allocator)
{
    uint8_t *data;

    if (!cache || !allocator || muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    allocator->first_data_zone = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    allocator->zone_count = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    if (allocator->zone_count > MUZIX_FS_MAX_ZONES ||
        allocator->first_data_zone >= allocator->zone_count) {
        return -1;
    }
    memcpy(allocator->used, &data[4], sizeof(allocator->used));
    /* Everything below first_data_zone is metadata -- superblock, allocator,
     * inode table, root directory -- and can never be handed to a file.  A
     * bitmap that does not say so is not a bitmap this filesystem wrote: with
     * the root directory unmarked, muzix_fs_zone_is_used(4) answered "free"
     * and only muzix_fs_zone_alloc's start at first_data_zone kept block 4 from
     * being allocated.  Refuse such a volume instead of trusting that. */
    for (uint16_t zone = 0; zone < allocator->first_data_zone; zone++) {
        if (!muzix_fs_zone_is_used(allocator, zone)) {
            return -1;
        }
    }
    return 0;
}

int muzix_fs_allocator_save(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            const muzix_fs_zone_allocator_t *allocator)
{
    uint8_t *data;

    if (!cache || !allocator || allocator->zone_count > MUZIX_FS_MAX_ZONES ||
        allocator->first_data_zone >= allocator->zone_count ||
        muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    memset(data, 0, MUZIX_FS_BLOCK_SIZE);
    data[0] = (uint8_t)allocator->first_data_zone;
    data[1] = (uint8_t)(allocator->first_data_zone >> 8);
    data[2] = (uint8_t)allocator->zone_count;
    data[3] = (uint8_t)(allocator->zone_count >> 8);
    memcpy(&data[4], allocator->used, sizeof(allocator->used));
    return muzix_fs_cache_mark_dirty(cache, block);
}
