#include "block_cache.h"

#include <string.h>
#include "../platform/zeta-v2/trace.h"

void muzix_fs_cache_init(muzix_fs_block_cache_t *cache,
                         muzix_block_device_t *device)
{
    if (!cache) {
        return;
    }

    memset(cache, 0, sizeof(*cache));
    cache->device = device;
}

/* Write back one slot if it has been modified.
 *
 * The backing store is the ROMFS image in ROM, so there is no write path at
 * all: muzix_block_device_write always fails for this device. Returning -1
 * here used to wedge the cache permanently -- the victim is chosen as the
 * lowest-numbered clean slot, so with every slot dirty the same one was picked
 * forever, its write-back kept failing, and every later cache_get for a
 * non-resident block failed too. The root directory, inode table, zone
 * allocator and all file data became unreadable after the first flush.
 *
 * So an unwritable block is simply dropped: the modification is lost, which is
 * the honest outcome for a read-only filesystem, and the slot is freed for
 * reuse instead of poisoning the pool. */
static int cache_write_back(muzix_fs_block_cache_t *cache, uint8_t slot)
{
    if (!cache->slots[slot].valid || !cache->slots[slot].dirty) {
        return 0;
    }
    if (muzix_block_device_write(cache->device, cache->slots[slot].block,
                                 cache->slots[slot].data,
                                 MUZIX_FS_BLOCK_SIZE) != 0) {
        cache->slots[slot].valid = 0;
        cache->slots[slot].dirty = 0;
        return 0;
    }
    cache->slots[slot].dirty = 0;
    return 0;
}

/* Load `block` into `slot`, writing back whatever that slot held. */
static int cache_load(muzix_fs_block_cache_t *cache, uint8_t slot,
                      uint16_t block)
{
    if (cache_write_back(cache, slot) != 0) {
        return -1;
    }
    if (muzix_block_device_read(cache->device, block, cache->slots[slot].data,
                                MUZIX_FS_BLOCK_SIZE) != 0) {
        return -1;
    }
    cache->slots[slot].block = block;
    cache->slots[slot].valid = 1;
    cache->slots[slot].dirty = 0;
    return 0;
}

int muzix_fs_cache_get(muzix_fs_block_cache_t *cache,
                       uint16_t block,
                       uint8_t **data)
{
    uint8_t slot;
    uint8_t victim;

    if (!cache || !cache->device || !data) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_CACHE_SLOTS; slot++) {
        if (cache->slots[slot].valid && cache->slots[slot].block == block) {
            *data = cache->slots[slot].data;
            return 0;
        }
    }

    /* Prefer a slot that was never used. */
    for (slot = 0; slot < MUZIX_FS_CACHE_SLOTS; slot++) {
        if (!cache->slots[slot].valid) {
            if (cache_load(cache, slot, block) != 0) {
                return -1;
            }
            *data = cache->slots[slot].data;
            return 0;
        }
    }

    /* Pool is full. Every slot is still marked valid forever unless something
     * replaces it, and nothing does unless we do it here: without this the
     * blocks touched while mounting (superblock, zone allocator, inode table,
     * root directory) consume every slot and all later file reads fail.
     *
     * Eviction is safe because no caller holds a slot pointer across a second
     * cache_get -- every one copies out or consumes the block before
     * returning, and muzix_fs_cache_mark_dirty never reloads. Prefer a clean
     * victim so we only pay a write-back when we have to. */
    victim = 0;
    for (slot = 0; slot < MUZIX_FS_CACHE_SLOTS; slot++) {
        if (!cache->slots[slot].dirty) {
            victim = slot;
            break;
        }
    }

    if (cache_load(cache, victim, block) != 0) {
        return -1;
    }
    *data = cache->slots[victim].data;
    return 0;
}

int muzix_fs_cache_mark_dirty(muzix_fs_block_cache_t *cache,
                              uint16_t block)
{
    uint8_t slot;

    if (!cache) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_CACHE_SLOTS; slot++) {
        if (cache->slots[slot].valid && cache->slots[slot].block == block) {
            cache->slots[slot].dirty = 1;
            return 0;
        }
    }
    return -1;
}

int muzix_fs_cache_flush(muzix_fs_block_cache_t *cache)
{
    uint8_t slot;

    if (!cache || !cache->device) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_CACHE_SLOTS; slot++) {
        if (cache->slots[slot].valid && cache->slots[slot].dirty) {
            if (muzix_block_device_write(cache->device,
                                         cache->slots[slot].block,
                                         cache->slots[slot].data,
                                         MUZIX_FS_BLOCK_SIZE) != 0) {
                return -1;
            }
            cache->slots[slot].dirty = 0;
        }
    }
    return 0;
}
