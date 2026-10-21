#include "block_cache.h"
#include "inode_table.h"

static uint8_t disk[MUZIX_FS_BLOCK_SIZE * 8];

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

static int test_storage_core(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 8};
    muzix_fs_block_cache_t cache;
    muzix_fs_inode_table_t inodes;
    muzix_fs_inode_t *inode;
    uint8_t *data;
    uint16_t inode_nr;

    muzix_fs_cache_init(&cache, &device);
    if (muzix_fs_cache_get(&cache, 1, &data) != 0) {
        return 1;
    }
    data[0] = 0x5a;
    if (muzix_fs_cache_mark_dirty(&cache, 1) != 0 ||
        muzix_fs_cache_flush(&cache) != 0 || disk[MUZIX_FS_BLOCK_SIZE] != 0x5a) {
        return 2;
    }

    muzix_fs_inode_table_init(&inodes);
    if (muzix_fs_inode_alloc(&inodes, &inode_nr) != 0) {
        return 3;
    }
    inode = muzix_fs_inode_get(&inodes, inode_nr);
    if (!inode || inode->links != 1 || muzix_fs_inode_release(&inodes, inode_nr) != 0) {
        return 4;
    }
    return 0;
}

int main(void)
{
    return test_storage_core();
}
