#ifndef MUZIX_FS_INODE_STORE_H
#define MUZIX_FS_INODE_STORE_H

#include "block_cache.h"
#include "inode_table.h"

int muzix_fs_inode_table_load(muzix_fs_block_cache_t *cache,
                              uint16_t block,
                              muzix_fs_inode_table_t *table);
int muzix_fs_inode_table_save(muzix_fs_block_cache_t *cache,
                              uint16_t block,
                              const muzix_fs_inode_table_t *table);

#endif
