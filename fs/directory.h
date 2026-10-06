#ifndef MUZIX_FS_DIRECTORY_H
#define MUZIX_FS_DIRECTORY_H

#include <stdint.h>

#include "fs_types.h"

#define MUZIX_FS_NAME_MAX 14u
#define MUZIX_FS_DIR_ENTRIES 16u

typedef struct {
    uint16_t inode;
    char name[MUZIX_FS_NAME_MAX];
} muzix_fs_dir_entry_t;

typedef struct {
    muzix_fs_dir_entry_t entries[MUZIX_FS_DIR_ENTRIES];
} muzix_fs_directory_t;

void muzix_fs_directory_init(muzix_fs_directory_t *directory);
int muzix_fs_directory_add(muzix_fs_directory_t *directory,
                           uint16_t inode,
                           const char *name);
int muzix_fs_directory_lookup(const muzix_fs_directory_t *directory,
                              const char *name,
                              uint16_t *inode);
int muzix_fs_directory_remove(muzix_fs_directory_t *directory,
                              const char *name);

#endif
