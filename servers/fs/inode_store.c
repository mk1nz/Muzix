#include "inode_store.h"

#include <string.h>

#define MUZIX_FS_INODE_DISK_SIZE 32u

static uint16_t get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

int muzix_fs_inode_table_load(muzix_fs_block_cache_t *cache,
                              uint16_t block,
                              muzix_fs_inode_table_t *table)
{
    uint8_t *data;
    uint8_t slot;

    if (!cache || !table || muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    muzix_fs_inode_table_init(table);
    for (slot = 0; slot < MUZIX_FS_INODE_SLOTS; slot++) {
        uint8_t *entry = &data[slot * MUZIX_FS_INODE_DISK_SIZE];
        muzix_fs_inode_t *inode = &table->entries[slot];
        inode->mode = get16(&entry[0]);
        inode->uid = get16(&entry[2]);
        inode->size = get32(&entry[4]);
        inode->gid = get16(&entry[8]);
        inode->links = entry[10];
        for (uint8_t zone = 0; zone < MUZIX_FS_DIRECT_ZONES; zone++) {
            inode->zone[zone] = get16(&entry[11 + zone * 2]);
        }
        inode->indirect_zone = get16(&entry[25]);
        inode->used = entry[27];
    }
    return 0;
}

int muzix_fs_inode_table_save(muzix_fs_block_cache_t *cache,
                              uint16_t block,
                              const muzix_fs_inode_table_t *table)
{
    uint8_t *data;
    uint8_t slot;

    if (!cache || !table || muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    memset(data, 0, MUZIX_FS_BLOCK_SIZE);
    for (slot = 0; slot < MUZIX_FS_INODE_SLOTS; slot++) {
        const muzix_fs_inode_t *inode = &table->entries[slot];
        uint8_t *entry = &data[slot * MUZIX_FS_INODE_DISK_SIZE];
        put16(&entry[0], inode->mode);
        put16(&entry[2], inode->uid);
        put32(&entry[4], inode->size);
        put16(&entry[8], inode->gid);
        entry[10] = inode->links;
        for (uint8_t zone = 0; zone < MUZIX_FS_DIRECT_ZONES; zone++) {
            put16(&entry[11 + zone * 2], inode->zone[zone]);
        }
        put16(&entry[25], inode->indirect_zone);
        entry[27] = inode->used;
    }
    return muzix_fs_cache_mark_dirty(cache, block);
}
