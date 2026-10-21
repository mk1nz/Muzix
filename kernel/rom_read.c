#include <stdint.h>
#include <string.h>

#include "../platform/zeta-v2/test_kernel/bank_model.h"
#include "../fs/block_device.h"
#include "../mm/mm_service.h"
#include "../mm/mm_copy.h"
#include "../platform/zeta-v2/uart_io.h"
#include "rom_read.h"
#include "../platform/zeta-v2/trace.h"

/*
 * ROMFS layout in the Zeta V2 512 KiB Flash ROM:
 *   0x00000-0x00FFF : boot stub + stage-2 kernel (copied to RAM by rom_boot.s)
 *   0x010000-...    : ROMFS (device->block_count blocks of 512 bytes)
 *
 * The ROMFS region is only reachable through banked memory.  Window 1
 * The bank model temporarily maps window 1 to the ROM page, copies out the
 * requested bytes, then restores the complete caller map.  Drivers never
 * manipulate bank registers directly.
 *
 * Context is preserved through the bank switching helpers so the caller's
 * bank registers are never corrupted.
 */

/* The MINIX-v1 filesystem image region is only reachable through banked memory.  Window 1
 * The bank model temporarily maps window 1 to the ROM page, copies out the
 * requested bytes, then restores the complete caller map.  Drivers never
 * manipulate bank registers directly.
 *
 * MUZIX_ROMFS_BASE_OFFSET and MUZIX_ROMFS_BLOCK_COUNT now live in rom_read.h so
 * the mount's block count and the read offset cannot drift apart again.
 *
 * Context is preserved through the bank switching helpers so the caller's
 * bank registers are never corrupted.
 */

#define MUZIX_ROMFS_WINDOW_ADDR 0x4000u
#define MUZIX_ROMFS_BANK_SIZE   0x4000u

int rom_read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer)
{
    uint32_t rom_offset;
    uint8_t page;
    uint16_t offset;
    muzix_mm_service_t *mm;

    if (!device || !buffer) {
        return -1;
    }


    /* The block count is the device's, not a constant: hardcoding 66 here
     * silently limited a differently sized ROMFS image while claiming to read
     * every block the volume described, so a read past the real end returned
     * image bytes from beyond the filesystem. */
    if (device->block_count == 0 || block >= device->block_count) {
        return -1;
    }

    mm = device->context ? (muzix_mm_service_t *)device->context : 0;

    rom_offset = (uint32_t)MUZIX_ROMFS_BASE_OFFSET
               + (uint32_t)block * (uint32_t)MUZIX_FS_BLOCK_SIZE;
    page = (uint8_t)(rom_offset / MUZIX_ROMFS_BANK_SIZE);
    offset = (uint16_t)(rom_offset % MUZIX_ROMFS_BANK_SIZE);


    if (!mm) {
        return -1;
    }
    int result = muzix_mm_copy_rom_page_to_kernel(mm, page,
                                                  offset, buffer,
                                                  MUZIX_FS_BLOCK_SIZE);
    if (result != 0) {
        return -1;
    }
    return 0;
}

/* Window-0 helper for boot-time kernel-to-user copy.
 *
 * Every byte goes through muzix_mm_copy_kernel_to_map() in mm/mm_copy.c, which
 * is the memory manager's entry point: it forwards to zeta_copy_kernel_to_user()
 * with the scheduler's bank state, and that primitive saves the COMPLETE current
 * map - all four windows, via muzix_live_banks - installs a temporary one that
 * differs in exactly the window the destination falls in, and puts the saved map
 * back afterwards.  Nothing here reads a bank register back, because the ports
 * are write-only in strict hardware mode.
 *
 * It used to special-case the destination window: region 2 was written by
 * staging window 1 to the destination page with a direct z80_outb() to port
 * $79, copying with ldir, and writing the saved selector back - a second
 * implementation of the same map swap, one that touched a bank register from a
 * module that is not the memory manager.  The primitive already does this
 * correctly for region 2: it stages into g_copy_staging BEFORE the switch (the
 * caller's source buffer lives in _DATA, which is window 2), then maps the
 * destination page into window 1 and writes at 0x4000 + offset - the same
 * physical page, reached through a window that cannot hide the staging buffer.
 * The MM module already defines an muzix_mm_* function of its own, so routing
 * here costs nothing and removes the bank write.
 *
 * The helper itself still exists for the window-0 reason it always had: during
 * early boot exec_loader lives above window 0 and cannot call the primitive that
 * changes a map, while this module is linked fourth in KERNEL_LINK_ORDER and so
 * sits below 0x4000. */
int muzix_boot_copy_kernel_to_user(struct muzix_mm_service *mm,
                                     const void *dst_map,
                                     uint16_t user_addr,
                                     const uint8_t *src,
                                     size_t len)
{
    if (!mm || !dst_map || !src || len == 0) {
        return -1;
    }

    /* One path for every window.  The MM entry point refuses a request longer
     * than its staging buffer (MUZIX_ZETA_COPY_STAGING) or one that would run
     * past the end of the window, which is the same all-or-nothing answer the
     * region-2 clamp used to give, and the caller - the exec loader's
     * MUZIX_EXEC_MAX_READ-sized chunks and the two-byte argv words - never
     * asks for more. */
    return muzix_mm_copy_kernel_to_map(mm, (const process_map_t *)dst_map,
                                       user_addr, src, (uint16_t)len);
}

/* ---- the kernel-side ROM filesystem interceptors -----------------------
 *
 * `ls` printed nothing, and the reason had two halves that only look like
 * one bug.  Measured on 2026-10-14 with the send/recv paths traced:
 *
 *   1. every reply the FS server sent to a FORKED CHILD was refused with
 *      E_BAD_DEST, because `muzix_ipc_register_server()` had only ever been
 *      called for the four slots kernel_main() starts itself (0, FS, MM, PM,
 *      shell) - never for a child of SYS_FORK, so slot 5's IPC entry still
 *      had P_SLOT_FREE set.  A parked child's syscall then returned the park
 *      placeholder 0 instead of the reply, forever.
 *
 *   2. with a real answer arriving, it would still have been -1: the FS
 *      server's file handlers are stubs.  servers/fs/fs_server.lk links
 *      fs_server + fs_clock + tty_device + the bank primitives and nothing
 *      else, so handle_open()/handle_readdir()/handle_read()/handle_stat()
 *      are all `return -1` with a comment saying the fs/ service layer that
 *      would serve them is not linked there.
 *
 * The second half is answered HERE, in the kernel, for the ROM filesystem -
 * which is the only filesystem that exists (the MINIX-v1 image lives in the
 * ROM at 0x20000 and there is no writable disk behind it).  The kernel
 * already owns every piece this needs: the block device (g_block_dev), the
 * bank-safe block read (rom_read_block above), and the on-disk layout that
 * exec_loader.c's ROM path has walked since the boot loader.  Answering
 * open/readdir for ROM paths here rather than routing them into a stub is
 * the same move the kernel already makes for TIMES, PROCTAB, RTC, LOAD and
 * IDS - an interceptor ahead of the routing table, in
 * muzix_handle_userspace_syscall_ipc().
 *
 * What this deliberately does NOT do is become a filesystem: there is no
 * write path, no subdirectories (the image is flat - one root directory),
 * and no fd table.  open() resolves a name to an inode and hands back a
 * synthetic descriptor; readdir() walks the root directory's block.  The
 * descriptor range is disjoint from the console's 0/1/2 precisely so a
 * synthetic fd can never be mistaken for one, and close() on one is a no-op
 * acknowledged by the kernel rather than by the stub it would otherwise
 * reach.
 */

extern struct muzix_block_device g_block_dev;

/* One block of MINIX-v1 metadata, in _DATA rather than on the kernel stack:
 * exec_loader.c's ROM path takes the stack route with three such blocks at
 * once, which works, and it is also why the kernel's headroom figure is what
 * it is.  The two reads below are sequential - the inode-table block is
 * parsed into the zone number, then the same buffer takes the directory
 * block - and the read path parses its inode into locals before reusing the
 * buffer for data, so one block's worth of data is enough. */
static uint8_t g_rom_fs_block[512];

/* A synthetic descriptor is the inode number offset clear of the console
 * descriptors.  Inodes are 1..31 in this image (two 512-byte table blocks of
 * 16 inodes), so the range is 5..35 and cannot collide with 0/1/2. */
#define MUZIX_ROM_FD_BASE 4

#define MUZIX_ROM_FD_SLOTS 32
#define MUZIX_ROM_DIRECT_ZONES 7
#define MUZIX_ROM_BLOCK_SIZE 512
#define MUZIX_ROM_INDIRECT_SLOTS 256

/* The read pointer of each synthetic descriptor.  There is no descriptor
 * table here to hang it on - the FS server that would own one links no
 * service layer - so the offset lives beside the descriptor it belongs to.
 * One process at a time reads the ROM filesystem in practice (the shell forks
 * and waits), which is what makes a flat table the honest size: it is a known
 * limit, not an unknown one. */
static uint32_t g_rom_fd_pos[MUZIX_ROM_FD_SLOTS];

#define MUZIX_FS_ROOT_INODE 1

/* Filesystem constants */
#define MUZIX_FS_BLOCK_SIZE 512u
#define MUZIX_FS_INODE_SIZE 32u
#define MUZIX_FS_DIR_ENTRY_SIZE 16u

/* The MINIX-v1 layout, in one place, because two readers of it already
 * disagree about nothing but would drift if they could: the inode table is
 * blocks 3-4 (16 inodes per 512-byte block, 32 bytes per inode), the root
 * directory is inode 1, and an inode's first zone is at byte offset 14. */
static int rom_fs_read_block(uint16_t block, uint8_t *buffer)
{
    return rom_read_block(&g_block_dev, block, buffer);
}

/* Read an inode by number into a caller-provided 32-byte buffer. */
static int rom_fs_get_inode(uint16_t inode_num, uint8_t *inode_out)
{
    uint8_t block_buf[512];
    uint16_t block = (inode_num * MUZIX_FS_INODE_SIZE) / MUZIX_FS_BLOCK_SIZE;
    uint16_t off = (inode_num * MUZIX_FS_INODE_SIZE) % MUZIX_FS_BLOCK_SIZE;
    if (rom_fs_read_block((uint16_t)(3 + block), block_buf) != 0) {
        return -1;
    }
    memcpy(inode_out, &block_buf[off], MUZIX_FS_INODE_SIZE);
    return 0;
}

/* Read the data zone (directory block) for an inode into the caller's buffer. */
static int rom_fs_read_dir_block(uint16_t inode_num, uint8_t *buf)
{
    uint8_t inode_buf[32];
    uint16_t zone;

    if (rom_fs_get_inode(inode_num, inode_buf) != 0) {
        return -1;
    }
    zone = (uint16_t)inode_buf[14] | ((uint16_t)inode_buf[15] << 8);
    if (zone == 0) {
        return -1;
    }
    return rom_fs_read_block(zone, buf);
}

/* Look up a name in a directory inode, returning the entry inode or 0. */
static uint16_t rom_fs_lookup(uint16_t dir_inode, const char *name, uint8_t name_len)
{
    uint8_t dir_block[512];
    int i;

    if (name_len == 0) {
        return 0;
    }
    if (rom_fs_read_dir_block(dir_inode, dir_block) != 0) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        uint8_t *entry = &dir_block[i * MUZIX_FS_DIR_ENTRY_SIZE];
        uint16_t entry_inode = (uint16_t)entry[0] | ((uint16_t)entry[1] << 8);
        if (entry_inode == 0) {
            continue;
        }
        /* Bounded compare: check length match and content match */
        if (strncmp((char*)&entry[2], name, name_len) == 0 &&
            entry[2 + name_len] == 0) {
            return entry_inode;
        }
    }
    return 0;
}

/* Read the root directory's first block into the caller's 512-byte buffer. */
static int rom_fs_root_dir(uint8_t *block)
{
    return rom_fs_read_dir_block((uint16_t)MUZIX_FS_ROOT_INODE, block);
}

int muzix_rom_fd_is_synthetic(int fd)
{
    int inode = fd - MUZIX_ROM_FD_BASE;
    return inode >= MUZIX_FS_ROOT_INODE && inode <= 31;
}

/* Resolve a path in the ROM filesystem.  Supports /bin and /sbin directories.
 * Returns a synthetic descriptor or -1. */
int muzix_rom_open_path(const char *path)
{
    uint16_t current_inode = MUZIX_FS_ROOT_INODE;
    const char *p = path;

    if (!path) {
        return -1;
    }

    /* Skip leading slashes */
    while (*p == '/') {
        p++;
    }

    /* Empty path or "." is the root directory */
    if (*p == '\0' || (p[0] == '.' && p[1] == '\0')) {
        g_rom_fd_pos[MUZIX_FS_ROOT_INODE] = 0;
        return MUZIX_ROM_FD_BASE + MUZIX_FS_ROOT_INODE;
    }

    /* Traverse path components */
    while (*p) {
        const char *comp_start = p;
        uint8_t comp_len = 0;

        while (*p && *p != '/') {
            comp_len++;
            p++;
        }

        /* Look up this component */
        uint16_t next_inode = rom_fs_lookup(current_inode, comp_start, comp_len);
        if (next_inode == 0) {
            return -1;
        }
        current_inode = next_inode;

        /* Skip trailing slashes */
        while (*p == '/') {
            p++;
        }
    }

    g_rom_fd_pos[current_inode] = 0;
    return MUZIX_ROM_FD_BASE + current_inode;
}

/* Walk a path to its inode number (the caller can then read the inode).
 * Returns the inode number, or 0 on failure. */
uint16_t muzix_rom_resolve_inode(const char *path)
{
    uint16_t current_inode = MUZIX_FS_ROOT_INODE;
    const char *p = path;

    if (!path) {
        return 0;
    }

    while (*p == '/') {
        p++;
    }

    if (*p == '\0' || (p[0] == '.' && p[1] == '\0')) {
        return MUZIX_FS_ROOT_INODE;
    }

    while (*p != '\0') {
        char name[15];
        uint8_t dir_block[512];
        int i;
        uint16_t found_inode = 0;
        int name_len;

        for (i = 0; i < 14 && p[i] != '\0' && p[i] != '/'; i++) {
            name[i] = p[i];
        }
        name_len = i;
        name[name_len] = '\0';
        p += name_len;
        while (*p == '/') {
            p++;
        }

        if (name_len == 0) {
            continue;
        }

        if (rom_fs_read_dir_block(current_inode, dir_block) != 0) {
            return 0;
        }

        for (i = 0; i < 32; i++) {
            uint8_t *entry = &dir_block[i * MUZIX_FS_DIR_ENTRY_SIZE];
            uint16_t entry_inode = (uint16_t)entry[0] | ((uint16_t)entry[1] << 8);

            if (entry_inode == 0) {
                continue;
            }

            if (strncmp((const char *)&entry[2], name, name_len) == 0 &&
                ((const char *)&entry[2])[name_len] == '\0') {
                found_inode = entry_inode;
                break;
            }
        }

        if (found_inode == 0) {
            return 0;
        }

        current_inode = found_inode;
    }

    return current_inode;
}

/* Look up a path's inode and copy the raw 32-byte inode data into the caller's buffer. */
int muzix_rom_lookup_inode(const char *path, uint8_t *inode_out)
{
    uint16_t inode_num = muzix_rom_resolve_inode(path);
    if (inode_num == 0) {
        return -1;
    }
    return rom_fs_get_inode(inode_num, inode_out);
}

/* Copy the inode for a synthetic fd into the caller's 32-byte buffer. */
int muzix_rom_fstat(int fd, uint8_t *inode_out)
{
    int inode = fd - MUZIX_ROM_FD_BASE;
    if (inode < MUZIX_FS_ROOT_INODE || inode > 31) {
        return -1;
    }
    return rom_fs_get_inode((uint16_t)inode, inode_out);
}

/* Walk a directory inode to its index-th entry.  Free slots are skipped, so
 * index counts entries the way the fs/ service layer's walk does, and 0/1/-1
 * are the same three answers its readdir gives: entry filled, past the end,
 * error. */
int muzix_rom_readdir(int fd, uint16_t index, uint8_t *entry_out)
{
    int inode = fd - MUZIX_ROM_FD_BASE;
    uint16_t seen = 0;
    int i;
    uint8_t dir_block[512];

    if (!entry_out || !muzix_rom_fd_is_synthetic(fd)) {
        return -1;
    }
    if (inode != MUZIX_FS_ROOT_INODE) {
        /* Reading a subdirectory: read its data block */
        if (rom_fs_read_dir_block((uint16_t)inode, dir_block) != 0) {
            return -1;
        }
    } else {
        if (rom_fs_root_dir(dir_block) != 0) {
            return -1;
        }
    }
    for (i = 0; i < 32; i++) {
        uint8_t *entry = &dir_block[i * MUZIX_FS_DIR_ENTRY_SIZE];

        if (((uint16_t)entry[0] | ((uint16_t)entry[1] << 8)) == 0) {
            continue;
        }
        if (seen == index) {
            /* MINIX-v1 directory entries have no terminating NUL in the 14-byte
             * name; the userspace struct dirent gives name[] one extra byte so a
             * NUL can be placed there. Copy the 16-byte on-disk entry and add
             * that terminator. */
            memcpy(entry_out, entry, MUZIX_FS_DIR_ENTRY_SIZE);
            entry_out[MUZIX_FS_DIR_ENTRY_SIZE] = '\0';
            return 0;
        }
        seen++;
    }
    return 1;
}

/* Move a synthetic descriptor's read pointer.  whence is the SEEK_SET family
 * the syscall layer is given: 0 (set) is the only one the ROM filesystem
 * needs, and anything else is answered as "set" because there is no other
 * origin a flat image has. */
int muzix_rom_seek(int fd, uint32_t offset, int whence)
{
    int inode;

    if (!muzix_rom_fd_is_synthetic(fd)) {
        return -1;
    }
    inode = fd - MUZIX_ROM_FD_BASE;
    if (offset > 0xFFFFu) {
        offset = 0xFFFFu;
    }
    (void)whence;
    g_rom_fd_pos[inode] = offset;
    return 0;
}

/* Read the next `count` bytes of a synthetic descriptor into a KERNEL buffer.
 * Returns the byte count (0 at end of file) or -1.  The copy out to the
 * caller's buffer is the syscall layer's, exactly as readdir's is: this
 * function fills kernel memory and muzix_copy_to_proc() puts it in the
 * process, because a read() from a parked process cannot reach its own
 * buffer after the park has happened.
 *
 * The zone walk is exec_loader.c's, in one place instead of two: seven direct
 * zones in the inode, one indirect block holding 256 more, 512 bytes each.
 * The inode is parsed into locals first so that g_rom_fs_block can carry the
 * data blocks afterwards. */
int muzix_rom_read(int fd, uint8_t *buffer, uint16_t count)
{
    int inode;
    uint8_t *inode_entry;
    uint32_t file_size;
    uint16_t zones[MUZIX_ROM_DIRECT_ZONES];
    uint16_t indirect_zone = 0;
    uint16_t indirect_zones[MUZIX_ROM_INDIRECT_SLOTS];
    uint8_t have_indirect = 0;
    uint32_t pos;
    uint16_t done = 0;

    if (!buffer || !muzix_rom_fd_is_synthetic(fd)) {
        return -1;
    }
    inode = fd - MUZIX_ROM_FD_BASE;
    if (inode == MUZIX_FS_ROOT_INODE) {
        /* The one directory in the image is not a readable file. */
        return -1;
    }

    if (rom_fs_read_block((uint16_t)(3 + (inode >> 4)), g_rom_fs_block) != 0) {
        return -1;
    }
    inode_entry = &g_rom_fs_block[(inode & 0x0F) * 32];
    file_size = (uint32_t)inode_entry[4]
              | ((uint32_t)inode_entry[5] << 8)
              | ((uint32_t)inode_entry[6] << 16)
              | ((uint32_t)inode_entry[7] << 24);
    for (int i = 0; i < MUZIX_ROM_DIRECT_ZONES; i++) {
        zones[i] = (uint16_t)inode_entry[14 + i * 2]
                 | ((uint16_t)inode_entry[15 + i * 2] << 8);
    }
    indirect_zone = (uint16_t)inode_entry[28] | ((uint16_t)inode_entry[29] << 8);

    pos = g_rom_fd_pos[inode];
    if (pos >= file_size) {
        return 0;
    }
    if ((uint32_t)count > file_size - pos) {
        count = (uint16_t)(file_size - pos);
    }

    while (done < count) {
        uint32_t zone_index = (pos + done) / MUZIX_ROM_BLOCK_SIZE;
        uint16_t zone;
        uint16_t offset = (uint16_t)((pos + done) % MUZIX_ROM_BLOCK_SIZE);
        uint16_t chunk = MUZIX_ROM_BLOCK_SIZE - offset;

        if (chunk > count - done) {
            chunk = (uint16_t)(count - done);
        }
        if (zone_index < MUZIX_ROM_DIRECT_ZONES) {
            zone = zones[zone_index];
        } else {
            if (!have_indirect) {
                if (indirect_zone == 0 ||
                    rom_fs_read_block(indirect_zone, g_rom_fs_block) != 0) {
                    return (done > 0) ? (int)done : -1;
                }
                for (int i = 0; i < MUZIX_ROM_INDIRECT_SLOTS; i++) {
                    indirect_zones[i] = (uint16_t)g_rom_fs_block[i * 2]
                                      | ((uint16_t)g_rom_fs_block[i * 2 + 1] << 8);
                }
                have_indirect = 1;
            }
            zone_index -= MUZIX_ROM_DIRECT_ZONES;
            if (zone_index >= MUZIX_ROM_INDIRECT_SLOTS) {
                break;
            }
            zone = indirect_zones[zone_index];
        }
        if (zone == 0) {
            break;
        }
        if (rom_fs_read_block(zone, g_rom_fs_block) != 0) {
            return (done > 0) ? (int)done : -1;
        }
        for (uint16_t i = 0; i < chunk; i++) {
            buffer[done + i] = g_rom_fs_block[offset + i];
        }
        done += chunk;
    }

    g_rom_fd_pos[inode] = pos + done;
    return (int)done;
}
