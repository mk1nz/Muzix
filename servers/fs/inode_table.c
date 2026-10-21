#include "inode_table.h"

#include <string.h>

void muzix_fs_inode_table_init(muzix_fs_inode_table_t *table)
{
    if (table) {
        memset(table, 0, sizeof(*table));
    }
}

int muzix_fs_inode_alloc(muzix_fs_inode_table_t *table, uint16_t *inode_nr)
{
    uint8_t slot;

    if (!table || !inode_nr) {
        return -1;
    }

    for (slot = 1; slot < MUZIX_FS_INODE_SLOTS; slot++) {
        if (!table->entries[slot].used) {
            memset(&table->entries[slot], 0, sizeof(table->entries[slot]));
            table->entries[slot].used = 1;
            table->entries[slot].links = 1;
            *inode_nr = slot;
            return 0;
        }
    }
    return -1;
}

int muzix_fs_inode_release(muzix_fs_inode_table_t *table, uint16_t inode_nr)
{
    if (!table || inode_nr == 0 || inode_nr >= MUZIX_FS_INODE_SLOTS ||
        !table->entries[inode_nr].used) {
        return -1;
    }

    memset(&table->entries[inode_nr], 0, sizeof(table->entries[inode_nr]));
    return 0;
}

muzix_fs_inode_t *muzix_fs_inode_get(muzix_fs_inode_table_t *table,
                                     uint16_t inode_nr)
{
    if (!table || inode_nr >= MUZIX_FS_INODE_SLOTS ||
        !table->entries[inode_nr].used) {
        return 0;
    }

    return &table->entries[inode_nr];
}
