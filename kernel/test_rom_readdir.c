/* host-test-provides: g_block_dev muzix_mm_copy_rom_page_to_kernel
 * muzix_mm_copy_kernel_to_map.  These three extern symbols are referenced by
 * kernel/rom_read.c; the test defines them itself (reading the real minifs
 * image) so the kernel_main/mm_service copies that program the Z80 banks are
 * not pulled into the host link.
 *
 * Verifies that sequential readdir over a directory returns every entry
 * exactly once, with correct inodes and NUL-terminated names, for both the
 * root directory and /doc/.  The ROM block read
 * (muzix_mm_copy_rom_page_to_kernel) is stubbed to read the built minifs
 * image file; g_block_dev is defined here.
 *
 * `index` is a zero-based entry index counting non-free directory entries:
 * `seen` starts at 0 and is compared against `index` before being incremented,
 * so the first non-free entry is index 0, the second index 1, and so on.
 * Free (inode == 0) slots are skipped without incrementing `seen`.
 */

#include "libc.h"
#include "libc.h"
#include "libc.h"

#include "../kernel/rom_read.h"
#include "../fs/block_device.h"

struct muzix_mm_service; /* forward declaration, matches rom_read.h */

/* --- stubs this host test provides itself ----------------------------- */

static uint8_t g_mm_dummy;

muzix_block_device_t g_block_dev = {
    .context = &g_mm_dummy,
    .read    = NULL,
    .write   = NULL,
    .block_count = MUZIX_ROMFS_BLOCK_COUNT,
};

/* Read one ROMFS block from the built minifs image.  rom_read_block() maps a
 * block to (page, offset) via page = rom_offset / 0x4000, so invert that here. */
int muzix_mm_copy_rom_page_to_kernel(struct muzix_mm_service *mm,
                                     uint8_t page,
                                     uint16_t offset,
                                     void *dst,
                                     uint16_t len)
{
    (void)mm;
    uint32_t rom_offset = ((uint32_t)page << 14) + (uint32_t)offset; /* page * 0x4000 */
    if (rom_offset < MUZIX_ROMFS_BASE_OFFSET) {
        return -1;
    }
    uint32_t block = (rom_offset - MUZIX_ROMFS_BASE_OFFSET) / MUZIX_FS_BLOCK_SIZE;
    FILE *f = fopen("build/muzix-minifs.bin", "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, (long)(block * MUZIX_FS_BLOCK_SIZE), SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    size_t n = fread(dst, 1, (size_t)len, f);
    fclose(f);
    return (n == (size_t)len) ? 0 : -1;
}

/* Not called by muzix_rom_readdir(), but rom_read.c references it. */
int muzix_mm_copy_kernel_to_map(struct muzix_mm_service *mm,
                                const void *dst_map,
                                uint16_t user_addr,
                                const uint8_t *src,
                                uint16_t len)
{
    (void)mm; (void)dst_map; (void)user_addr; (void)src; (void)len;
    return 0;
}

/* --- assertions ------------------------------------------------------- */

#define ROOT_FD 5   /* MUZIX_ROM_FD_BASE + MUZIX_FS_ROOT_INODE */
#define DOC_FD  6   /* MUZIX_ROM_FD_BASE + inode 2 (/doc) */

static int check_entry(uint8_t *entry, uint16_t expect_inode, const char *expect_name,
                       const char *where, int index)
{
    uint16_t inode = (uint16_t)entry[0] | ((uint16_t)entry[1] << 8);
    if (inode != expect_inode) {
        printf("%s index %d: inode %u, expected %u\n", where, index, inode, expect_inode);
        return 1;
    }
    /* name occupies entry[2..15] (14 bytes), NUL-terminated at entry[16]. */
    if (entry[16] != '\0') {
        printf("%s index %d: name not NUL-terminated\n", where, index);
        return 1;
    }
    if (strncmp((const char *)entry + 2, expect_name, 14) != 0) {
        printf("%s index %d: name '%.*s', expected '%s'\n", where, index,
               14, (const char *)entry + 2, expect_name);
        return 1;
    }
    size_t n = strlen(expect_name);
    for (size_t i = n; i < 14; i++) {
        if (entry[2 + i] != '\0') {
            printf("%s index %d: name field not NUL-padded\n", where, index);
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    uint8_t entry[17];
    int fails = 0;

    /* Root directory entries (inode 1): . .. doc bin sbin cgi-bin, then past the end. */
    static const struct { uint16_t inode; const char *name; } root_expected[] = {
        {  1, "." },      {  1, ".." },     {  2, "doc" },      {  3, "bin" },
        {  4, "sbin" },   {  5, "cgi-bin" },
    };
    for (int i = 0; i < 6; i++) {
        if (muzix_rom_readdir(ROOT_FD, i, entry) != 0) {
            printf("root index %d: expected an entry, got end-of-directory\n", i);
            fails++;
            continue;
        }
        if (check_entry(entry, root_expected[i].inode, root_expected[i].name,
                        "root", i)) {
            fails++;
        }
    }
    if (muzix_rom_readdir(ROOT_FD, 6, entry) != 1) {
        printf("root index 6: expected past-the-end (1)\n");
        fails++;
    }

    /* /doc entries (inode 2): . .. readme.txt hw-notes.txt copy-notes.txt
     * arch.txt rom.txt, then past the end. */
    static const struct { uint16_t inode; const char *name; } doc_expected[] = {
        {  2, "." },       {  1, ".." },       {  6, "readme.txt"},
        {  7, "hw-notes.txt"},{  8, "copy-notes.txt"},{  9, "arch.txt"},
        { 10, "rom.txt"},
    };
    for (int i = 0; i < 7; i++) {
        if (muzix_rom_readdir(DOC_FD, i, entry) != 0) {
            printf("/doc index %d: expected an entry, got end-of-directory\n", i);
            fails++;
            continue;
        }
        if (check_entry(entry, doc_expected[i].inode, doc_expected[i].name,
                        "/doc", i)) {
            fails++;
        }
    }
    if (muzix_rom_readdir(DOC_FD, 7, entry) != 1) {
        printf("/doc index 7: expected past-the-end (1)\n");
        fails++;
    }

    /* Invalid descriptors are rejected. */
    if (muzix_rom_readdir(0, 0, entry) != -1) {
        printf("fd 0 (console) should be rejected\n");
        fails++;
    }
    if (muzix_rom_readdir(ROOT_FD, 0, NULL) != -1) {
        printf("NULL entry_out should be rejected\n");
        fails++;
    }

    if (fails) {
        printf("FAIL: %d\n", fails);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
