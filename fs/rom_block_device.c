#include "block_device.h"

#include <stdint.h>

int muzix_rom_block_read(struct muzix_block_device *device, uint16_t block, uint8_t *buffer)
{
    const uint8_t *base;
    uint32_t offset;
    uint16_t i;

    if (!device || !buffer || !device->context ||
        block >= device->block_count) {
        return -1;
    }

    base = (const uint8_t *)device->context;
    offset = (uint32_t)block * MUZIX_FS_BLOCK_SIZE;

    for (i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        buffer[i] = base[offset + i];
    }
    return 0;
}

int muzix_rom_block_write(struct muzix_block_device *device, uint16_t block, const uint8_t *buffer)
{
    (void)device;
    (void)block;
    (void)buffer;
    return -1;
}
