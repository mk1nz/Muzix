#include "superblock.h"

static uint8_t disk[MUZIX_FS_BLOCK_SIZE * 4];

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

static int test_superblock(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 4};
    muzix_fs_block_cache_t cache;
    muzix_fs_superblock_state_t state;
    muzix_fs_superblock_state_t loaded;

    muzix_fs_cache_init(&cache, &device);
    muzix_fs_superblock_init(&state, &cache);
    if (muzix_fs_superblock_format(&state, 32, 256, 4) != 0 ||
        muzix_fs_cache_flush(&cache) != 0 ||
        !muzix_fs_superblock_valid(&state)) {
        return 1;
    }

    muzix_fs_superblock_init(&loaded, &cache);
    if (muzix_fs_superblock_load(&loaded) != 0 ||
        loaded.value.ninodes != 32 || loaded.value.first_data_zone != 4) {
        return 2;
    }
    return 0;
}

int main(void)
{
    return test_superblock();
}
