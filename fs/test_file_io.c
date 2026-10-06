#include "file_io.h"

#include <string.h>

/* Where the indirect block starts, and why it is not offset 512.
 *
 * An inode carries MUZIX_FS_DIRECT_ZONES zone numbers directly and one more as
 * its single-indirect pointer.  That count is 7, and it is MINIX 1.0's own
 * number rather than an arbitrary one: NR_ZONE_NUMS is 9 and NR_DZONE_NUM is
 * NR_ZONE_NUMS - 2, so of the nine zone numbers
 * in a MINIX inode the first seven are direct, i_zone[7] is the single indirect
 * block and i_zone[8] is the double one.  This port keeps the on-disk record at
 * 32 bytes with the same offsets - fs/inode_store.c:52-55 reads seven zone
 * pointers from disk offsets 11,13,...,23 and the indirect pointer from offset
 * 25 - and tools/make_minixfs.rb pins the same 7 in its own source with the
 * sentence "the kernel reads exactly this many direct zone pointers from the
 * inode and then follows indirect_zone".
 *
 * So the first block reached through the indirect block is index 7.
 *
 * This file used to write at offset 512 and expect the write to land in the
 * indirect block, i.e. it assumed one direct zone.  Nothing in the tree has ever
 * used one: MUZIX_FS_DIRECT_ZONES has been 7 since the initial import.  Offset
 * 512 is index 1, which is below the direct count, so muzix_fs_file_zone()
 * returned inode->zone[1] - 0, because the test only ever set zone[0] - and
 * refused with -1.  Measured, not reasoned: with the test unchanged the write
 * returns -1 and done stays 0, and inode->zone[1] reads back 0.  The refusal
 * was the correct behaviour, because a zone number of 0 means "no zone here".
 *
 * Cases 5 and 6 below are that same case moved to index 7, so they still cover
 * the indirect block rather than skipping past it.  Case 7 is new and is the
 * one that would catch the interesting fault: a write that spans the boundary
 * has to split across two zones, taking the low half from zone[6] and the high
 * half through the indirect block, and read and write have to agree about where
 * that boundary is.  Cases 9 and 10 are new and cover the allocating branch,
 * which is the one fs_service.c:2031 uses and which every other case here skips
 * by pre-populating the indirect entry.
 */
#define FIRST_INDIRECT_POS ((uint32_t)MUZIX_FS_DIRECT_ZONES * MUZIX_FS_BLOCK_SIZE)

static uint8_t disk[MUZIX_FS_BLOCK_SIZE * 4];

static void put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static uint16_t get16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static int read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        buffer[i] = disk[block * MUZIX_FS_BLOCK_SIZE + i];
    }
    return 0;
}

static int write_block(struct muzix_block_device *device, uint16_t block, const uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        disk[block * MUZIX_FS_BLOCK_SIZE + i] = buffer[i];
    }
    return 0;
}

static int test_file_io(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 4};
    muzix_fs_block_cache_t cache;
    muzix_fs_inode_t inode = {0};
    muzix_fs_inode_t allocated_inode = {0};
    muzix_fs_zone_allocator_t allocator;
    uint8_t source[4] = {1, 2, 3, 4};
    uint8_t result_buffer[4] = {0, 0, 0, 0};
    size_t done;

    muzix_fs_cache_init(&cache, &device);
    inode.zone[0] = 1;
    if (muzix_fs_file_write(&cache, &inode, 0, source, sizeof(source), &done) != 0 ||
        done != sizeof(source) || inode.size != sizeof(source) ||
        muzix_fs_cache_flush(&cache) != 0) {
        return 1;
    }

    muzix_fs_cache_init(&cache, &device);
    if (muzix_fs_file_read(&cache, &inode, 0, result_buffer,
                           sizeof(result_buffer), &done) != 0 ||
        done != sizeof(result_buffer) || result_buffer[2] != 3) {
        return 2;
    }
    if (muzix_fs_file_read(&cache, &inode, inode.size,
                           result_buffer, sizeof(result_buffer), &done) != 0 ||
        done != 0) {
        return 3;
    }

    muzix_fs_zone_allocator_init(&allocator, 4, 1);
    if (muzix_fs_file_write_alloc(&cache, &allocator, &allocated_inode,
                                  0, source, 2, &done) != 0 || done != 2 ||
        allocated_inode.zone[0] != 1) {
        return 4;
    }

    muzix_fs_cache_init(&cache, &device);
    inode.size = FIRST_INDIRECT_POS + 2;
    inode.indirect_zone = 2;
    put16(&disk[2 * MUZIX_FS_BLOCK_SIZE], 3);
    if (muzix_fs_file_write(&cache, &inode, FIRST_INDIRECT_POS,
                            source, 2, &done) != 0 || done != 2 ||
        muzix_fs_cache_flush(&cache) != 0 ||
        disk[3 * MUZIX_FS_BLOCK_SIZE] != 1 ||
        disk[3 * MUZIX_FS_BLOCK_SIZE + 1] != 2) {
        return 5;
    }

    muzix_fs_cache_init(&cache, &device);
    memset(result_buffer, 0, sizeof(result_buffer));
    if (muzix_fs_file_read(&cache, &inode, FIRST_INDIRECT_POS,
                           result_buffer, 2, &done) != 0 ||
        done != 2 || result_buffer[0] != 1 || result_buffer[1] != 2) {
        return 6;
    }

    /* A write that straddles the last direct zone and the first indirect one:
     * two bytes into zone[6] at offset 510, two bytes through the indirect block
     * at offset 0 of the block the indirect entry names. */
    inode.zone[MUZIX_FS_DIRECT_ZONES - 1] = 1;
    muzix_fs_cache_init(&cache, &device);
    if (muzix_fs_file_write(&cache, &inode, FIRST_INDIRECT_POS - 2,
                            source, 4, &done) != 0 || done != 4 ||
        muzix_fs_cache_flush(&cache) != 0 ||
        disk[1 * MUZIX_FS_BLOCK_SIZE + 510] != 1 ||
        disk[1 * MUZIX_FS_BLOCK_SIZE + 511] != 2 ||
        disk[3 * MUZIX_FS_BLOCK_SIZE] != 3 ||
        disk[3 * MUZIX_FS_BLOCK_SIZE + 1] != 4) {
        return 7;
    }

    muzix_fs_cache_init(&cache, &device);
    memset(result_buffer, 0, sizeof(result_buffer));
    if (muzix_fs_file_read(&cache, &inode, FIRST_INDIRECT_POS - 2,
                           result_buffer, 4, &done) != 0 ||
        done != 4 || result_buffer[0] != 1 || result_buffer[1] != 2 ||
        result_buffer[2] != 3 || result_buffer[3] != 4) {
        return 8;
    }

    /* The path the FS service actually uses.  muzix_fs_file_write() above
     * reaches muzix_fs_file_zone(); this reaches muzix_fs_file_zone_alloc(),
     * whose indirect branch is a separate function and is the one that has to
     * allocate the indirect block, populate the entry and mark that block
     * dirty.  Cases 5 to 8 all pre-populate the entry, so nothing else here
     * runs it. */
    muzix_fs_zone_allocator_init(&allocator, 4, 1);
    memset(&allocated_inode, 0, sizeof(allocated_inode));
    memset(disk, 0, sizeof(disk));
    muzix_fs_cache_init(&cache, &device);
    if (muzix_fs_file_write_alloc(&cache, &allocator, &allocated_inode,
                                  FIRST_INDIRECT_POS, source, 2, &done) != 0 ||
        done != 2 || allocated_inode.indirect_zone != 1 ||
        muzix_fs_cache_flush(&cache) != 0 ||
        get16(&disk[1 * MUZIX_FS_BLOCK_SIZE]) != 2 ||
        disk[2 * MUZIX_FS_BLOCK_SIZE] != 1 ||
        disk[2 * MUZIX_FS_BLOCK_SIZE + 1] != 2) {
        return 9;
    }

    muzix_fs_cache_init(&cache, &device);
    memset(result_buffer, 0, sizeof(result_buffer));
    if (muzix_fs_file_read(&cache, &allocated_inode, FIRST_INDIRECT_POS,
                           result_buffer, 2, &done) != 0 ||
        done != 2 || result_buffer[0] != 1 || result_buffer[1] != 2) {
        return 10;
    }
    return 0;
}

int main(void)
{
    return test_file_io();
}
