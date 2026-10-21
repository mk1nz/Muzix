#include "inode_store.h"

#include <string.h>

/* The on-disk inode record, in the order tools/make_minixfs.rb writes it and
 * kernel/exec_loader.c reads it back (MINIX-v1 d_inode1).  Thirty-two bytes:
 *
 *   0   mode      u16      4   size      u32      8   time   u32
 *   12  gid       u8       13  nlinks    u8       14  zone[7] u16 x 7
 *   28  indirect  u16      30  used      u8       31  spare
 *
 * The library's own `used` marker lives at 30: the image tool's inode writer
 * stops at the indirect pointer and leaves 30 and 31 zero, so a byte there
 * costs nothing on a tool-shaped image and is what a library-written image
 * carries.  It used to be read from 27, which is the high half of zone[6]: on
 * the real image that is a zone number, so an inode whose sixth direct zone was
 * non-zero was reported in use and one whose sixth zone was zero was reported
 * free, root included.  The in-memory record (fs_types.h) is unchanged; only
 * where each field sits on disk moved. */
#define MUZIX_FS_INODE_DISK_SIZE 32u
#define MUZIX_FS_INODE_MODE_OFFSET 0u
#define MUZIX_FS_INODE_UID_OFFSET 2u
#define MUZIX_FS_INODE_SIZE_OFFSET 4u
#define MUZIX_FS_INODE_GID_OFFSET 12u
#define MUZIX_FS_INODE_NLINKS_OFFSET 13u
#define MUZIX_FS_INODE_ZONE_OFFSET 14u
#define MUZIX_FS_INODE_INDIRECT_OFFSET 28u
#define MUZIX_FS_INODE_USED_OFFSET 30u

static uint16_t get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

int muzix_fs_inode_table_load(muzix_fs_block_cache_t *cache,
                               uint16_t block,
                               muzix_fs_inode_table_t *table)
{
    uint8_t *data;
    uint8_t slot;

    if (!cache || !table || muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    muzix_fs_inode_table_init(table);
    for (slot = 0; slot < MUZIX_FS_INODE_SLOTS; slot++) {
        uint8_t *entry = &data[slot * MUZIX_FS_INODE_DISK_SIZE];
        muzix_fs_inode_t *inode = &table->entries[slot];
        inode->mode = get16(&entry[MUZIX_FS_INODE_MODE_OFFSET]);
        inode->uid = get16(&entry[MUZIX_FS_INODE_UID_OFFSET]);
        inode->size = get32(&entry[MUZIX_FS_INODE_SIZE_OFFSET]);
        /* entry[8..11] is the modification time.  The in-memory record has no
         * field for it, so it is read over rather than stored. */
        inode->gid = entry[MUZIX_FS_INODE_GID_OFFSET];
        inode->links = entry[MUZIX_FS_INODE_NLINKS_OFFSET];
        for (uint8_t zone = 0; zone < MUZIX_FS_DIRECT_ZONES; zone++) {
            inode->zone[zone] =
                get16(&entry[MUZIX_FS_INODE_ZONE_OFFSET + zone * 2]);
        }
        inode->indirect_zone =
            get16(&entry[MUZIX_FS_INODE_INDIRECT_OFFSET]);
        /* A record the library wrote carries the marker at 30; a record the
         * image tool wrote leaves it zero, because its inode writer has no such
         * field, and has to be recognised by its mode instead.  A mode of zero
         * is what an unwritten slot looks like on both sides, so a free slot
         * stays free and the root (0x4000) and every file (0x8000|perms) are in
         * use whichever side wrote the image. */
        inode->used = (uint8_t)(entry[MUZIX_FS_INODE_USED_OFFSET] |
                                (inode->mode != 0 ? 1u : 0u));
    }
    return 0;
}

int muzix_fs_inode_table_save(muzix_fs_block_cache_t *cache,
                               uint16_t block,
                               const muzix_fs_inode_table_t *table)
{
    uint8_t *data;
    uint8_t slot;

    if (!cache || !table || muzix_fs_cache_get(cache, block, &data) != 0) {
        return -1;
    }

    memset(data, 0, MUZIX_FS_BLOCK_SIZE);
    for (slot = 0; slot < MUZIX_FS_INODE_SLOTS; slot++) {
        const muzix_fs_inode_t *inode = &table->entries[slot];
        uint8_t *entry = &data[slot * MUZIX_FS_INODE_DISK_SIZE];
        put16(&entry[MUZIX_FS_INODE_MODE_OFFSET], inode->mode);
        put16(&entry[MUZIX_FS_INODE_UID_OFFSET], inode->uid);
        put32(&entry[MUZIX_FS_INODE_SIZE_OFFSET], inode->size);
        /* 8..11 is the modification time and stays zero, which is what
         * make_minixfs.rb writes for it. */
        entry[MUZIX_FS_INODE_GID_OFFSET] = (uint8_t)inode->gid;
        entry[MUZIX_FS_INODE_NLINKS_OFFSET] = inode->links;
        for (uint8_t zone = 0; zone < MUZIX_FS_DIRECT_ZONES; zone++) {
            put16(&entry[MUZIX_FS_INODE_ZONE_OFFSET + zone * 2],
                  inode->zone[zone]);
        }
        put16(&entry[MUZIX_FS_INODE_INDIRECT_OFFSET], inode->indirect_zone);
        entry[MUZIX_FS_INODE_USED_OFFSET] = inode->used;
    }
    return muzix_fs_cache_mark_dirty(cache, block);
}
