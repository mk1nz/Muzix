#ifndef MUZIX_FS_ALLOCATOR_H
#define MUZIX_FS_ALLOCATOR_H

#include <stdint.h>

/*
 * Zone ceiling, and it is the device's, not a preference: 384 KiB of ROMFS at
 * 512 bytes a block is 768 zones, which is what tools/make_minixfs.rb's
 * MAX_ZONES says and what kernel/rom_read.h's MUZIX_ROMFS_BLOCK_COUNT
 * configures the ROM block device for.  This was 256 - one eighth of the
 * device - so muzix_fs_zone_allocator_init() silently clamped a 768-zone image
 * to 256 and muzix_fs_allocator_load() refused the volume outright: zones
 * 256..767 had no bit to live in and the real filesystem would not mount.  The
 * zone count is a 16-bit field on disk, so 768 is well inside what the format
 * can describe.
 *
 * The bitmap is therefore 96 bytes and sits at offset 4 of the allocator
 * block, which is where make_minixfs.rb writes it.
 */
#define MUZIX_FS_MAX_ZONES 768u

typedef struct {
    uint16_t first_data_zone;
    uint16_t zone_count;
    uint8_t used[MUZIX_FS_MAX_ZONES / 8u];
} muzix_fs_zone_allocator_t;

void muzix_fs_zone_allocator_init(muzix_fs_zone_allocator_t *allocator,
                                  uint16_t zone_count,
                                  uint16_t first_data_zone);
int muzix_fs_zone_alloc(muzix_fs_zone_allocator_t *allocator,
                        uint16_t *zone);
int muzix_fs_zone_free(muzix_fs_zone_allocator_t *allocator,
                       uint16_t zone);
int muzix_fs_zone_is_used(const muzix_fs_zone_allocator_t *allocator,
                          uint16_t zone);

#endif
