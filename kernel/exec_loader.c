#include "exec_loader.h"

#include <string.h>

#include "syscalls.h"
#include "proc_table.h"
#include "../platform/zeta-v2/test_kernel/bank_model.h"
#include "../platform/zeta-v2/rtc_ds1302.h"
#include "../mm/mproc_model.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../fs/fs_types.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../mm/mm_service.h"
#include "../include/muzix_ipc.h"
#include "rom_read.h"

/* External reference to the block device from kernel_main.c */
extern struct muzix_block_device g_block_dev;

/* Filesystem constants for ROM path lookup */
#define EXEC_BLOCK_SIZE 512u
#define EXEC_INODE_SIZE 32u
#define EXEC_DIR_ENTRY_SIZE 16u
#define EXEC_FS_ROOT_INODE 1u

/* Flag to control whether to use IPC (true) or direct ROM reads (false) */
static bool g_exec_use_ipc = false;

/* Static variables for exec_loader state */
static int g_exec_handle;
static int g_exec_result;
static int g_exec_pos;
static int g_exec_offset;
static int g_exec_chunk;
static int g_exec_file_size;
static uint8_t g_exec_text_page;
static uint16_t g_exec_entry_point;
static muzix_mm_service_t *g_exec_mm;
static muzix_kernel_proc_table_t *g_exec_proc_table;
static int g_exec_slot;
static const char *g_exec_path;
static const char *const *g_exec_argv;
static int g_exec_argc;
static uint16_t g_exec_argv_ptr;

/* Buffer for loading executable data */
static uint8_t g_exec_load_buf[256];

/* Buffer for argv pointer array (max 16 args).
 *
 * The bound is a named constant because two loops write it and a third reads
 * it: muzix_exec_copy_argv() counts argc by walking this array until a NUL, so
 * the walk has to stop at the array's own last entry rather than at a
 * terminator that might never come. */
#define MUZIX_EXEC_ARGV_PTRS 16
static const char *g_exec_argv_ptrs[MUZIX_EXEC_ARGV_PTRS];

/* Cached inode info for ROM reads */
static uint8_t g_exec_rom_inode_cached = 0;
static uint16_t g_exec_rom_cached_inode = 0;
static uint32_t g_exec_rom_file_size = 0;
static uint16_t g_exec_rom_zones[7];
static uint16_t g_exec_rom_indirect_zone = 0;

/* The whole zone list, direct plus indirect, built once per open.
 *
 * exec_rom_read() used to rebuild it on every call - which is every 256-byte
 * chunk of the image, so 27 times for the shell - and rebuilding it cost a
 * read of the inode block, a read of the indirect block, and a walk of 256
 * little-endian zone numbers to count the nonzero ones, on top of the data
 * block it actually wanted.  Measured in the boot: 437 ROM block reads where
 * the images need 108, and the exec phase was the largest single part of the
 * boot before the banner.  The list is stable for the life of the open, so it
 * is built once and dropped when the handle is opened again. */
static uint16_t g_exec_zone_list[263];  /* 7 direct + 256 indirect */
static uint16_t g_exec_zone_count = 0;
static uint8_t g_exec_zone_list_valid = 0;

/* External reference to the block device from kernel_main.c */
extern struct muzix_block_device g_block_dev;

/* Flag to control whether to use IPC (true) or direct ROM reads (false) */
/* (defined above) */

/* Forward declarations for static functions */
static int exec_ipc_open_path(const char *path, int flags);
static int exec_ipc_close(int fd);
static int exec_ipc_read(int fd, void *buf, size_t count, size_t *result);
static int exec_ipc_fstat(int fd, void *stat);
static int exec_rom_open_path(const char *path, int flags);
static int exec_rom_close(int inode);
static int exec_rom_read(int inode, void *buf, size_t count, size_t *result);
static int exec_rom_fstat(int inode, void *stat);

/* Wrapper functions that choose between IPC and direct ROM */
static int exec_open_path(const char *path, int flags)
{
    if (g_exec_use_ipc) {
        return exec_ipc_open_path(path, flags);
    } else {
        return exec_rom_open_path(path, flags);
    }
}

static int exec_close(int fd)
{
    if (g_exec_use_ipc) {
        return exec_ipc_close(fd);
    } else {
        return exec_rom_close(fd);
    }
}

static int exec_read(int fd, void *buf, size_t count, size_t *result)
{
    if (g_exec_use_ipc) {
        return exec_ipc_read(fd, buf, count, result);
    } else {
        return exec_rom_read(fd, buf, count, result);
    }
}

static int exec_fstat(int fd, void *stat)
{
    if (g_exec_use_ipc) {
        return exec_ipc_fstat(fd, stat);
    } else {
        return exec_rom_fstat(fd, stat);
    }
}

/* IPC helper functions for filesystem operations */
static int exec_ipc_open_path(const char *path, int flags)
{
    muzix_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_USER_SYS_OPEN;
    msg.m1_p1 = (uint16_t)path;
    msg.m1_i2 = flags;
    
    int server_slot = MUZIX_PROC_FS;
    int current_slot = g_exec_slot;
    muzix_ipc_table_t *ipc = muzix_ipc_get_global();
    int ipc_result = muzix_ipc_sendrec(ipc, current_slot, server_slot, &msg);
    if (ipc_result >= 10) {
        int woken_slot = ipc_result - 10;
        if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_proc_table_ready(g_exec_proc_table, woken_slot);
        }
    }
    return (ipc_result == MUZIX_IPC_OK) ? msg.m1_i1 : -1;
}

static int exec_ipc_close(int fd)
{
    muzix_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_USER_SYS_CLOSE;
    msg.m1_i1 = fd;
    
    int server_slot = MUZIX_PROC_FS;
    int current_slot = g_exec_slot;
    muzix_ipc_table_t *ipc = muzix_ipc_get_global();
    int ipc_result = muzix_ipc_sendrec(ipc, current_slot, server_slot, &msg);
    if (ipc_result >= 10) {
        int woken_slot = ipc_result - 10;
        if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_proc_table_ready(g_exec_proc_table, woken_slot);
        }
    }
    return (ipc_result == MUZIX_IPC_OK) ? msg.m1_i1 : -1;
}

static int exec_ipc_read(int fd, void *buf, size_t count, size_t *result)
{
    muzix_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_USER_SYS_READ;
    msg.m1_i1 = fd;
    msg.m1_p1 = (uint16_t)buf;
    msg.m1_i2 = count;
    
    int server_slot = MUZIX_PROC_FS;
    int current_slot = g_exec_slot;
    muzix_ipc_table_t *ipc = muzix_ipc_get_global();
    int ipc_result = muzix_ipc_sendrec(ipc, current_slot, server_slot, &msg);
    if (ipc_result >= 10) {
        int woken_slot = ipc_result - 10;
        if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_proc_table_ready(g_exec_proc_table, woken_slot);
        }
    }
    if (ipc_result == MUZIX_IPC_OK && result) {
        *result = msg.m1_i1;
    }
    return (ipc_result == MUZIX_IPC_OK) ? 0 : -1;
}

static int exec_ipc_fstat(int fd, void *stat)
{
    muzix_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_USER_SYS_FSTAT;
    msg.m1_i1 = fd;
    msg.m1_p1 = (uint16_t)stat;
    
    int server_slot = MUZIX_PROC_FS;
    int current_slot = g_exec_slot;
    muzix_ipc_table_t *ipc = muzix_ipc_get_global();
    int ipc_result = muzix_ipc_sendrec(ipc, current_slot, server_slot, &msg);
    if (ipc_result >= 10) {
        int woken_slot = ipc_result - 10;
        if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_proc_table_ready(g_exec_proc_table, woken_slot);
        }
    }
    return (ipc_result == MUZIX_IPC_OK) ? msg.m1_i1 : -1;
}

/* Direct ROM read helpers for initial server loading (before FS server is running) */
static int exec_rom_read_block(uint16_t block, uint8_t *buffer)
{
    return rom_read_block(&g_block_dev, block, buffer);
}

/* Read an inode from the ROM filesystem by inode number.
 * Returns 0 on success, -1 on failure. */
static int exec_rom_read_inode(uint16_t inode_num, uint8_t *inode_buf)
{
    /* Inode table starts at block 3, each inode is 32 bytes */
    uint16_t inode_block = 3 + (inode_num * EXEC_INODE_SIZE) / EXEC_BLOCK_SIZE;
    uint16_t inode_off = (inode_num * EXEC_INODE_SIZE) % EXEC_BLOCK_SIZE;
    uint8_t block_buf[512];

    if (exec_rom_read_block(inode_block, block_buf) != 0) {
        return -1;
    }
    memcpy(inode_buf, &block_buf[inode_off], EXEC_INODE_SIZE);
    return 0;
}

/* Read a block from a zone (data block) in the filesystem */
static int exec_rom_read_zone(uint16_t zone, uint8_t *buffer)
{
    return exec_rom_read_block(zone, buffer);
}

/* Look up a name in a directory inode, return inode number or 0. */
static uint16_t exec_rom_lookup_in_dir(uint16_t dir_inode, const char *name, uint8_t name_len)
{
    uint8_t inode_buf[32];
    uint8_t dir_block[512];
    uint16_t dir_zone;
    uint16_t i;

    if (exec_rom_read_inode(dir_inode, inode_buf) != 0) {
        return 0;
    }

    /* Directory zone 0 holds the directory entries */
    dir_zone = inode_buf[14] | (inode_buf[15] << 8);
    if (dir_zone == 0) {
        return 0;
    }

    if (exec_rom_read_zone(dir_zone, dir_block) != 0) {
        return 0;
    }

    /* Search directory entries: 16 bytes each, 32 entries per 512-byte block */
    for (i = 0; i < 32; i++) {
        uint8_t *entry = &dir_block[i * 16];
        uint16_t entry_inode = entry[0] | (entry[1] << 8);
        if (entry_inode == 0) {
            continue;
        }
        /* Compare name (14 bytes, bounded compare) */
        if (entry[2 + name_len] == 0 &&
            strncmp((char*)&entry[2], name, name_len) == 0) {
            return entry_inode;
        }
    }
    return 0;
}

static int exec_rom_open_path(const char *path, int flags)
{
    (void)flags;

    uint16_t current_inode = EXEC_FS_ROOT_INODE;  /* Start at root */

    /* Skip leading slash */
    while (*path == '/') {
        path++;
    }

    /* If empty path, just return root */
    if (*path == 0) {
        g_exec_rom_inode_cached = 0;
        g_exec_zone_list_valid = 0;
        return EXEC_FS_ROOT_INODE;
    }

    /* Traverse path components */
    while (*path) {
        /* Find the next component */
        const char *comp_start = path;
        while (*path && *path != '/') {
            path++;
        }
        uint8_t comp_len = path - comp_start;

        /* Look up this component in the current directory */
        uint16_t next_inode = exec_rom_lookup_in_dir(current_inode, comp_start, comp_len);
        if (next_inode == 0) {
            return -1;
        }
        current_inode = next_inode;

        /* Skip slashes */
        while (*path == '/') {
            path++;
        }
    }


    /* A new handle invalidates the cached inode AND the cached zone
     * list: the next read belongs to a different file, and the list
     * the last one built would send it to the last one's data. */
    g_exec_rom_inode_cached = 0;
    g_exec_zone_list_valid = 0;
    return current_inode;  /* Return inode number as handle */
}

static int exec_rom_close(int inode)
{
    (void)inode;
    return 0;
}

static int exec_rom_read(int inode, void *buf, size_t count, size_t *result)
{
    
    uint16_t zones[7];
    uint16_t indirect_zones[256];
    uint8_t indirect_block[512];
    uint8_t have_indirect = 0;
    uint16_t indirect_block_num = 0;
    
    if (!g_exec_rom_inode_cached || g_exec_rom_file_size == 0 || g_exec_rom_cached_inode != inode) {
        /* Read inode table (blocks 3-4) to get file info */
        /* Inode table is at blocks 3-4 (2 blocks = 1024 bytes = 32 inodes max) */
        uint8_t inode_block[512];
        uint8_t inode_block_num = 3 + (inode >> 4);  /* 16 inodes per block */
        if (exec_rom_read_block(inode_block_num, inode_block) != 0) {
            return -1;
        }
        
        /* Get inode entry (32 bytes each) - index within block */
        uint8_t *inode_entry = &inode_block[(inode & 0xF) * 32];
        g_exec_rom_file_size = (uint32_t)inode_entry[4]
                             | ((uint32_t)inode_entry[5] << 8)
                             | ((uint32_t)inode_entry[6] << 16)
                             | ((uint32_t)inode_entry[7] << 24);
        for (int i = 0; i < 7; i++) {
            g_exec_rom_zones[i] = (uint16_t)inode_entry[14 + i * 2]
                                | ((uint16_t)inode_entry[15 + i * 2] << 8);
        }
        /* Read indirect block pointer (offset 28 in inode) */
        g_exec_rom_indirect_zone = (uint16_t)inode_entry[28]
                                 | ((uint16_t)inode_entry[29] << 8);
        g_exec_rom_inode_cached = 1;
        g_exec_rom_cached_inode = inode;
        
    }
    
    /* Calculate starting zone from current file position */
    uint32_t pos = g_exec_pos;
    uint16_t start_zone = pos >> 9;       /* pos / 512 */
    uint16_t zone_offset = pos & 0x1FF;   /* pos % 512 */
    
    /* Build the zone list (direct + indirect) once per open.  The raw zones now
     * come from the inode cache, which is what the first read of the file
     * already populated, so a steady-state read reaches here only when the
     * list has not been built yet at all. */
    /* The zone list is built once per open, straight into the cache: the
     * steady-state read does no metadata work at all. */
    
    if (!g_exec_zone_list_valid) {
    uint16_t num_zones = 0;
        for (int i = 0; i < 7; i++) {
            if (g_exec_rom_zones[i] != 0) {
                g_exec_zone_list[num_zones++] = g_exec_rom_zones[i];
            }
        }
        if (g_exec_rom_indirect_zone != 0) {
            if (exec_rom_read_block(g_exec_rom_indirect_zone, indirect_block) == 0) {
                uint16_t ind_count = 0;
                for (int i = 0; i < 256; i++) {
                    uint16_t z = indirect_block[i * 2] | (indirect_block[i * 2 + 1] << 8);
                    if (z != 0) {
                        g_exec_zone_list[num_zones++] = z;
                        ind_count++;
                    }
                }
            } else {
            }
        }
        g_exec_zone_count = num_zones;
        g_exec_zone_list_valid = 1;
    }
    /* The steady-state read uses the cache, not the list just built: they are
     * the same bytes, and the point of the cache is that the next call does
     * not have to rebuild anything. */
    
    /* Read file data from zones starting at start_zone */
    size_t total_read = 0;
    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = count;
    
    
    for (uint16_t zi = start_zone; zi < g_exec_zone_count && remaining > 0; zi++) {
        uint16_t zone = g_exec_zone_list[zi];
        if (zone == 0) break;
        
        uint8_t block[512];
        if (exec_rom_read_block(zone, block) != 0) {
            return -1;
        }
        
        uint16_t zone_remaining = 512;
        uint8_t *src = block;
        
        /* Skip bytes before zone_offset in first zone */
        if (zi == start_zone && zone_offset > 0) {
            src += zone_offset;
            zone_remaining -= zone_offset;
        }
        
        uint16_t to_read = (remaining < zone_remaining) ? remaining : zone_remaining;
        memcpy(dst, src, to_read);
        dst += to_read;
        remaining -= to_read;
        total_read += to_read;
    }
    
    
    if (result) *result = total_read;
    return 0;
}

static int exec_rom_fstat(int inode, void *stat)
{
    
    uint8_t inode_block[512];
    uint8_t inode_block_num = 3 + (inode >> 4);  /* 16 inodes per block */
    if (exec_rom_read_block(inode_block_num, inode_block) != 0) {
        return -1;
    }
    
    uint8_t *inode_entry = &inode_block[(inode & 0xF) * 32];
    memcpy(stat, inode_entry, 32);
    return 0;
}

/* Enable IPC mode for exec_loader (called after FS server is running) */
void muzix_exec_enable_ipc(void)
{
    g_exec_use_ipc = true;
}

void muzix_exec_set_ipc_mode(bool use_ipc)
{
    g_exec_use_ipc = use_ipc;
}

/* The text window is 16 KB ($8000-$BFFF, window 2) and the loader copies the
 * whole flat image - code AND _DATA - to MUZIX_EXEC_TEXT_ADDR, so 0x2000 was
 * not a hardware limit but a guess that left no room for initialised data.
 * shl.lk used to pin _DATA at 0x9000, which is inside shell's 0x1833 bytes of
 * code; the data image was therefore emitted over live instructions
 * (_sys_times at 0x9129, _sys_signal at 0x914A, _sys_open at 0x9191) and the
 * linker's overlapping-data behaviour went unreported. With _DATA placed after
 * _CODE the image reaches ~0x7A00, so the cap has to be the window size. */
#define MUZIX_EXEC_TEXT_SIZE     0x4000u
#define MUZIX_EXEC_MAX_READ      256u
#define MUZIX_EXEC_TEXT_ADDR     0x8000u
/* The process stack lives at the top of WINDOW 1, not window 3.
 *
 * Window 3 is the kernel's own: the tail of _CODE, all of _DATA and the C
 * stack. zeta_copy_kernel_to_user() refuses a window-3 destination because
 * staging through g_copy_staging and the copy callback's own frame both live
 * in window 3, so remapping it would destroy the code doing the remapping -
 * and argv is written to 0xFFxx, so exec_load could never complete at all.
 * With the stack in window 1 no window-3 copy is ever needed.
 *
 * Window 1 is also the ROM-read scratch window, which is safe: the syscall
 * hooks install the kernel map on entry and restore the process map on exit,
 * so a ROM read during a syscall saves and restores the kernel's page 0x21
 * rather than the process's stack. */
#define MUZIX_EXEC_STACK_TOP     0x7FFFu

/* Where a freshly exec'd process is told its argc and argv.  lib/crt0.s reads
 * these two words on its way into main(); keep the two in step.  They sit in
 * the gap between the argument strings and the pointer array that
 * muzix_exec_copy_argv() builds, so nothing else can be using them. */
#define MUZIX_EXEC_ARGC_ADDR     0x7FF0u
#define MUZIX_EXEC_ARGV_ADDR     0x7FEEu
/* Where the process is told to start its stack.
 *
 * crt0 used to hardcode `ld sp,#0x7FF0`, which is only correct if nothing
 * lives between 0x7FF0 and the top of window 1.  The argument block does: the
 * pointer array sits just under 0x7FFF and the strings run down from there.  A
 * program with a frame - `cat` alone has a 512-byte buffer - then grows its
 * stack straight over the strings it was just handed and reads rubbish.
 *
 * So the loader owns the stack pointer: it places the block and publishes the
 * first address below it.  The stack then grows down into empty window 1 and
 * cannot reach the arguments. */
#define MUZIX_EXEC_SP_ADDR       0x7FECu
#define MUZIX_EXEC_STACK_SIZE    0x1000u

/* Identify which stage rejected the load. Without this every failure looks
 * identical (-1) and the exec path is undiagnosable from the console.
 * A single helper keeps the instrumentation to ~5 bytes per site: the code
 * segment has under 700 bytes of headroom (see tools/check_kernel_layout.rb). */
static void exec_fail(char tag)
{
}
 
static int muzix_exec_copy_argv(muzix_mm_service_t *mm, const process_map_t *dst_map,
                                 uint16_t stack_top, int *out_argc, uint16_t *out_argv_ptr);
 
extern uint8_t muzix_userspace_banks[4];
extern void muzix_set_entry_point(uint16_t entry);

/* The boot reading for uptime.  Non-static because the syscall handler hands it
 * back to callers; see exec_loader.h for why it is taken here.  Eight bytes of
 * _DATA, and the state byte is what makes "tried once" and "tried and refused"
 * different things. */
muzix_rtc_time_t muzix_boot_rtc;
uint8_t muzix_boot_rtc_state;

/* Load a binary into a slot chosen by the caller.
 *
 * The syscall layer reaches the loader through a function pointer that takes
 * (path, argv, slot) - it has to, because the slot is the process asking for
 * the exec and the whole point of a context switch is that the request comes
 * from the child. muzix_exec_load() takes no arguments and reads the slot out of
 * the global argument block instead, so the pointer could not be pointed at it
 * directly.
 *
 * Worse, it was never pointed at anything: muzix_zeta_syscall_runtime_init()
 * sets userspace.kernel and userspace.fs and nothing ever set exec_loader, so
 * it stayed NULL. The handler's `if (ctx->exec_loader && ...)` was therefore
 * false, the load was skipped entirely, and exec reported success without
 * loading anything - the handoff then re-entered the *previous* program from
 * its entry point. That is why `ls` printed the shell's banner and did nothing
 * else: it never ran.
 *
 * Setting the slot here rather than leaving it to the caller is also what keeps
 * the child and parent apart. Left at whatever boot set it to, every exec
 * replaced the parent. */
int muzix_exec_load_for_slot(const char *path, const char *const *argv,
                             int slot)
{
    if (!path) {
        return -1;
    }
    g_muzix_exec_load_args.slot = slot;
    g_muzix_exec_load_args.path = path;
    g_muzix_exec_load_args.argv = argv;
    return muzix_exec_load();
}
int muzix_exec_load(void)
{
    /* Manual 16-bit copy to avoid SDCC codegen bug on struct field access */
    {
        uint8_t *src = (uint8_t *)&g_muzix_exec_load_args + 2;
        uint8_t *dst = (uint8_t *)&g_exec_mm;
        dst[0] = src[0];
        dst[1] = src[1];
    }
    /* Fallback: _DATA not zeroed at startup, so struct may have garbage */
    if (!g_exec_mm) {
        extern muzix_mm_service_t g_mm_service;
        g_exec_mm = &g_mm_service;
    }
    g_exec_proc_table = (muzix_kernel_proc_table_t *)g_muzix_exec_load_args.processes;
    g_exec_slot = g_muzix_exec_load_args.slot;
    g_exec_path = g_muzix_exec_load_args.path;
    g_exec_argv = g_muzix_exec_load_args.argv;

    /* Reset ROM inode cache for new exec */
    g_exec_rom_inode_cached = 0;
    g_exec_rom_file_size = 0;
    g_exec_rom_indirect_zone = 0;
    g_exec_pos = 0;
    for (int i = 0; i < 7; i++) {
        g_exec_rom_zones[i] = 0;
    }

    /* Copy argv pointer array immediately to avoid kernel stack corruption.
     *
     * Every slot is cleared first, and only then filled.  A short command
     * after a longer one used to inherit the longer one's tail: the loop
     * stops at the first NULL it finds in the caller's array and the single
     * `g_exec_argv_ptrs[15] = NULL` below only ever cleared the last slot, so
     * `ls -l x` followed by `uptime` handed the loader ["uptime", "-l", "x"]
     * - muzix_exec_copy_argv() counts argc by walking this array until a NUL,
     * and nothing had written one. */
    for (int i = 0; i < MUZIX_EXEC_ARGV_PTRS; i++) {
        g_exec_argv_ptrs[i] = NULL;
    }
    if (g_exec_argv) {
        for (int i = 0; i < MUZIX_EXEC_ARGV_PTRS && g_exec_argv[i]; i++) {
            g_exec_argv_ptrs[i] = g_exec_argv[i];
        }
    }

    /* The command name for the process table, from the path this is loading.
     *
     * Here rather than in the exec syscall, because this is the one function
     * every exec goes through: the shell the kernel starts at boot calls
     * muzix_exec_load() directly and never reaches a syscall handler, so a name
     * filled in the syscall left the one process that is always on screen
     * unnamed.  Filling it before the open means a load that goes on to fail
     * still leaves the name of what was attempted, which is what a reader wants
     * to see on a slot that is about to become free anyway. */
    muzix_proc_set_name(g_exec_proc_table, g_exec_slot, g_exec_path);

    /* The clock reading at boot, taken on the first exec and never again.
     *
     * Uptime is the current DS1302 reading minus the reading at boot. The
     * scheduler tick supports deadlines and process accounting, but is not the
     * wall-clock source. The baseline therefore has to be taken before any
     * program can ask for one.
     *
     * The first exec is the boot exec - kernel_main() calls this function
     * directly, before the loop that hands the first process to userspace, and
     * no syscall happens on the way - so "taken here" and "taken at boot" are
     * the same statement.  The state is set to REFUSED before the read rather
     * than after it, so a chip that is not answering is not asked again on the
     * next exec: that next exec is the user's `ls`, minutes or hours later, and
     * a baseline taken then would report the time since that command instead of
     * the time since power-on.  A machine with no baseline says so. */
    if (muzix_boot_rtc_state == MUZIX_BOOT_RTC_UNTRIED) {
        muzix_boot_rtc_state = MUZIX_BOOT_RTC_REFUSED;
        if (muzix_zeta_rtc_get(&muzix_boot_rtc) == 0) {
            muzix_boot_rtc_state = MUZIX_BOOT_RTC_GOOD;
        }
    }

g_exec_handle = exec_open_path(g_exec_path, 0);
    if (g_exec_handle < 0) {
        exec_fail('A');
        return -1;
    }

    muzix_fs_inode_t inode;
    int fstat_rc = exec_fstat(g_exec_handle, &inode);
    if (fstat_rc != 0) {
        exec_close(g_exec_handle);
        exec_fail('B');
        return -1;
    }

    g_exec_file_size = inode.size;

    if (g_exec_file_size < 4 || g_exec_file_size > MUZIX_EXEC_TEXT_SIZE) {
        exec_close(g_exec_handle);
        exec_fail('C');
        return -1;
    }

if (exec_read(g_exec_handle, g_exec_load_buf, 4,
                                &g_exec_result) != 0 || g_exec_result != 4) {
        exec_close(g_exec_handle);
        exec_fail('D');
        return -1;
    }

    if (g_exec_load_buf[0] != 0x4d || g_exec_load_buf[1] != 0x5a) {
        exec_close(g_exec_handle);
        exec_fail('E');
        return -1;
    }

    /* MZ entry point is at offset 2-3 in the 4-byte header */
    g_exec_entry_point = (uint16_t)(g_exec_load_buf[2] |
                                    ((uint16_t)g_exec_load_buf[3] << 8));

    /* Two pages, not three. The map needs exactly one for the text and one for
     * the stack; the third was allocated, never placed in the map and never
     * released on the success path, so every successful exec burned a page for
     * good. With ~27 allocatable pages that is 9 execs before exec fails
     * permanently. Error paths below free what they allocated. */
    uint8_t exec_pages[2];
    for (int i = 0; i < 2; i++) {
        int alloc_result = muzix_mm_alloc_page(g_exec_mm, &exec_pages[i]);
        if (alloc_result != 0) {
            for (int j = 0; j < i; j++) {
                muzix_mm_free_page(g_exec_mm, exec_pages[j]);
            }
            exec_close(g_exec_handle);
            exec_fail('F');
            return -1;
        }
    }
    g_exec_text_page = exec_pages[0];


    process_map_t proc_map;

    /* The Zeta V2 maps each 16 KiB bank register as a distinct window:
     *   window 0 = 0x0000-0x3FFF, window 1 = 0x4000-0x7FFF
     *   window 2 = 0x8000-0xBFFF, window 3 = 0xC000-0xFFFF
     *
     * A single physical page therefore appears at four different Z80 offsets,
     * so the stack in window 1 and the text in window 2 are NOT the same
     * window.  The layout this project loads is
     *   _CODE = 0x8004 (window 2)   - so text lives in window 2's page
     *   stack 0x7FFF (window 1)     - and it needs its own page
     * hence two pages, one per window.  Pointing both windows at the text page
     * leaves the stack reading and writing the code image: the first call
     * overwrites the program it is running.
     */
    proc_map.pages[0] = MUZIX_ZETA_KERNEL_BANK_0;
    proc_map.pages[1] = exec_pages[1];
    proc_map.pages[2] = exec_pages[0];
    proc_map.pages[3] = MUZIX_ZETA_KERNEL_BANK_3;

    g_exec_pos = 4;
    g_exec_offset = 0;


    /* The slot's state as this exec found it, saved before anything is
     * published.
     *
     * The map used to be installed here, before the text was copied, on the
     * grounds that the copy path resolves its destination through the slot's
     * map.  It does not: every copy below is handed `&proc_map` directly, and
     * the MM entry point they reach resolves the destination through that map
     * and through muzix_live_banks - nothing in the copy path reads the slot.
     * What the early install did buy was a failure path that left the slot
     * active with a map naming two pages that had just been freed: 'G' and 'H'
     * free exec_pages[] and return -1, and a slot whose map points at pages the
     * allocator has handed to somebody else is a slot that runs on another
     * process's memory.  muzix_handle_exec() answers a non-zero rc by returning
     * -1 to the caller, so the exec'ing process is dispatched again from its
     * recorded resume point - it needs the image it was running, not a new one.
     *
     * So the map is published only once the whole image has landed, and the
     * 'G'/'H' paths put back what they found. */
    muzix_proc_slot_t *pslot = &g_exec_proc_table->slots[g_exec_slot];
    process_map_t prev_map = pslot->map;
    uint8_t prev_active = pslot->active;

while (g_exec_pos < g_exec_file_size) {
        g_exec_chunk = MUZIX_EXEC_MAX_READ;
        if (g_exec_pos + g_exec_chunk > g_exec_file_size) {
            g_exec_chunk = g_exec_file_size - g_exec_pos;
        }
        if (exec_read(g_exec_handle, g_exec_load_buf, g_exec_chunk,
                                    &g_exec_result) != 0 || g_exec_result == 0) {
            for (int i = 0; i < 2; i++) {
                muzix_mm_free_page(g_exec_mm, exec_pages[i]);
            }
            pslot->map = prev_map;
            pslot->active = prev_active;
            exec_close(g_exec_handle);
            exec_fail('G');
            return -1;
        }

if (muzix_boot_copy_kernel_to_user(g_exec_mm, &proc_map,
                                                (uint16_t)(MUZIX_EXEC_TEXT_ADDR + g_exec_offset),
                                                g_exec_load_buf, g_exec_result) != 0) {
            for (int i = 0; i < 2; i++) {
                muzix_mm_free_page(g_exec_mm, exec_pages[i]);
            }
            pslot->map = prev_map;
            pslot->active = prev_active;
            exec_close(g_exec_handle);
            exec_fail('H');
            return -1;
        }
        g_exec_offset += g_exec_result;
        g_exec_pos += g_exec_result;
    }

    exec_close(g_exec_handle);

if (muzix_exec_copy_argv(g_exec_mm, &proc_map,
                              MUZIX_EXEC_STACK_TOP, &g_exec_argc, &g_exec_argv_ptr) != 0) {
        for (int i = 0; i < 2; i++) {
            muzix_mm_free_page(g_exec_mm, exec_pages[i]);
        }
        exec_fail('I');
        return -1;
    }

    /* The whole image is in place, so publish.  Everything below this point
     * either succeeds or leaves the slot exactly as the pre-exec save above
     * found it: an argv that cannot be written ('I') has published nothing,
     * because the slot is still carrying its previous map and active flag. */
    pslot->map = proc_map;
    pslot->active = 1;

    pslot->entry_point = g_exec_entry_point;
    pslot->stack_ptr = MUZIX_EXEC_STACK_TOP;
    pslot->argc = g_exec_argc;
    pslot->argv_ptr = g_exec_argv_ptr;
    pslot->p_flags &= (uint8_t)~MUZIX_PROC_NO_MAP;

    pslot->mproc.mp_pid = (uint8_t)pslot->pid;
    pslot->mproc.mp_flags = 0;
    muzix_mproc_set_seg(&pslot->mproc, MUZIX_SEG_TEXT, 0, g_exec_text_page,
                        (uint16_t)(g_exec_file_size - 4));
    muzix_mproc_set_seg(&pslot->mproc, MUZIX_SEG_DATA, 0, g_exec_text_page,
                        (uint16_t)(g_exec_file_size - 4));
    /* The stack is its own page, the one the published map puts in window 1
     * (proc_map.pages[1], which is exec_pages[1] and where MUZIX_EXEC_STACK_TOP
     * of 0x7FFF actually lives).  This named the text page under a
     * single-page-userspace comment, so every consumer that converts the mproc
     * back into a map resolved the stack to window 2: mm/mm_copy.c's
     * map-to-map copy and the SYS_COPY handler in kernel/system_service.c both
     * go through muzix_mproc_to_zeta_map(), which puts STACK in window 1 and
     * TEXT in window 2. */
    muzix_mproc_set_seg(&pslot->mproc, MUZIX_SEG_STACK, 0, exec_pages[1],
                        MUZIX_EXEC_STACK_SIZE);

    /* A forked child may already own its private copy of the parent's stack.
     * The new map is now complete, so release that replaced image before
     * recording ownership of the pages allocated for this exec. */
    muzix_proc_table_release_pages(g_exec_proc_table, g_exec_slot, g_exec_mm);

    /* Both pages, in the order they were allocated: exec_pages[0] is the text
     * page (window 2) and exec_pages[1] is the stack page (window 1).  Only the
     * first was recorded here, so muzix_proc_table_release_pages() - and with
     * it muzix_proc_table_exit() - returned the text page and left the stack
     * page marked as owned by a slot nobody will exit again.  With 27 allocatable
     * pages that is one page lost per successful exec, which is the same class of
     * leak as the third page the allocation above once burned. */
    muzix_proc_table_set_owned(g_exec_proc_table, g_exec_slot, exec_pages, 2);

    muzix_set_entry_point(g_exec_entry_point);
    muzix_userspace_banks[0] = proc_map.pages[0];
    muzix_userspace_banks[1] = proc_map.pages[1];
    muzix_userspace_banks[2] = proc_map.pages[2];
    muzix_userspace_banks[3] = proc_map.pages[3];

    /* Mark process ready to run. exec_loader dual-path: before FS server
     * g_exec_proc_table is the kernel proc table; after, it's the same table
     * reached via IPC. Either way, ready the slot. */
    muzix_proc_table_ready(g_exec_proc_table, g_exec_slot);

    muzix_trace_stack_mark();

    /* No bank state is written from here.  The kernel is not allowed to write
     * bank registers directly, and the shadow it would have updated
     * (_user_banks in bank_io.s) was never read on the boot path: the
     * userspace handoff in kernel_entry.s loads muzix_userspace_banks, written
     * just above.  Both shadows and the two entry points that used them were
     * removed on 2026-10-02 as dead - they had no callers, and leaving four
     * bank registers' worth of storage behind reads as a live handshake.  The
     * map that matters is the one published on the slot (pslot->map, set once
     * the image was copied) and mirrored into muzix_userspace_banks. */
    return 0;
}

/* Copies argv strings and the argv pointer array into the *destination
 * process's* stack page.
 *
 * The destination map is given as a map rather than resolved from a slot,
 * because the loader has the map and the slot does not have it yet: the map is
 * published on the slot only after the image and this block have both landed, so
 * a copy that asked the slot for its map would read the exec'ing process's.
 *
 * Passing slot 0's map here is the failure this once had: it is the kernel slot,
 * {0x20,0x21,0x22,0x23}, so the writes landed in the kernel's own address
 * space. That put "shell\0" and the argv word into the live kernel stack
 * around 0xFF33/0xFFFD, overwriting a return address with the bytes
 * 73 68 65 6C 6C 00. The next `ret` then jumped into the 0x0000-0x0098 boot
 * stub, whose `jp 0x0098` re-entered kernel_main: the machine rebooted in a loop
 * instead of starting the shell. */
static int muzix_exec_copy_argv(muzix_mm_service_t *mm, const process_map_t *dst_map,
                           uint16_t stack_top, int *out_argc, uint16_t *out_argv_ptr)
{
    if (!mm || !out_argc || !out_argv_ptr) {
        return -1;
    }

    const char *const *argv = g_exec_argv_ptrs;
    int argc = 0;
    const char *const *arg = argv;
    /* Bounded by the array, not only by the NUL: a command line of sixteen
     * arguments fills every slot above, and a walk that stopped solely at a
     * terminator would read the _DATA object that follows the array. */
    while (argc < MUZIX_EXEC_ARGV_PTRS && *arg) {
        argc++;
        arg++;
    }


if (argc == 0) {
        uint16_t zero = 0;
        uint16_t argv_word = stack_top;
        *out_argc = 0;
        *out_argv_ptr = stack_top;
        /* main() still gets a real argv array, just an empty one. */
        muzix_boot_copy_kernel_to_user(mm, dst_map, MUZIX_EXEC_ARGC_ADDR,
                                      (const uint8_t *)&zero, 2);
        muzix_boot_copy_kernel_to_user(mm, dst_map, MUZIX_EXEC_ARGV_ADDR,
                                      (const uint8_t *)&argv_word, 2);
        {
            uint16_t sp_word = (uint16_t)(stack_top - 16);
            muzix_boot_copy_kernel_to_user(mm, dst_map, MUZIX_EXEC_SP_ADDR,
                                          (const uint8_t *)&sp_word, 2);
        }
        return 0;
    }

    uint16_t argv_ptr = stack_top;
    uint8_t *stack_base = (uint8_t *)(stack_top - argc * 2 - 256);
    uint16_t argv_array_addr = (uint16_t)(stack_top - argc * 2);

    /* Place each string and record where it went, in the same pass.
     *
     * These were two loops.  The first walked down the stack writing the
     * strings, the second walked back up writing pointers - and started from
     * wherever the first had finished, which is the *lowest* address.  So the
     * pointers came out in the reverse order: for ["cat", "readme"] the strings
     * were laid at 0x7EF7 and 0x7EF0, argv[0] was given 0x7EF0 and argv[1] 0x7EF4.
     * `cat readme` therefore read the wrong argument, and the second one pointed
     * into the gap above it where nothing had been written.
     *
     * Recording the address where the string is actually placed is the only
     * ordering that cannot drift from the placement. */
for (int i = 0; i < argc; i++) {
        const char *str = argv[i];
        uint16_t str_len = 0;
        while (str[str_len]) str_len++;
        str_len++;

        stack_base -= str_len;
        if (muzix_boot_copy_kernel_to_user(mm, dst_map, (uint16_t)stack_base,
                                          (const uint8_t *)str, str_len) != 0) {
            return -1;
        }

        uint16_t str_addr = (uint16_t)stack_base;
        if (muzix_boot_copy_kernel_to_user(mm, dst_map, argv_array_addr + i * 2,
                                          (const uint8_t *)&str_addr, 2) != 0) {
            return -1;
        }
    }

    *out_argc = argc;
    *out_argv_ptr = argv_array_addr;

    /* Hand argc and argv to the program where it can find them.
     *
     * A process is entered at its entry point with a fresh stack, so it has no
     * way to learn them otherwise - and crt0 used to push a hardcoded
     * argc = 0, argv = NULL, which made every program see no arguments at all.
     * `cat readme` printed its usage line and exited, and the shell reported
     * the command as failed.
     *
     * Two fixed words just below the top of window 1, in the gap the strings
     * are kept clear of: the strings start at stack_top - argc*2 - 256 and grow
     * down, while the pointer array sits at stack_top - argc*2, so the region
     * between them is free for any argc a process could plausibly have.
     * lib/crt0.s reads exactly these two addresses. */
{
        uint16_t argc_word = (uint16_t)argc;
        uint16_t argv_word = argv_array_addr;
        /* stack_base now points at the lowest byte the strings occupy, because
         * the single placement loop walked down to it.  The program's stack
         * starts below that. */
        uint16_t sp_word = (uint16_t)(stack_base - 16);
        if (muzix_boot_copy_kernel_to_user(mm, dst_map,
                                          MUZIX_EXEC_ARGC_ADDR,
                                          (const uint8_t *)&argc_word,
                                          2) != 0) {
            return -1;
        }
        if (muzix_boot_copy_kernel_to_user(mm, dst_map,
                                          MUZIX_EXEC_ARGV_ADDR,
                                          (const uint8_t *)&argv_word,
                                          2) != 0) {
            return -1;
        }
        if (muzix_boot_copy_kernel_to_user(mm, dst_map,
                                          MUZIX_EXEC_SP_ADDR,
                                          (const uint8_t *)&sp_word,
                                          2) != 0) {
            return -1;
        }
    }

    return 0;
}
