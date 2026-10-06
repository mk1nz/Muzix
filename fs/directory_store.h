#ifndef MUZIX_FS_DIRECTORY_STORE_H
#define MUZIX_FS_DIRECTORY_STORE_H

#include "block_cache.h"
#include "directory.h"

int muzix_fs_directory_load(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            muzix_fs_directory_t *directory);
int muzix_fs_directory_save(muzix_fs_block_cache_t *cache,
                            uint16_t block,
                            const muzix_fs_directory_t *directory);

#endif
