#include "inode_store.h"

static uint8_t disk[MUZIX_FS_BLOCK_SIZE * 2];

static int read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        buffer[i] = disk[block * MUZIX_FS_BLOCK_SIZE + i];
    }
    return 0;
}

static int write_block(struct muzix_block_device *device, uint16_t block, const uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        disk[block * MUZIX_FS_BLOCK_SIZE + i] = buffer[i];
    }
    return 0;
}

static int test_inode_store(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 2};
    muzix_fs_block_cache_t cache;
    muzix_fs_inode_table_t source;
    muzix_fs_inode_table_t loaded;
    muzix_fs_inode_t *inode;
    uint16_t inode_nr;

    muzix_fs_cache_init(&cache, &device);
    muzix_fs_inode_table_init(&source);
    if (muzix_fs_inode_alloc(&source, &inode_nr) != 0) {
        return 1;
    }
    inode = muzix_fs_inode_get(&source, inode_nr);
    inode->mode = 0x4000u;
    inode->size = 1234u;
    inode->zone[0] = 7;
    if (muzix_fs_inode_table_save(&cache, 1, &source) != 0 ||
        muzix_fs_cache_flush(&cache) != 0) {
        return 2;
    }

    muzix_fs_cache_init(&cache, &device);
    if (muzix_fs_inode_table_load(&cache, 1, &loaded) != 0) {
        return 3;
    }
    inode = muzix_fs_inode_get(&loaded, inode_nr);
    if (!inode || inode->mode != 0x4000u || inode->size != 1234u || inode->zone[0] != 7) {
        return 4;
    }
    return 0;
}

int main(void)
{
    return test_inode_store();
}
