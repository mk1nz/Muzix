#include "volume.h"

#include <string.h>
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/trace.h"

#define MUZIX_FS_ALLOCATOR_BLOCK 2u
#define MUZIX_FS_INODE_BLOCK 3u
#define MUZIX_FS_ROOT_DIRECTORY_BLOCK 4u
#define MUZIX_FS_ROOT_INODE 1u
#define MUZIX_FS_FIRST_DATA_BLOCK 5u

/* Formatting a volume is only ever done by tools/make_minixfs.rb, off-target,
 * and by the fs/test_* binaries. Nothing in a booted kernel calls it: the block
 * device is the ROM image and muzix_rom_block_write() returns -1, so a runtime
 * format could never persist anything. It is compiled out of the kernel link by
 * default because the kernel's C stack is the span above _DATA and the kernel is
 * 59 KB of a 64 KB address space - muzix_fs_volume_format together with
 * muzix_fs_superblock_format and its put16/put32 helpers cost about 1.5 KB,
 * which is more stack than the whole userspace syscall path needs. Set to 1 when
 * building the test binaries, which do exercise it. */
#ifndef MUZIX_ENABLE_FORMAT
#define MUZIX_ENABLE_FORMAT 0
#endif

void muzix_fs_volume_init(muzix_fs_volume_t *volume,
                          muzix_block_device_t *device)
{
    if (!volume) {
        return;
    }

    memset(volume, 0, sizeof(*volume));
    volume->device = device;
    muzix_fs_cache_init(&volume->cache, device);
    muzix_fs_superblock_init(&volume->superblock, &volume->cache);
}

int muzix_fs_volume_flush(muzix_fs_volume_t *volume)
{
    if (!volume || !volume->mounted) {
        return -1;
    }

    if (muzix_fs_allocator_save(&volume->cache, MUZIX_FS_ALLOCATOR_BLOCK,
                               &volume->allocator) != 0 ||
        muzix_fs_inode_table_save(&volume->cache, MUZIX_FS_INODE_BLOCK,
                                  &volume->inodes) != 0 ||
        muzix_fs_directory_save(&volume->cache, MUZIX_FS_ROOT_DIRECTORY_BLOCK,
                                &volume->root_directory) != 0) {
        return -1;
    }
    return muzix_fs_cache_flush(&volume->cache);
}

/* Reserve the metadata blocks (0 reserved, 1 superblock, 2 zone allocator,
 * 3 inode table, 4 root directory) in a freshly formatted zone bitmap.  The
 * image tool marks them too, and muzix_fs_allocator_load now insists on it.
 * Without it muzix_fs_zone_is_used(4) reported the root directory as free, and
 * only the fact that muzix_fs_zone_alloc starts at first_data_zone kept
 * anything from handing block 4 to a file.  The bit layout is the one
 * muzix_fs_zone_is_used reads, so it is written out here rather than through
 * the allocator, whose mark helper is private. */
static void muzix_fs_volume_reserve_metadata(muzix_fs_zone_allocator_t *allocator)
{
    uint16_t zone;

    for (zone = 0; zone < allocator->first_data_zone &&
                    zone < allocator->zone_count; zone++) {
        allocator->used[zone >> 3] |= (uint8_t)(1u << (zone & 7u));
    }
}

#if MUZIX_ENABLE_FORMAT
int muzix_fs_volume_format(muzix_fs_volume_t *volume,
                           uint16_t inode_count,
                           uint16_t zone_count,
                           uint16_t first_data_zone)
{
    muzix_fs_inode_t *root;

    if (!volume || !volume->device || inode_count > MUZIX_FS_INODE_SLOTS ||
        zone_count > volume->device->block_count ||
        first_data_zone < MUZIX_FS_FIRST_DATA_BLOCK ||
        muzix_fs_superblock_format(&volume->superblock, inode_count,
                                   zone_count, first_data_zone) != 0) {
        return -1;
    }

    muzix_fs_zone_allocator_init(&volume->allocator, zone_count, first_data_zone);
    muzix_fs_volume_reserve_metadata(&volume->allocator);
    muzix_fs_inode_table_init(&volume->inodes);
    volume->inodes.entries[MUZIX_FS_ROOT_INODE].used = 1;
    volume->inodes.entries[MUZIX_FS_ROOT_INODE].links = 2;
    volume->inodes.entries[MUZIX_FS_ROOT_INODE].mode = 0x4000u;
    muzix_fs_directory_init(&volume->root_directory);
    if (muzix_fs_directory_add(&volume->root_directory, MUZIX_FS_ROOT_INODE, ".") != 0 ||
        muzix_fs_directory_add(&volume->root_directory, MUZIX_FS_ROOT_INODE, "..") != 0) {
        return -1;
    }
    root = &volume->inodes.entries[MUZIX_FS_ROOT_INODE];
    root->zone[0] = MUZIX_FS_ROOT_DIRECTORY_BLOCK;
    volume->mounted = 1;
    return muzix_fs_volume_flush(volume);
}
#endif /* MUZIX_ENABLE_FORMAT */

int muzix_fs_volume_mount(muzix_fs_volume_t *volume)
{
    if (!volume || !volume->device) {
        return -1;
    }
    if (muzix_fs_superblock_load(&volume->superblock) != 0) {
        return -1;
    }
    if (volume->superblock.value.ninodes > MUZIX_FS_INODE_SLOTS ||
        volume->superblock.value.nzones > volume->device->block_count ||
        volume->superblock.value.first_data_zone < MUZIX_FS_FIRST_DATA_BLOCK) {
        return -1;
    }
;
    if (muzix_fs_allocator_load(&volume->cache, MUZIX_FS_ALLOCATOR_BLOCK,
                                &volume->allocator) != 0) {
        return -1;
    }
    /* Block 2 states its own geometry, and the superblock states another.
     * Trusting block 2 alone let a volume with a zone_count of 60 mount
     * "successfully" under a superblock claiming 66 zones: zones 60-65 became
     * unreachable, and every later write past zone 59 failed with no
     * diagnostic.  The two must describe the same filesystem. */
    if (volume->allocator.zone_count != volume->superblock.value.nzones ||
        volume->allocator.first_data_zone !=
            volume->superblock.value.first_data_zone) {
        return -1;
    }
;
    if (muzix_fs_inode_table_load(&volume->cache, MUZIX_FS_INODE_BLOCK,
                                  &volume->inodes) != 0) {
        return -1;
    }
;
    if (muzix_fs_directory_load(&volume->cache, MUZIX_FS_ROOT_DIRECTORY_BLOCK,
                                &volume->root_directory) != 0) {
        return -1;
    }

    volume->mounted = 1;
    return 0;
}
