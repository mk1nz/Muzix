#include "path.h"

#include <string.h>

#define MUZIX_FS_PATH_COMPONENT_MAX 14u

int muzix_fs_path_lookup(void *context,
                         uint16_t root_inode,
                         const char *path,
                         muzix_fs_path_component_lookup_fn lookup,
                         uint16_t *inode)
{
    char component[MUZIX_FS_PATH_COMPONENT_MAX + 1];
    uint16_t current;
    uint16_t next;
    uint8_t length;

    if (!path || !lookup || !inode || root_inode == 0) {
        return -1;
    }

    current = root_inode;
    while (*path == '/') {
        path++;
    }
    if (!*path) {
        *inode = current;
        return 0;
    }

    while (*path) {
        length = 0;
        while (*path && *path != '/') {
            /* A component of MUZIX_FS_PATH_COMPONENT_MAX characters is
             * storable: a directory entry keeps 14 name bytes, so 14 is the
             * longest name the filesystem has room for.  Rejecting at 13 made
             * a 14-character name openable and visible in ls but unresolvable,
             * so the file could be neither created nor removed through a path
             * that named it. */
            if (length > MUZIX_FS_PATH_COMPONENT_MAX) {
                return -1;
            }
            component[length++] = *path++;
        }
        component[length] = '\0';
        while (*path == '/') {
            path++;
        }
        if (length == 0 || lookup(context, current, component, &next) != 0) {
            return -1;
        }
        current = next;
    }

    *inode = current;
    return 0;
}
