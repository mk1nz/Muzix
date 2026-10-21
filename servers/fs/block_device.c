#include "block_device.h"
#include "fs_syscalls.h"

#define MUZIX_ROMFS_BASE_OFFSET  0x20000u
#define MUZIX_ROMFS_BLOCK_COUNT  768u

void muzix_block_device_init_rom(muzix_block_device_t *device, uint16_t block_count)
{
    if (!device) {
        return;
    }
    device->block_count = block_count ? block_count : MUZIX_ROMFS_BLOCK_COUNT;
    device->is_rom = 1;
}

int muzix_block_device_read(muzix_block_device_t *device,
                            uint16_t block,
                            uint8_t *buffer,
                            size_t length)
{
    if (!device || !buffer || length != MUZIX_FS_BLOCK_SIZE ||
        block >= device->block_count) {
        return -1;
    }

    if (device->is_rom) {
        return sys_rom_read(block, buffer);
    }

    return -1;
}

int muzix_block_device_write(muzix_block_device_t *device,
                             uint16_t block,
                             const uint8_t *buffer,
                             size_t length)
{
    (void)device;
    (void)block;
    (void)buffer;
    (void)length;
    return -1;
}