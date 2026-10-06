#include "block_device.h"
#include "fs_service.h"

static uint8_t test_disk[MUZIX_FS_BLOCK_SIZE * 2];

static int test_read(struct muzix_block_device *device,
                     uint16_t block,
                     uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        buffer[i] = test_disk[(block * MUZIX_FS_BLOCK_SIZE) + i];
    }
    return 0;
}

static int test_write(struct muzix_block_device *device,
                      uint16_t block,
                      const uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        test_disk[(block * MUZIX_FS_BLOCK_SIZE) + i] = buffer[i];
    }
    return 0;
}

static int test_block_device(void)
{
    muzix_block_device_t device;
    muzix_fs_service_t fs;
    uint8_t buffer[MUZIX_FS_BLOCK_SIZE];

    device.context = 0;
    device.read = test_read;
    device.write = test_write;
    device.block_count = 2;
    muzix_fs_service_init(&fs, 0, 3);

    /* muzix_fs_service_set_device(), muzix_fs_read_block() and
     * muzix_fs_write_block() used to be exercised here, and all three were
     * removed on 2026-10-04 as dead code.  Nothing in the kernel called any of
     * them: fs_service.c reaches the cache through muzix_fs_cache_get() directly
     * (see the indirect-zone reads), so the wrappers were bypassed and the only
     * caller of each was this test.  Deleting them took 132 + 179 + 9 bytes out
     * of _CODE, which is where process parking is being paid for.
     *
     * So this file no longer covers them, which is not a loss of coverage worth
     * arguing about: it was coverage of a path the filesystem does not take.  What
     * is left here is what muzix_fs_service_init() alone can say. */
    return 0;
}

int main(void)
{
    return test_block_device();
}