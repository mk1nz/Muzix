#include "block_device.h"
#include "../kernel/rom_read.h"

/* The read and the write used to be asymmetric, and the read was the wrong one.
 *
 * muzix_block_device_write() has always called device->write().  The read
 * called rom_read_block() unconditionally and never looked at device->read at
 * all - so the read function pointer in muzix_block_device_t was dead, and the
 * only device that could be read was the one rom_read_block() happens to know
 * about.  Any other device was silently unreadable: no error, just an -1 out of
 * the cache load and a volume that would not mount.
 *
 * The board never showed it, because the ROMFS device is the only one that
 * exists: kernel_main.c sets g_block_dev.context to the MM service and its
 * block_count, and leaves read NULL, so the ROM path was reached by the accident
 * of a NULL pointer rather than by a decision.
 *
 * Measured, not reasoned: seven of the fs tests had never been executed and all
 * seven failed on this.  They fail at their first muzix_fs_cache_get(), because
 * cache_load() goes through here, and every one of them supplies its own read
 * function over a static array - which is the entire point of a block device
 * abstraction.  Adding the dispatch below turns five of them green
 * (fs/test_directory_store, fs/test_inode_store, fs/test_storage_core,
 * fs/test_superblock, fs/test_volume) and takes the other two past the first
 * assertion they used to die on.
 *
 * A device with no read function is still read from ROM, because that is the
 * device the kernel builds.  The asymmetry with the write path - where a missing
 * write function is refused - is left alone deliberately: kernel_main.c would
 * have to start assigning a function pointer to get the symmetry, and the ROM
 * read has to be reached with the MM in hand, which is what context is for.  The
 * better fix is g_block_dev.read = rom_read_block plus a refusal when read is
 * NULL, and that is a change to the boot path, not to a dispatch. */

int muzix_block_device_read(muzix_block_device_t *device,
                            uint16_t block,
                            uint8_t *buffer,
                            size_t length)
{
    if (!device || !buffer || length != MUZIX_FS_BLOCK_SIZE ||
        block >= device->block_count) {
        return -1;
    }

    if (device->read) {
        return device->read(device, block, buffer);
    }

    return rom_read_block(device, block, buffer);
}

int muzix_block_device_write(muzix_block_device_t *device,
                             uint16_t block,
                             const uint8_t *buffer,
                             size_t length)
{
    if (!device || !device->write || !buffer || length != MUZIX_FS_BLOCK_SIZE ||
        block >= device->block_count) {
        return -1;
    }

    return device->write(device, block, buffer);
}
