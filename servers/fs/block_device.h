#ifndef MUZIX_BLOCK_DEVICE_H
#define MUZIX_BLOCK_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#define MUZIX_FS_BLOCK_SIZE 512u

typedef struct {
    uint16_t block_count;
    uint8_t is_rom;
} muzix_block_device_t;

int muzix_block_device_read(muzix_block_device_t *device,
                            uint16_t block,
                            uint8_t *buffer,
                            size_t length);
int muzix_block_device_write(muzix_block_device_t *device,
                             uint16_t block,
                             const uint8_t *buffer,
                             size_t length);
void muzix_block_device_init_rom(muzix_block_device_t *device, uint16_t block_count);

#endif
