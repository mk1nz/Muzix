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
            /* Fourteen bytes is the whole slot, so a name of fourteen
             * characters is stored in full and carries no terminator - and
             * fourteen is what muzix_fs_name_valid accepts and what the image
             * tool writes.  Truncating to thirteen would put the name back out
             * of reach of the very operations that have to find it again (see
             * the note in fs_service.c's path normaliser), so the slot keeps
             * all fourteen bytes and every reader of a name compares it
             * bounded: muzix_fs_directory_lookup() and
             * muzix_fs_directory_remove() use strncmp(..., MUZIX_FS_NAME_MAX)
             * and the rename ".." search does the same.  Nothing may call
             * strcmp, strlen or strcpy on an entry name. */
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
