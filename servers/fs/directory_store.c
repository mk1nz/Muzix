#include "directory_store.h"

#include <string.h>

#define MUZIX_FS_DIR_ENTRY_SIZE 16u

static uint16_t muzix_fs_dir_get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void muzix_fs_dir_put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

int muzix_fs_directory_load(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            muzix_fs_directory_t *directory)
{
    uint8_t *data;
    uint8_t slot;

    if (!cache || !directory ||
        muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    muzix_fs_directory_init(directory);
    for (slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        uint8_t *entry = &data[slot * MUZIX_FS_DIR_ENTRY_SIZE];
        directory->entries[slot].inode = muzix_fs_dir_get16(entry);
        memcpy(directory->entries[slot].name, &entry[2], MUZIX_FS_NAME_MAX);
    }
    return 0;
}

int muzix_fs_directory_save(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            const muzix_fs_directory_t *directory)
{
    uint8_t *data;
    uint8_t slot;

    if (!cache || !directory ||
        muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    memset(data, 0, MUZIX_FS_BLOCK_SIZE);
    for (slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        uint8_t *entry = &data[slot * MUZIX_FS_DIR_ENTRY_SIZE];
        muzix_fs_dir_put16(entry, directory->entries[slot].inode);
        memcpy(&entry[2], directory->entries[slot].name, MUZIX_FS_NAME_MAX);
    }
    return muzix_fs_cache_mark_dirty(cache, block);
}
