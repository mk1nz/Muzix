#include "exec_loader.h"

#include <string.h>

#include "../fs/fs_service.h"
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
    muzix_trace_write("EXEC FAIL ");
    muzix_trace_putc((uint8_t)tag);
    muzix_trace_write("\r\n");
}

int muzix_exec_copy_argv(muzix_mm_service_t *mm, uint8_t dst_proc,
                         const char *const *argv,
                         uint16_t stack_top, int *out_argc, uint16_t *out_argv_ptr);

static uint8_t g_exec_load_buf[256];
static int g_exec_handle;
static int g_exec_result;
static int g_exec_pos;
static int g_exec_offset;
static int g_exec_chunk;
static int g_exec_file_size;
static uint8_t g_exec_text_page;
static uint8_t g_exec_stack_page;
static uint16_t g_exec_entry_point;
static muzix_fs_service_t *g_exec_fs;
static muzix_mm_service_t *g_exec_mm;
static muzix_kernel_proc_table_t *g_exec_proc_table;
static int g_exec_slot;
static const char *g_exec_path;
static const char *const *g_exec_argv;
static int g_exec_argc;
static uint16_t g_exec_argv_ptr;

extern uint16_t muzix_userspace_entry;
extern uint8_t muzix_userspace_banks[4];

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
    g_exec_fs = g_muzix_exec_load_args.fs;
    g_exec_mm = g_muzix_exec_load_args.mm;
    g_exec_proc_table = (muzix_kernel_proc_table_t *)g_muzix_exec_load_args.processes;
    g_exec_slot = g_muzix_exec_load_args.slot;
    g_exec_path = g_muzix_exec_load_args.path;
    g_exec_argv = g_muzix_exec_load_args.argv;

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

    g_exec_handle = muzix_fs_service_open_path(g_exec_fs, g_exec_path);
    if (g_exec_handle < 0) {
        exec_fail('A');
        return -1;
    }

    muzix_fs_inode_t inode;
    int fstat_rc = muzix_fs_service_fstat(g_exec_fs, g_exec_handle, &inode);
    if (fstat_rc != 0) {
        muzix_fs_service_close(g_exec_fs, g_exec_handle);
        exec_fail('B');
        return -1;
    }

    g_exec_file_size = inode.size;

    if (g_exec_file_size < 4 || g_exec_file_size > MUZIX_EXEC_TEXT_SIZE) {
        muzix_fs_service_close(g_exec_fs, g_exec_handle);
        exec_fail('C');
        return -1;
    }

    if (muzix_fs_service_read(g_exec_fs, g_exec_handle, g_exec_load_buf, 4,
                              &g_exec_result) != 0 || g_exec_result != 4) {
        muzix_fs_service_close(g_exec_fs, g_exec_handle);
        exec_fail('D');
        return -1;
    }

    if (g_exec_load_buf[0] != 0x4d || g_exec_load_buf[1] != 0x5a) {
        muzix_fs_service_close(g_exec_fs, g_exec_handle);
        exec_fail('E');
        return -1;
    }

    g_exec_entry_point = (uint16_t)(g_exec_load_buf[2] |
                                    ((uint16_t)g_exec_load_buf[3] << 8));

    /* Two pages, not three. The map needs exactly one for the text and one for
     * the stack; the third was allocated, never placed in the map and never
     * released on the success path, so every successful exec burned a page for
     * good. With ~27 allocatable pages that is 9 execs before exec fails
     * permanently. Error paths below free what they allocated. */
    uint8_t exec_pages[2];
    for (int i = 0; i < 2; i++) {
        if (muzix_mm_alloc_page(g_exec_mm, &exec_pages[i]) != 0) {
            for (int j = 0; j < i; j++) {
                muzix_mm_free_page(g_exec_mm, exec_pages[j]);
            }
            muzix_fs_service_close(g_exec_fs, g_exec_handle);
            exec_fail('F');
            return -1;
        }
    }
    g_exec_text_page = exec_pages[0];
    g_exec_stack_page = exec_pages[1];

    process_map_t proc_map;

    proc_map.pages[0] = MUZIX_ZETA_KERNEL_BANK_0;
    proc_map.pages[1] = exec_pages[1];
    proc_map.pages[2] = exec_pages[0];
    proc_map.pages[3] = MUZIX_ZETA_KERNEL_BANK_3;

    g_exec_pos = 4;
    g_exec_offset = 0;

    /* Publish the map before the text is copied. The copy path resolves the
     * destination through this slot's process map, and until it is set the
     * slot still carries the kernel map inherited from fork -- which would put
     * the destination window on the kernel's own code page. */
    muzix_proc_slot_t *pslot = &g_exec_proc_table->slots[g_exec_slot];
    pslot->map = proc_map;

    while (g_exec_pos < g_exec_file_size) {
        g_exec_chunk = MUZIX_EXEC_MAX_READ;
        if (g_exec_pos + g_exec_chunk > g_exec_file_size) {
            g_exec_chunk = g_exec_file_size - g_exec_pos;
        }
        if (muzix_fs_service_read(g_exec_fs, g_exec_handle, g_exec_load_buf, g_exec_chunk,
                                  &g_exec_result) != 0 || g_exec_result == 0) {
            for (int i = 0; i < 2; i++) {
                muzix_mm_free_page(g_exec_mm, exec_pages[i]);
            }
            muzix_fs_service_close(g_exec_fs, g_exec_handle);
            exec_fail('G');
            return -1;
        }

        if (muzix_mm_copy_kernel_to_user(g_exec_mm, g_exec_slot,
                                         (uint16_t)(MUZIX_EXEC_TEXT_ADDR + g_exec_offset),
                                         g_exec_load_buf, g_exec_result) != 0) {
            for (int i = 0; i < 2; i++) {
                muzix_mm_free_page(g_exec_mm, exec_pages[i]);
            }
            muzix_fs_service_close(g_exec_fs, g_exec_handle);
            exec_fail('H');
            return -1;
        }
        g_exec_offset += g_exec_result;
        g_exec_pos += g_exec_result;
    }

    muzix_fs_service_close(g_exec_fs, g_exec_handle);

    if (muzix_exec_copy_argv(g_exec_mm, (uint8_t)g_exec_slot, g_exec_argv,
                             MUZIX_EXEC_STACK_TOP, &g_exec_argc, &g_exec_argv_ptr) != 0) {
        for (int i = 0; i < 2; i++) {
            muzix_mm_free_page(g_exec_mm, exec_pages[i]);
        }
        exec_fail('I');
        return -1;
    }

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
    muzix_mproc_set_seg(&pslot->mproc, MUZIX_SEG_STACK, 0, g_exec_stack_page,
                        MUZIX_EXEC_STACK_SIZE);

    /* A forked child may already own its private copy of the parent's stack.
     * The new map is now complete, so release that replaced image before
     * recording ownership of the text and stack pages allocated for this exec.
     * Doing this only after loading succeeds keeps failed execs able to exit
     * safely on the pages they still own. */
    muzix_proc_table_release_pages(g_exec_proc_table, g_exec_slot, g_exec_mm);

    /* The slot now owns these two pages, and only these. muzix_proc_table_exit()
     * returns them to the allocator. */
    muzix_proc_table_set_owned(g_exec_proc_table, g_exec_slot, exec_pages, 2);

    muzix_userspace_entry = g_exec_entry_point;
    muzix_userspace_banks[0] = proc_map.pages[0];
    muzix_userspace_banks[1] = proc_map.pages[1];
    muzix_userspace_banks[2] = proc_map.pages[2];
    muzix_userspace_banks[3] = proc_map.pages[3];

    muzix_trace_stack_mark();

    /* No bank state is written from here.  The kernel is not allowed to write
     * bank registers directly, and the shadow it would have updated
     * (_user_banks in bank_io.s) was never read on the boot path: the
     * userspace handoff in kernel_entry.s loads muzix_userspace_banks, written
     * just above.  Both shadows and the two entry points that used them were
     * removed on 2026-10-02 as dead - they had no callers, and leaving four
     * bank registers' worth of storage behind reads as a live handshake.  The
     * map that matters is the one published on the slot (pslot->map, set
     * before the text was copied) and mirrored into muzix_userspace_banks. */
    return 0;
}

/* Copies argv strings and the argv pointer array into the *destination
 * process's* stack page.
 *
 * The destination must be given as a process SLOT, because the copy is routed
 * through the MM service, which resolves the destination bank map from the slot.
 * Passing 0 here means slot 0 -- the kernel slot, whose map is
 * {0x20,0x21,0x22,0x23} -- so the writes land in the kernel's own address
 * space. That put "shell\0" and the argv word into the live kernel stack
 * around 0xFF33/0xFFFD, overwriting a return address with the bytes
 * 73 68 65 6C 6C 00. The next `ret` then jumped into the 0x0000-0x0098 boot
 * stub, whose `jp 0x0098` re-entered kernel_main: the machine rebooted in a loop
 * instead of starting the shell. */
int muzix_exec_copy_argv(muzix_mm_service_t *mm, uint8_t dst_proc,
                         const char *const *argv,
                         uint16_t stack_top, int *out_argc, uint16_t *out_argv_ptr)
{
    if (!mm || !argv || !out_argc || !out_argv_ptr) {
        return -1;
    }

    int argc = 0;
    const char *const *arg = argv;
    while (*arg) {
        argc++;
        arg++;
    }

    if (argc == 0) {
        uint16_t zero = 0;
        uint16_t argv_word = stack_top;
        *out_argc = 0;
        *out_argv_ptr = stack_top;
        /* main() still gets a real argv array, just an empty one. */
        muzix_mm_copy_kernel_to_user(mm, dst_proc, MUZIX_EXEC_ARGC_ADDR,
                                     (const uint8_t *)&zero, 2);
        muzix_mm_copy_kernel_to_user(mm, dst_proc, MUZIX_EXEC_ARGV_ADDR,
                                     (const uint8_t *)&argv_word, 2);
        {
            uint16_t sp_word = (uint16_t)(stack_top - 16);
            muzix_mm_copy_kernel_to_user(mm, dst_proc, MUZIX_EXEC_SP_ADDR,
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
        if (muzix_mm_copy_kernel_to_user(mm, dst_proc, (uint16_t)stack_base,
                                         (const uint8_t *)str, str_len) != 0) {
            return -1;
        }

        uint16_t str_addr = (uint16_t)stack_base;
        if (muzix_mm_copy_kernel_to_user(mm, dst_proc, argv_array_addr + i * 2,
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
        if (muzix_mm_copy_kernel_to_user(mm, dst_proc,
                                         MUZIX_EXEC_ARGC_ADDR,
                                         (const uint8_t *)&argc_word,
                                         2) != 0 ||
            muzix_mm_copy_kernel_to_user(mm, dst_proc,
                                         MUZIX_EXEC_ARGV_ADDR,
                                         (const uint8_t *)&argv_word,
                                         2) != 0 ||
            muzix_mm_copy_kernel_to_user(mm, dst_proc,
                                         MUZIX_EXEC_SP_ADDR,
                                         (const uint8_t *)&sp_word,
                                         2) != 0) {
            return -1;
        }
    }

    return 0;
}
