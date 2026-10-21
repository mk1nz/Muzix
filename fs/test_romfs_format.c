#include "block_cache.h"
#include "directory.h"
#include "file_io.h"
#include "inode_store.h"
#include "volume.h"

#include <string.h>

/* host-test-provides: rom_read_block */

/*
 * A tool-shaped MINIX-v1 image, mounted by the fs/ library.
 *
 * What this test is for
 * ---------------------
 * The fs/ storage library's on-disk format disagreed with the only producer of
 * an image there is, tools/make_minixfs.rb, and with the live kernel reader in
 * kernel/exec_loader.c.  The image below is written by hand, byte for byte, in
 * the order that script's writers use - a superblock with ninodes at 0, nzones
 * at 2, imap at 4, zmap at 5, first_datazone at 6, log at 8, maxsize at 9 and
 * the magic at 13; a 32-byte inode record with gid at 12, nlinks at 13,
 * zone[7] at 14 and the indirect pointer at 28; an inode table in blocks 3 and
 * 4; the root directory in block 5 and the first data zone at 6 - and then read
 * back through muzix_fs_volume_mount(), muzix_fs_inode_get(), the directory
 * reader and muzix_fs_file_read().  Nothing here calls the library's own
 * writer to build the image: the point is that the library reads an image it
 * did not make.
 *
 * Every field is written at its literal offset rather than through a constant
 * shared with the library, so a library that drifts from the format fails here
 * instead of agreeing with itself.  The offsets the library used to have were
 * gid@8, nlinks@10, zone[7]@11, indirect@25, a root directory at block 4 and a
 * first data zone of 5, and against this image they read nlinks as 0, zone[0]
 * as 0, the root directory as an empty block and a 768-zone volume as one the
 * allocator refuses.
 *
 * rom_read_block is provided here because fs/block_device.c calls it when a
 * device carries no read function of its own, and the module that defines it,
 * kernel/rom_read.c, reaches the memory manager's bank primitives, which the
 * host cannot link.  The device below always supplies `read`, so this
 * definition stands in for nothing but the symbol.
 */

/* ---- geometry, as tools/make_minixfs.rb sets it ------------------------ */

#define TEST_BLOCK_SIZE MUZIX_FS_BLOCK_SIZE
#define TEST_BLOCK_COUNT 24u
/* MAX_INODES: the image's inode capacity, two blocks of records. */
#define TEST_INODE_COUNT 18u
/* zone_count: the number of blocks the image occupies. */
#define TEST_ZONE_COUNT 17u
#define TEST_ALLOCATOR_BLOCK 2u
#define TEST_INODE_TABLE_BLOCK 3u
#define TEST_ROOT_DIRECTORY_BLOCK 5u
#define TEST_FIRST_DATA_ZONE 6u
#define TEST_SUPERBLOCK_BLOCK 1u
#define TEST_DIR_ENTRY_SIZE 16u
#define TEST_NAME_FIELD 14u
#define TEST_INODE_DISK_SIZE 32u

#define TEST_ROOT_INODE 1u
#define TEST_HELLO_INODE 2u
#define TEST_BIG_INODE 3u
#define TEST_HELLO_ZONE 6u
#define TEST_BIG_FIRST_ZONE 7u
#define TEST_BIG_DIRECT_ZONES 7u
#define TEST_BIG_INDIRECT_ZONES 2u
#define TEST_BIG_INDIRECT_BLOCK 16u
#define TEST_BIG_SIZE \
    ((uint32_t)(TEST_BIG_DIRECT_ZONES + TEST_BIG_INDIRECT_ZONES) * \
     (uint32_t)TEST_BLOCK_SIZE)

static const char test_hello_name[] = "hello.txt";
static const char test_big_name[] = "big.bin";
static const char test_hello_data[] = "hello muzix romfs\n";

static uint8_t disk[TEST_BLOCK_SIZE * TEST_BLOCK_COUNT];

/* One block's worth of records, checked at compile time.  MUZIX_FS_INODE_SLOTS
 * is how many in-memory records the library serialises, and sixteen of them are
 * exactly one 512-byte block.  Eighteen used to be the value, and 18 * 32 is
 * 576, so slots 16 and 17 were written past the end of the block and into the
 * block cache's slot metadata.  A wrong value here stops the build instead of
 * corrupting a data structure. */
typedef char muzix_fs_inode_slots_fill_one_block[
    (MUZIX_FS_INODE_SLOTS * TEST_INODE_DISK_SIZE == TEST_BLOCK_SIZE) ? 1 : -1];

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

static uint16_t get16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t get32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* ---- the image, written the way make_minixfs.rb writes it -------------- */

static void romfs_mark_zone(uint16_t zone)
{
    disk[TEST_ALLOCATOR_BLOCK * TEST_BLOCK_SIZE + 4u + zone / 8u] |=
        (uint8_t)(1u << (zone % 8u));
}

static void romfs_write_inode(uint16_t inode, uint16_t mode, uint32_t size,
                              uint8_t gid, uint8_t links, uint16_t indirect,
                              const uint16_t *zones, uint8_t zone_count)
{
    uint8_t *entry = &disk[TEST_INODE_TABLE_BLOCK * TEST_BLOCK_SIZE +
                           inode * TEST_INODE_DISK_SIZE];

    put16(&entry[0], mode);
    put16(&entry[2], 0);                  /* uid */
    put32(&entry[4], size);
    put32(&entry[8], 0);                  /* time */
    entry[12] = gid;
    entry[13] = links;
    for (uint8_t zone = 0; zone < zone_count; zone++) {
        put16(&entry[14 + zone * 2], zones[zone]);
    }
    put16(&entry[28], indirect);
    /* 30 and 31 stay zero, which is what the tool leaves them: its inode
     * writer has no field for either. */
}

static void romfs_write_dirent(uint16_t block, uint8_t slot, uint16_t inode,
                               const char *name)
{
    uint8_t *entry = &disk[block * TEST_BLOCK_SIZE + slot * TEST_DIR_ENTRY_SIZE];

    put16(&entry[0], inode);
    memset(&entry[2], 0, TEST_NAME_FIELD);
    memcpy(&entry[2], name, strlen(name));
}

/* A deterministic fill, so the long file's contents can be checked byte by
 * byte rather than against a copy of what was just written. */
static uint8_t romfs_big_byte(uint32_t position)
{
    uint16_t zone = (uint16_t)(position / TEST_BLOCK_SIZE);
    uint16_t within = (uint16_t)(position % TEST_BLOCK_SIZE);

    return (uint8_t)((zone * 37u) ^ (within * 11u) ^ 0xa5u);
}

static void romfs_build_image(void)
{
    uint16_t zones[MUZIX_FS_DIRECT_ZONES];
    uint8_t zone;

    memset(disk, 0, sizeof(disk));

    /* Block 1: superblock. */
    put16(&disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 0],
          TEST_INODE_COUNT);
    put16(&disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 2],
          TEST_ZONE_COUNT);
    disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 4] = 1;      /* imap */
    disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 5] = 1;      /* zmap */
    put16(&disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 6],
          TEST_FIRST_DATA_ZONE);
    disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 8] = 0;      /* log */
    put32(&disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 9],
          0xffffffffu);                                         /* max_size */
    put16(&disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 13], MUZIX_FS_MAGIC);

    /* Block 2: zone allocator - the metadata blocks first, then the data. */
    put16(&disk[TEST_ALLOCATOR_BLOCK * TEST_BLOCK_SIZE + 0],
          TEST_FIRST_DATA_ZONE);
    put16(&disk[TEST_ALLOCATOR_BLOCK * TEST_BLOCK_SIZE + 2], TEST_ZONE_COUNT);
    for (uint16_t reserved = 0; reserved < TEST_FIRST_DATA_ZONE; reserved++) {
        romfs_mark_zone(reserved);
    }
    romfs_mark_zone(TEST_HELLO_ZONE);
    for (zone = 0; zone < TEST_BIG_DIRECT_ZONES + TEST_BIG_INDIRECT_ZONES;
         zone++) {
        romfs_mark_zone((uint16_t)(TEST_BIG_FIRST_ZONE + zone));
    }
    romfs_mark_zone(TEST_BIG_INDIRECT_BLOCK);

    /* Block 6: the short file's data, then the long file's. */
    memcpy(&disk[TEST_HELLO_ZONE * TEST_BLOCK_SIZE], test_hello_data,
           sizeof(test_hello_data) - 1u);
    for (uint32_t position = 0; position < TEST_BIG_SIZE; position++) {
        disk[(TEST_BIG_FIRST_ZONE + position / TEST_BLOCK_SIZE) *
             TEST_BLOCK_SIZE + position % TEST_BLOCK_SIZE] =
            romfs_big_byte(position);
    }
    /* The remainder past the seven direct zones lives in the indirect block,
     * one 16-bit zone number per entry. */
    for (zone = 0; zone < TEST_BIG_INDIRECT_ZONES; zone++) {
        put16(&disk[TEST_BIG_INDIRECT_BLOCK * TEST_BLOCK_SIZE + zone * 2],
              (uint16_t)(TEST_BIG_FIRST_ZONE + TEST_BIG_DIRECT_ZONES + zone));
    }

    /* Blocks 3-4: the inode table.  Slot 1 is the root, 2 and 3 are files. */
    romfs_write_inode(TEST_ROOT_INODE, MUZIX_FS_MODE_DIR, 0, 0, 2, 0,
                      &(uint16_t){TEST_ROOT_DIRECTORY_BLOCK}, 1);
    romfs_write_inode(TEST_HELLO_INODE,
                      (uint16_t)(MUZIX_FS_MODE_REGULAR | 0777u),
                      (uint32_t)sizeof(test_hello_data) - 1u, 0, 1, 0,
                      &(uint16_t){TEST_HELLO_ZONE}, 1);
    for (zone = 0; zone < MUZIX_FS_DIRECT_ZONES; zone++) {
        zones[zone] = (uint16_t)(TEST_BIG_FIRST_ZONE + zone);
    }
    romfs_write_inode(TEST_BIG_INODE,
                      (uint16_t)(MUZIX_FS_MODE_REGULAR | 0777u),
                      TEST_BIG_SIZE, 0, 1, TEST_BIG_INDIRECT_BLOCK,
                      zones, MUZIX_FS_DIRECT_ZONES);

    /* Block 5: the root directory. */
    romfs_write_dirent(TEST_ROOT_DIRECTORY_BLOCK, 0, TEST_ROOT_INODE, ".");
    romfs_write_dirent(TEST_ROOT_DIRECTORY_BLOCK, 1, TEST_ROOT_INODE, "..");
    romfs_write_dirent(TEST_ROOT_DIRECTORY_BLOCK, 2, TEST_HELLO_INODE,
                       test_hello_name);
    romfs_write_dirent(TEST_ROOT_DIRECTORY_BLOCK, 3, TEST_BIG_INODE,
                       test_big_name);
}

/* ---- the device -------------------------------------------------------- */

static int read_block(struct muzix_block_device *device, uint16_t block,
                      uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        buffer[i] = disk[block * MUZIX_FS_BLOCK_SIZE + i];
    }
    return 0;
}

static int write_block(struct muzix_block_device *device, uint16_t block,
                       const uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        disk[block * MUZIX_FS_BLOCK_SIZE + i] = buffer[i];
    }
    return 0;
}

/* See the note at the top of the file: this exists to complete the link, and
 * the device below never reaches it. */
int rom_read_block(struct muzix_block_device *device, uint16_t block,
                   uint8_t *buffer);

int rom_read_block(struct muzix_block_device *device, uint16_t block,
                   uint8_t *buffer)
{
    (void)device;
    (void)block;
    (void)buffer;
    return -1;
}

static int romfs_mount(muzix_fs_volume_t *volume,
                       muzix_block_device_t *device)
{
    /* The device is the caller's, and has to outlive the volume: the cache
     * keeps the pointer rather than a copy. */
    muzix_fs_volume_init(volume, device);
    return muzix_fs_volume_mount(volume);
}

static void romfs_build_device(muzix_block_device_t *device)
{
    device->context = 0;
    device->read = read_block;
    device->write = write_block;
    device->block_count = TEST_BLOCK_COUNT;
}

/* ---- the assertions ---------------------------------------------------- */

static int test_superblock_geometry(muzix_fs_volume_t *volume)
{
    if (volume->superblock.value.ninodes != TEST_INODE_COUNT ||
        volume->superblock.value.nzones != TEST_ZONE_COUNT ||
        volume->superblock.value.imap_blocks != 1 ||
        volume->superblock.value.zmap_blocks != 1 ||
        volume->superblock.value.first_data_zone != TEST_FIRST_DATA_ZONE ||
        volume->superblock.value.log_zone_size != 0 ||
        volume->superblock.value.max_size != 0xffffffffu ||
        volume->superblock.value.magic != MUZIX_FS_MAGIC) {
        return 1;
    }
    /* The allocator block states the same geometry, or the volume is two
     * filesystems wearing one superblock. */
    if (volume->allocator.zone_count != TEST_ZONE_COUNT ||
        volume->allocator.first_data_zone != TEST_FIRST_DATA_ZONE) {
        return 2;
    }
    return 0;
}

static int test_root_inode(muzix_fs_volume_t *volume)
{
    muzix_fs_inode_t *root =
        muzix_fs_inode_get(&volume->inodes, TEST_ROOT_INODE);

    if (!root) {
        return 1;
    }
    if (root->mode != MUZIX_FS_MODE_DIR || root->size != 0 ||
        root->gid != 0 || root->links != 2 || root->indirect_zone != 0) {
        return 2;
    }
    /* zone[0] is the root directory block.  A library reading the zone list
     * from offset 11 sees whatever the time field left there instead. */
    if (root->zone[0] != TEST_ROOT_DIRECTORY_BLOCK) {
        return 3;
    }
    return 0;
}

static int test_file_inodes(muzix_fs_volume_t *volume)
{
    muzix_fs_inode_t *hello =
        muzix_fs_inode_get(&volume->inodes, TEST_HELLO_INODE);
    muzix_fs_inode_t *big = muzix_fs_inode_get(&volume->inodes, TEST_BIG_INODE);

    if (!hello || !big) {
        return 1;
    }
    if (hello->mode != (uint16_t)(MUZIX_FS_MODE_REGULAR | 0777u) ||
        hello->links != 1 || hello->gid != 0 ||
        hello->size != (uint32_t)sizeof(test_hello_data) - 1u ||
        hello->zone[0] != TEST_HELLO_ZONE || hello->indirect_zone != 0) {
        return 2;
    }
    if (big->mode != (uint16_t)(MUZIX_FS_MODE_REGULAR | 0777u) ||
        big->links != 1 || big->size != TEST_BIG_SIZE ||
        big->indirect_zone != TEST_BIG_INDIRECT_BLOCK) {
        return 3;
    }
    for (uint8_t zone = 0; zone < MUZIX_FS_DIRECT_ZONES; zone++) {
        if (big->zone[zone] != (uint16_t)(TEST_BIG_FIRST_ZONE + zone)) {
            return 4;
        }
    }
    return 0;
}

static int test_directory_entries(muzix_fs_volume_t *volume)
{
    uint16_t inode;

    if (muzix_fs_directory_lookup(&volume->root_directory, ".", &inode) != 0 ||
        inode != TEST_ROOT_INODE) {
        return 1;
    }
    if (muzix_fs_directory_lookup(&volume->root_directory, "..", &inode) != 0 ||
        inode != TEST_ROOT_INODE) {
        return 2;
    }
    /* The root directory lives in block 5.  A library looking at block 4 reads
     * the image's second inode-table block, which is zero here, and finds no
     * files at all. */
    if (muzix_fs_directory_lookup(&volume->root_directory, test_hello_name,
                                  &inode) != 0 ||
        inode != TEST_HELLO_INODE) {
        return 3;
    }
    if (muzix_fs_directory_lookup(&volume->root_directory, test_big_name,
                                  &inode) != 0 ||
        inode != TEST_BIG_INODE) {
        return 4;
    }
    return 0;
}

static int test_file_contents(muzix_fs_volume_t *volume)
{
    muzix_fs_inode_t *hello =
        muzix_fs_inode_get(&volume->inodes, TEST_HELLO_INODE);
    muzix_fs_inode_t *big = muzix_fs_inode_get(&volume->inodes, TEST_BIG_INODE);
    uint8_t buffer[TEST_BLOCK_SIZE * 2];
    size_t expected = sizeof(test_hello_data) - 1u;
    size_t done;

    if (!hello || !big) {
        return 1;
    }
    if (muzix_fs_file_read(&volume->cache, hello, 0, buffer, expected,
                           &done) != 0 ||
        done != expected || memcmp(buffer, test_hello_data, expected) != 0) {
        return 2;
    }

    /* The long file spans all seven direct zones and then the indirect block.
     * Reading it end to end is what proves the indirect pointer came from
     * offset 28 and not from 25, which is the middle of zone[5]. */
    for (uint32_t position = 0; position < TEST_BIG_SIZE;
         position += TEST_BLOCK_SIZE * 2u) {
        uint32_t chunk = TEST_BLOCK_SIZE * 2u;
        if (chunk > TEST_BIG_SIZE - position) {
            chunk = TEST_BIG_SIZE - position;
        }
        if (muzix_fs_file_read(&volume->cache, big, position, buffer, chunk,
                               &done) != 0 ||
            done != chunk) {
            return 3;
        }
        for (uint32_t offset = 0; offset < chunk; offset++) {
            if (buffer[offset] !=
                romfs_big_byte(position + offset)) {
                return 4;
            }
        }
    }
    return 0;
}

/* Write the table back out and check the bytes on disk land where the image
 * tool puts them.  This is the half that keeps a future offset change from
 * being introduced quietly: the library agrees with its own loader by
 * construction, so only the writer's output is checked against the tool. */
static int test_inode_round_trip(muzix_fs_volume_t *volume)
{
    muzix_fs_inode_t *root =
        muzix_fs_inode_get(&volume->inodes, TEST_ROOT_INODE);
    muzix_fs_inode_t *hello =
        muzix_fs_inode_get(&volume->inodes, TEST_HELLO_INODE);
    muzix_fs_inode_table_t reloaded;
    uint8_t *record;

    if (!root || !hello) {
        return 1;
    }
    root->gid = 7;
    root->links = 3;
    /* zone[6] is the last direct zone, and offset 27 is where the old library
     * kept its `used` byte: right in the middle of it. */
    root->zone[6] = 0x0055u;
    root->indirect_zone = 0x0066u;
    hello->indirect_zone = 0x0077u;
    if (muzix_fs_inode_table_save(&volume->cache, TEST_INODE_TABLE_BLOCK,
                                  &volume->inodes) != 0 ||
        muzix_fs_cache_flush(&volume->cache) != 0) {
        return 2;
    }

    record = &disk[TEST_INODE_TABLE_BLOCK * TEST_BLOCK_SIZE +
                   TEST_ROOT_INODE * TEST_INODE_DISK_SIZE];
    if (get16(&record[0]) != MUZIX_FS_MODE_DIR ||
        get16(&record[2]) != 0 ||
        get32(&record[4]) != 0 ||
        record[12] != 7 || record[13] != 3 ||
        get16(&record[14]) != TEST_ROOT_DIRECTORY_BLOCK ||
        get16(&record[26]) != 0x0055u ||
        get16(&record[28]) != 0x0066u ||
        record[30] == 0) {
        return 3;
    }
    record = &disk[TEST_INODE_TABLE_BLOCK * TEST_BLOCK_SIZE +
                   TEST_HELLO_INODE * TEST_INODE_DISK_SIZE];
    if (get16(&record[0]) != (uint16_t)(MUZIX_FS_MODE_REGULAR | 0777u) ||
        record[12] != 0 || record[13] != 1 ||
        get16(&record[14]) != TEST_HELLO_ZONE ||
        get16(&record[28]) != 0x0077u) {
        return 4;
    }
    /* Slot 0 is unwritten on the image and must stay free. */
    record = &disk[TEST_INODE_TABLE_BLOCK * TEST_BLOCK_SIZE];
    if (get16(&record[0]) != 0 || record[30] != 0) {
        return 5;
    }

    if (muzix_fs_inode_table_load(&volume->cache, TEST_INODE_TABLE_BLOCK,
                                  &reloaded) != 0) {
        return 6;
    }
    root = muzix_fs_inode_get(&reloaded, TEST_ROOT_INODE);
    hello = muzix_fs_inode_get(&reloaded, TEST_HELLO_INODE);
    if (!root || !hello || muzix_fs_inode_get(&reloaded, 0) != 0) {
        return 7;
    }
    if (root->gid != 7 || root->links != 3 || root->zone[6] != 0x0055u ||
        root->indirect_zone != 0x0066u || hello->indirect_zone != 0x0077u) {
        return 8;
    }
    return 0;
}

/* The zone ceiling is the device's: 384 KiB of ROMFS at 512 bytes a block is
 * 768 blocks, which is what tools/make_minixfs.rb's MAX_ZONES says and what
 * kernel/rom_read.h's MUZIX_ROMFS_BLOCK_COUNT configures the ROM block device
 * for.  A ceiling of 256 clamped every real image to a third of its size, and
 * muzix_fs_allocator_load() refused the volume outright.  The count below is
 * written out rather than taken from MUZIX_FS_MAX_ZONES, so that the library
 * is checked against the image instead of against itself. */
#define TEST_MAX_ZONES 768u

static int test_zone_ceiling(void)
{
    muzix_fs_zone_allocator_t allocator;
    muzix_fs_zone_allocator_t reloaded;
    muzix_fs_block_cache_t cache;
    muzix_block_device_t device;
    uint16_t zone;

    romfs_build_device(&device);
    muzix_fs_cache_init(&cache, &device);
    if (MUZIX_FS_MAX_ZONES != TEST_MAX_ZONES) {
        return 1;
    }
    muzix_fs_zone_allocator_init(&allocator, TEST_MAX_ZONES,
                                 TEST_FIRST_DATA_ZONE);
    if (allocator.zone_count != TEST_MAX_ZONES) {
        return 2;
    }
    /* The image tool reserves the metadata blocks before it marks any data
     * zone, and muzix_fs_allocator_load() insists on it. */
    for (uint16_t reserved = 0; reserved < TEST_FIRST_DATA_ZONE; reserved++) {
        allocator.used[reserved >> 3] |= (uint8_t)(1u << (reserved & 7u));
    }
    if (muzix_fs_zone_alloc(&allocator, &zone) != 0 ||
        zone != TEST_FIRST_DATA_ZONE) {
        return 3;
    }
    if (muzix_fs_allocator_save(&cache, TEST_ALLOCATOR_BLOCK,
                                &allocator) != 0 ||
        muzix_fs_cache_flush(&cache) != 0) {
        return 4;
    }
    if (muzix_fs_allocator_load(&cache, TEST_ALLOCATOR_BLOCK,
                                &reloaded) != 0 ||
        reloaded.zone_count != TEST_MAX_ZONES ||
        !muzix_fs_zone_is_used(&reloaded, TEST_FIRST_DATA_ZONE)) {
        return 5;
    }
    return 0;
}

/* Volumes the library has to refuse: a wrong magic, a first data zone inside
 * the metadata, and an allocator block that leaves a metadata block free. */
static int test_refused_volumes(void)
{
    muzix_fs_volume_t volume;
    muzix_block_device_t device;

    romfs_build_device(&device);
    romfs_build_image();
    disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 13] ^= 0xffu;
    if (romfs_mount(&volume, &device) == 0) {
        return 1;
    }

    romfs_build_image();
    put16(&disk[TEST_SUPERBLOCK_BLOCK * TEST_BLOCK_SIZE + 6], 5u);
    put16(&disk[TEST_ALLOCATOR_BLOCK * TEST_BLOCK_SIZE + 0], 5u);
    if (romfs_mount(&volume, &device) == 0) {
        return 2;
    }

    romfs_build_image();
    disk[TEST_ALLOCATOR_BLOCK * TEST_BLOCK_SIZE + 4u] &=
        (uint8_t)~(1u << 4u);
    if (romfs_mount(&volume, &device) == 0) {
        return 3;
    }
    return 0;
}

static int test_romfs_format(void)
{
    muzix_fs_volume_t volume;
    muzix_block_device_t device;
    int status;

    romfs_build_device(&device);
    romfs_build_image();
    if (romfs_mount(&volume, &device) != 0 || !volume.mounted) {
        return 1;
    }
    if ((status = test_superblock_geometry(&volume)) != 0) {
        return 10 + status;
    }
    if ((status = test_root_inode(&volume)) != 0) {
        return 20 + status;
    }
    if ((status = test_file_inodes(&volume)) != 0) {
        return 30 + status;
    }
    if ((status = test_directory_entries(&volume)) != 0) {
        return 40 + status;
    }
    if ((status = test_file_contents(&volume)) != 0) {
        return 50 + status;
    }
    if ((status = test_inode_round_trip(&volume)) != 0) {
        return 60 + status;
    }
    if ((status = test_zone_ceiling()) != 0) {
        return 70 + status;
    }
    if ((status = test_refused_volumes()) != 0) {
        return 80 + status;
    }
    return 0;
}

int main(void)
{
    return test_romfs_format();
}
