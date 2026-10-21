#ifndef MUZIX_FS_PATH_H
#define MUZIX_FS_PATH_H

#include <stdint.h>

typedef int (*muzix_fs_path_component_lookup_fn)(void *context,
                                                 uint16_t directory_inode,
                                                 const char *name,
                                                 uint16_t *inode);

int muzix_fs_path_lookup(void *context,
                         uint16_t root_inode,
                         const char *path,
                         muzix_fs_path_component_lookup_fn lookup,
                         uint16_t *inode);

#endif
