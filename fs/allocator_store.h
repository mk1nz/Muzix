#ifndef MUZIX_FS_ALLOCATOR_STORE_H
#define MUZIX_FS_ALLOCATOR_STORE_H

#include "allocator.h"
#include "block_cache.h"

int muzix_fs_allocator_load(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            muzix_fs_zone_allocator_t *allocator);
int muzix_fs_allocator_save(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            const muzix_fs_zone_allocator_t *allocator);

#endif
