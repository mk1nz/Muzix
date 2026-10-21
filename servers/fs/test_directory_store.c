#include "directory_store.h"

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

static int test_directory_store(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 2};
    muzix_fs_block_cache_t cache;
    muzix_fs_directory_t source;
    muzix_fs_directory_t loaded;
    uint16_t inode;

    muzix_fs_cache_init(&cache, &device);
    muzix_fs_directory_init(&source);
    if (muzix_fs_directory_add(&source, 2, "etc") != 0 ||
        muzix_fs_directory_save(&cache, 1, &source) != 0 ||
        muzix_fs_cache_flush(&cache) != 0) {
        return 1;
    }

    muzix_fs_cache_init(&cache, &device);
    if (muzix_fs_directory_load(&cache, 1, &loaded) != 0 ||
        muzix_fs_directory_lookup(&loaded, "etc", &inode) != 0 || inode != 2) {
        return 2;
    }
    return 0;
}

int main(void)
{
    return test_directory_store();
}
