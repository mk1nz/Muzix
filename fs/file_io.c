#include "file_io.h"

#include <string.h>

static int muzix_fs_file_zone(muzix_fs_block_cache_t *cache,
                              const muzix_fs_inode_t *inode,
                              uint32_t position,
                              uint16_t *zone,
                              uint16_t *offset)
{
    uint32_t index;
    if (!inode || !zone || !offset) {
        return -1;
    }

    /* The index has to stay 32-bit.  Truncating it to 16 bits aliased a large
     * position back onto the first zones, so a file could be made to read or
     * write zone[0] through an offset far past its end; the same truncation in
     * the allocator turned lseek(0x2000000) + write() into an overwrite of
     * zone[0] and a 512 MiB size. */
    index = position / MUZIX_FS_BLOCK_SIZE;
    if (index >= MUZIX_FS_ZONE_SLOTS) {
        return -1;
    }
    if (index < MUZIX_FS_DIRECT_ZONES) {
        *zone = inode->zone[index];
    } else {
        uint8_t *indirect;
        uint16_t indirect_index = (uint16_t)(index - MUZIX_FS_DIRECT_ZONES);
        if (inode->indirect_zone == 0 ||
            muzix_fs_cache_get(cache, inode->indirect_zone, &indirect) != 0) {
            return -1;
        }
        *zone = (uint16_t)indirect[indirect_index * 2] |
                ((uint16_t)indirect[indirect_index * 2 + 1] << 8);
    }
    if (*zone == 0) {
        return -1;
    }

    *offset = (uint16_t)(position & 0x1FFu);
    return 0;
}

static int muzix_fs_file_zone_alloc(muzix_fs_block_cache_t *cache,
                                    muzix_fs_zone_allocator_t *allocator,
                                    muzix_fs_inode_t *inode,
                                    uint32_t position,
                                    uint16_t *zone,
                                    uint16_t *offset)
{
    uint32_t index = position / MUZIX_FS_BLOCK_SIZE;
    uint8_t *indirect;
    /* 32-bit block index: see muzix_fs_file_zone.  A position beyond the
     * addressable set is refused outright instead of wrapping onto zone[0]. */
    if (index >= MUZIX_FS_ZONE_SLOTS) {
        return -1;
    }
    if (index < MUZIX_FS_DIRECT_ZONES) {
        if (inode->zone[index] == 0 &&
            muzix_fs_zone_alloc(allocator, &inode->zone[index]) != 0) {
            return -1;
        }
        *zone = inode->zone[index];
    } else {
        uint16_t indirect_index = (uint16_t)(index - MUZIX_FS_DIRECT_ZONES);
        uint8_t indirect_was_allocated = 0;
        if (inode->indirect_zone == 0) {
            if (muzix_fs_zone_alloc(allocator, &inode->indirect_zone) != 0) {
                return -1;
            }
            indirect_was_allocated = 1;
        }
        if (muzix_fs_cache_get(cache, inode->indirect_zone, &indirect) != 0) {
            return -1;
        }
        if (indirect_was_allocated) {
            memset(indirect, 0, MUZIX_FS_BLOCK_SIZE);
            if (muzix_fs_cache_mark_dirty(cache, inode->indirect_zone) != 0) {
                return -1;
            }
        }
        *zone = (uint16_t)indirect[indirect_index * 2] |
                ((uint16_t)indirect[indirect_index * 2 + 1] << 8);
        if (*zone == 0) {
            if (muzix_fs_zone_alloc(allocator, zone) != 0) {
                return -1;
            }
            indirect[indirect_index * 2] = (uint8_t)*zone;
            indirect[indirect_index * 2 + 1] = (uint8_t)(*zone >> 8);
            if (muzix_fs_cache_mark_dirty(cache, inode->indirect_zone) != 0) {
                return -1;
            }
        }
    }

    *offset = (uint16_t)(position & 0x1FFu);
    return 0;
}

int muzix_fs_file_read(muzix_fs_block_cache_t *cache,
                        const muzix_fs_inode_t *inode,
                        uint32_t position,
                        uint8_t *buffer,
                        size_t length,
                        size_t *result)
{
    size_t done = 0;
    if (result) {
        *result = 0;
    }
    if (!cache || !inode || !buffer || !result || position >= inode->size) {
        return 0;
    }

    if (length > (size_t)(inode->size - position)) {
        length = (size_t)(inode->size - position);
    }

    while (done < length) {
        uint16_t zone;
        uint16_t offset;
        uint8_t *data;
        size_t chunk;
        int fz_rc = muzix_fs_file_zone(cache, inode, position + done, &zone, &offset);
        if (fz_rc != 0 ||
            muzix_fs_cache_get(cache, zone, &data) != 0) {
            return -1;
        }
        chunk = MUZIX_FS_BLOCK_SIZE - offset;
        if (chunk > length - done) {
            chunk = length - done;
        }
        memcpy(&buffer[done], &data[offset], chunk);

        done += chunk;
    }

    *result = done;
    return 0;
}

int muzix_fs_file_write(muzix_fs_block_cache_t *cache,
                        muzix_fs_inode_t *inode,
                        uint32_t position,
                        const uint8_t *buffer,
                        size_t length,
                        size_t *result)
{
    return muzix_fs_file_write_alloc(cache, 0, inode, position,
                                     buffer, length, result);
}

int muzix_fs_file_write_alloc(muzix_fs_block_cache_t *cache,
                              muzix_fs_zone_allocator_t *allocator,
                              muzix_fs_inode_t *inode,
                              uint32_t position,
                              const uint8_t *buffer,
                              size_t length,
                              size_t *result)
{
    size_t done = 0;
    if (result) {
        *result = 0;
    }
    if (!cache || !inode || !buffer || !result) {
        return -1;
    }

    while (done < length) {
        uint16_t zone;
        uint16_t offset;
        uint8_t *data;
        size_t chunk;
        if ((allocator ? muzix_fs_file_zone_alloc(cache, allocator, inode,
                               position + done, &zone, &offset)
                   : muzix_fs_file_zone(cache, inode, position + done,
                            &zone, &offset)) != 0 ||
            muzix_fs_cache_get(cache, zone, &data) != 0) {
            return -1;
        }
        chunk = MUZIX_FS_BLOCK_SIZE - offset;
        if (chunk > length - done) {
            chunk = length - done;
        }
        /* Mark the block dirty *before* the data goes in.  The order was the
         * other way round: the block was marked, and only then could the mark
         * fail, so a failure returned -1 with the caller's bytes already
         * written into the block and no dirty flag to say so.  The caller saw
         * a hard error while the data had silently landed in the file. */
        if (muzix_fs_cache_mark_dirty(cache, zone) != 0) {
            return -1;
        }
        memcpy(&data[offset], &buffer[done], chunk);
        done += chunk;
    }

    if (position + done > inode->size) {
        inode->size = position + done;
    }
    *result = done;
    return 0;
}
