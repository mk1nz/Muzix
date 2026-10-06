#ifndef MUZIX_BLOCK_DEVICE_H
#define MUZIX_BLOCK_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#define MUZIX_FS_BLOCK_SIZE 512u

struct muzix_block_device;
typedef int (*muzix_block_read_fn)(struct muzix_block_device *device,
                                   uint16_t block,
                                   uint8_t *buffer);
typedef int (*muzix_block_write_fn)(struct muzix_block_device *device,
                                    uint16_t block,
                                    const uint8_t *buffer);

typedef struct muzix_block_device {
    void *context;
    muzix_block_read_fn read;
    muzix_block_write_fn write;
    uint16_t block_count;
} muzix_block_device_t;

int muzix_block_device_read(muzix_block_device_t *device,
                            uint16_t block,
                            uint8_t *buffer,
                            size_t length);
int muzix_block_device_write(muzix_block_device_t *device,
                             uint16_t block,
                             const uint8_t *buffer,
                             size_t length);

#endif
