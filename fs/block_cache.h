#ifndef MUZIX_BLOCK_CACHE_H
#define MUZIX_BLOCK_CACHE_H

#include <stdint.h>

#include "block_device.h"

/* The block cache is 516 bytes per slot and lives in the kernel's flat _DATA
 * segment, which must share the single 64 KiB address space with all of
 * _CODE.  Depth is therefore capped by space, not by preference.
 *
 * muzix_fs_cache_get evicts a clean slot (writing back a dirty one) when the
 * pool is full, so this depth is a hit-rate choice only.  It does not have to
 * be large enough to hold the mount-time blocks, unlike a pool with no
 * eviction, which silently wedges the first file read after mount. */
#define MUZIX_FS_CACHE_SLOTS 2u

typedef struct {
    uint8_t data[MUZIX_FS_BLOCK_SIZE];
    uint16_t block;
    uint8_t valid;
    uint8_t dirty;
} muzix_fs_cache_slot_t;

typedef struct {
    muzix_block_device_t *device;
    muzix_fs_cache_slot_t slots[MUZIX_FS_CACHE_SLOTS];
} muzix_fs_block_cache_t;

void muzix_fs_cache_init(muzix_fs_block_cache_t *cache,
                         muzix_block_device_t *device);
int muzix_fs_cache_get(muzix_fs_block_cache_t *cache,
                       uint16_t block,
                       uint8_t **data);
int muzix_fs_cache_mark_dirty(muzix_fs_block_cache_t *cache,
                              uint16_t block);
int muzix_fs_cache_flush(muzix_fs_block_cache_t *cache);

#endif
