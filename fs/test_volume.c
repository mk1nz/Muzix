#include "volume.h"
#include "path.h"

static uint8_t disk[MUZIX_FS_BLOCK_SIZE * 16];

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

static int test_volume(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 16};
    muzix_fs_volume_t volume;
    muzix_fs_volume_t mounted;
    uint16_t inode;

    muzix_fs_volume_init(&volume, &device);
    if (muzix_fs_volume_format(&volume, 8, 32, 5) == 0 ||
        muzix_fs_volume_format(&volume, 8, 16, 4) == 0) {
        return 1;
    }
    if (muzix_fs_volume_format(&volume, 8, 16, 5) != 0) {
        return 2;
    }

    muzix_fs_volume_init(&mounted, &device);
    if (muzix_fs_volume_mount(&mounted) != 0 || !mounted.mounted ||
        muzix_fs_directory_lookup(&mounted.root_directory, ".", &inode) != 0 ||
        inode != 1 || mounted.inodes.entries[1].mode != 0x4000u) {
        return 3;
    }
    disk[MUZIX_FS_BLOCK_SIZE + 6] = 4;
    disk[MUZIX_FS_BLOCK_SIZE + 7] = 0;
    muzix_fs_volume_init(&mounted, &device);
    if (muzix_fs_volume_mount(&mounted) == 0) {
        return 4;
    }
    return 0;
}

int main(void)
{
    return test_volume();
}
