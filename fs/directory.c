#include "directory.h"

#include <string.h>

static int muzix_fs_name_valid(const char *name)
{
    size_t length;

    if (!name || !name[0]) {
        return 0;
    }

    length = strlen(name);
    return length <= MUZIX_FS_NAME_MAX;
}

void muzix_fs_directory_init(muzix_fs_directory_t *directory)
{
    if (directory) {
        memset(directory, 0, sizeof(*directory));
    }
}

int muzix_fs_directory_add(muzix_fs_directory_t *directory,
                           uint16_t inode,
                           const char *name)
{
    uint8_t slot;

    if (!directory || inode == 0 || !muzix_fs_name_valid(name)) {
        return -1;
    }

    if (muzix_fs_directory_lookup(directory, name, 0) >= 0) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        if (directory->entries[slot].inode == 0) {
            directory->entries[slot].inode = inode;
            memset(directory->entries[slot].name, 0, MUZIX_FS_NAME_MAX);
            memcpy(directory->entries[slot].name, name, strlen(name));
            return 0;
        }
    }
    return -1;
}

int muzix_fs_directory_lookup(const muzix_fs_directory_t *directory,
                              const char *name,
                              uint16_t *inode)
{
    uint8_t slot;

    if (!directory || !muzix_fs_name_valid(name)) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        if (directory->entries[slot].inode != 0 &&
            strncmp(directory->entries[slot].name, name, MUZIX_FS_NAME_MAX) == 0) {
            if (inode) {
                *inode = directory->entries[slot].inode;
            }
            return 0;
        }
    }
    return -1;
}

int muzix_fs_directory_remove(muzix_fs_directory_t *directory,
                              const char *name)
{
    uint8_t slot;

    if (!directory || !muzix_fs_name_valid(name)) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        if (directory->entries[slot].inode != 0 &&
            strncmp(directory->entries[slot].name, name, MUZIX_FS_NAME_MAX) == 0) {
            memset(&directory->entries[slot], 0, sizeof(directory->entries[slot]));
            return 0;
        }
    }
    return -1;
}
