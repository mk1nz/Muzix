#include "syscalls.h"
#include "kernel_loop.h"
#include "proc_context.h"
#include "exec_loader.h"
#include "../fs/fs_service.h"
#include "../fs/tty_device.h"
#include "../mm/mproc_model.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/context_switch.h"
#include "../platform/zeta-v2/rtc_ds1302.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * kernel/syscalls.c, brought back against the current API.
 * =====================================================
 *
 * This file did not compile.  It named `zeta_process_table_t`, a type the tree
 * renamed to muzix_kernel_proc_table_t long ago, and it reached for fields on
 * muzix_syscall_context_t that no longer exist (`state`, `table`).  Nothing
 * reported that for years, because the old runner printed "BUILT" in green for
 * a successful compile and a successful compile was counted as a pass: the test
 * that could not have run was the test that read like coverage.
 *
 * WHAT THIS IS FOR
 * ----------------
 * kernel/syscalls.c is the file that did the most damage this week, and every
 * symptom it produced looked like a data problem rather than a control-flow one.
 * The chain, as it actually happened:
 *
 *   - `main(argc, argv)` is a C function and does not read its arguments off the
 *     stack: it receives the first in HL and the second in DE.  lib/crt0.s used
 *     to push both.  Pushing them left argc correct *by accident* - the last
 *     `ld hl,(...)` in crt0 happened to be the argc load - while DE kept
 *     whatever memcpy_init had left there.  On the real image, `cat readme`
 *     entered main with HL=0x0002 and DE=0x92E5, which is s__INITIALIZED: the
 *     middle of the program's own text window.
 *   - So argv[1] was read out of the text window instead of out of the loader's
 *     array, and the filename the kernel was asked to copy pointed below 0x4000.
 *   - A copy out of window 0 is refused, because window 0 is the kernel's.  So
 *     the open was refused - as `zeta: w0 map`, a message about a memory map,
 *     caused by an argument that had been wrong all along.
 *   - The same week: `date` and `uptime` printed "cannot read the clock" about
 *     a clock that had answered every time, because the reading arrived at the
 *     handler through a destination pointer taken from a stale register.
 *
 * None of those is catchable by asserting a return value alone.  What a return
 * value cannot tell you is whether the handler was handed the arguments the
 * caller meant to send.  So every path-taking case below checks *what arrived*:
 * the filesystem seam records the path, the flags and the bytes it was given,
 * the exec-loader seam records the path and the argv it was given, and the
 * memory-manager seam records the address and the length of every transfer the
 * handler asked for.  A return value is checked too, but it is the second
 * question, not the first.
 *
 * THE WINDOW DISCIPLINE
 * ---------------------
 * Four of this week's faults were one refusal in different clothes: window 0
 * ($0000-$3FFF, the boot stub and the syscall vector) and window 3 ($C000-$FFFF,
 * _DATA and the C stack) belong to the kernel, and the copy primitives refuse
 * both.  Windows 1 and 2 belong to the process.  SYS_PROCTAB, SYS_TIMES,
 * SYS_GETRTC, SYS_SETRTC, SYS_READ, SYS_WRITE and the fork stack copy all move
 * bytes across that line, so the cases below drive each of them at window 0,
 * window 3 and window 2 and require the right answer at each.
 *
 * WHAT IS REAL HERE
 * -----------------
 * The handler, the process table, the scheduler's `current`, the memory
 * manager's page allocator, the DS1302 driver and the emulator's own chip model
 * are all the tree's own.  So is the copy boundary underneath: the two seam
 * entry points below delegate to the very zeta_copy_* functions in
 * test_host/zeta_host.c, which is where the window refusals and the staging cap
 * are actually decided.  Those refusals are therefore not restated here - a
 * test that kept its own copy of them would agree with the code by
 * construction.
 *
 * WHAT IS NOT, AND WHY
 * --------------------
 * Two seams, both named in the directive below so the runner stops looking for
 * the real ones:
 *
 * 1. The memory manager's two user-facing copy entry points.  Those do not
 *    copy; they build a message whose dst_vir field carries the *kernel
 *    buffer's address*, and send it through the system task:
 *
 *        msg.dst_vir = (uint16_t)(uintptr_t)kernel_buf;   mm/mm_service.c
 *        kernel_buf = (void *)(uintptr_t)msg->dst_vir;   mm/mm_copy.c
 *
 *    A uint16_t is the whole address because the kernel's C stack lives in
 *    window 3.  On a host the same expression truncates a 64-bit stack address
 *    to its low word and the copy writes to 0x8555.  That is the Z80's address
 *    space being load-bearing in the message ABI, and it is not fixable in a
 *    test.  kernel/test_syscall_setrtc.c documents the same seam for the same
 *    reason, and uses the same flat address space underneath.
 *
 *    mm/mm_copy.c's other three are left REAL - muzix_mm_copy_maps() and the
 *    map-to-map pair call the primitives directly, with no message hop, so
 *    SYS_COPY and the fork stack copy are the tree's own code here.
 *
 *    Eight more memory-manager entry points are restated below, because
 *    excluding mm/mm_service.c from the closure leaves them undefined and none
 *    of them is on a path this test exercises.  Four of the eight do real
 *    work: muzix_mm_alloc_page() and muzix_mm_free_page() forward to the
 *    allocator in platform/zeta-v2/kernel_bank_model.c, which is in the closure
 *    and is the real one, so the fork case's page is a page rather than a
 *    number a test invented.  The other four return -1, so that a future case
 *    needing one reports "not covered here" instead of quietly passing.
 *
 * 2. Twenty-eight filesystem entry points: the twenty-three the handler calls
 *    from a process, and five that only the closure reaches.  They are replaced
 *    not to make them succeed but to make them *report*: a recorder holds the
 *    arguments, so a test can ask what the handler passed down rather than what
 *    it returned.  The filesystem behind every ls and cat is fs/fs_service.c,
 *    which no test reaches for real - see the note on fs/test_fs_service_io.c
 *    at the end of this file.
 *
 * The directive is one line because tools/host_tests.rb reads it with a
 * character class that cannot cross a newline: a name wrapped onto the next
 * line is silently not provided, and the runner then links the real module
 * beside the seam and lets the linker's order decide which wins.  The same
 * rule cuts the other way, which cost an hour once: the literal name of the
 * directive must not appear anywhere above it, or the first one found is the
 * one that is parsed.
 */

/* host-test-provides: muzix_fs_service_pipe muzix_fs_service_ioctl muzix_fs_service_mknod_path muzix_fs_service_open_path_flags muzix_fs_service_close muzix_fs_service_read muzix_fs_service_write muzix_fs_service_seek muzix_fs_service_mkdir_path muzix_fs_service_rmdir_path muzix_fs_service_chdir muzix_fs_service_getcwd muzix_fs_service_unlink_path muzix_fs_service_rename_path muzix_fs_service_stat_path muzix_fs_service_fstat muzix_fs_service_creat_path muzix_fs_service_link_path muzix_fs_service_chmod_path muzix_fs_service_access_path muzix_fs_service_dup muzix_fs_service_readdir muzix_fs_service_sync muzix_fs_service_init muzix_fs_service_attach_volume muzix_fs_service_attach_tty muzix_fs_service_open_path muzix_fs_service_open_root muzix_fs_service_open_tty muzix_mm_copy_user_to_kernel muzix_mm_copy_kernel_to_user muzix_mm_service_init muzix_mm_memory_init muzix_mm_alloc_page muzix_mm_free_page muzix_mm_load_kernel_map muzix_mm_cross_bank_call muzix_mm_copy_page_to_kernel muzix_mm_copy_rom_page_to_kernel */

/* ------------------------------------------------------------------ fixture */

/* test_host/zeta_host.c: the flat 64 KiB standing in for a process's four
 * windows.  One array for all of them, because a banked space needs banks. */
uint8_t *muzix_host_user_space(void);
void muzix_host_user_space_clear(void);

/* test_host/z80_io_host.c and test_host/ds1302_host.c: the driver's port
 * accesses, and the emulator's own chip model behind them. */
void muzix_host_port_attach(void (*write)(uint8_t port, uint8_t value),
                            uint8_t (*read)(uint8_t port));
void muzix_host_ds1302_reset(void);
void muzix_host_ds1302_write(uint8_t port, uint8_t value);
uint8_t muzix_host_ds1302_read(uint8_t port);

/* kernel/kernel_main.c: the cells window-0 assembly reads, and the ones the
 * trap fills on the way in.  The fork handler needs them and nothing sets them
 * on the host, so a test has to - which is itself worth knowing. */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;
extern uint16_t muzix_resume_ret;
extern uint16_t muzix_switch_pending;
extern uint16_t muzix_fork_stack_len;
extern uint8_t muzix_fork_stack_scratch[MUZIX_ZETA_COPY_STAGING];

/* Where the caller says its things are.  Windows 1 and 2 are the process's;
 * 0 and 3 are the kernel's and the copy primitives refuse both. */
#define U_PATH  0x8100u   /* window 2 */
#define U_BUF   0x8200u   /* window 2 */
#define U_ARG1  0x8080u   /* window 2, argv element 0 */
#define U_ARG2  0x8090u   /* window 2, argv element 1 */
#define U_W1    0x4180u   /* window 1 */
#define U_W0    0x0180u   /* window 0: boot stub / _CODE.  Refused. */
#define U_W3    0xFF80u   /* window 3: _DATA / the C stack.  Refused. */

#define CHECK(cond, why) do { if (!(cond)) { return (why); } } while (0)

static muzix_kernel_loop_t g_loop;
static muzix_mm_service_t g_mm;
static muzix_system_task_t g_task;
static muzix_fs_service_t g_fs;
static muzix_syscall_context_t g_kctx;
static muzix_userspace_syscall_context_t g_ctx;
static muzix_mproc_t g_mp[2];
static process_map_t g_kmap = {{MUZIX_ZETA_KERNEL_BANK_0,
                                MUZIX_ZETA_KERNEL_BANK_1,
                                MUZIX_ZETA_KERNEL_BANK_2,
                                MUZIX_ZETA_KERNEL_BANK_3}};
static process_map_t g_maps[2];

/* ------------------------------------------------- the filesystem recorder */

#define SEEN_PATH_MAX (MUZIX_FS_NAME_MAX + 2)
#define SEEN_MAX_CHUNKS 16

typedef struct {
    const char *which;                 /* which entry point was reached */
    int calls;
    int fd;
    uint16_t flags;
    uint16_t mode;
    uint16_t device;
    uint16_t uid;
    uint16_t gid;
    uint16_t umask;
    uint16_t index;
    uint32_t position;
    char path[SEEN_PATH_MAX];
    char path2[SEEN_PATH_MAX];
    /* Every byte handed to read()/write(), in order, across every chunk.  This
     * is what makes the staging chunking observable: one call means the handler
     * asked the MM for more than it can carry, and the call would have failed. */
    uint8_t sink[1024];
    size_t sink_len;
    size_t chunk_len[SEEN_MAX_CHUNKS];
} seen_t;

static seen_t g_seen;

/* What the seam should answer with, per call.  A test sets one of these to
 * force the error path; the defaults are the happy ones. */
static int g_seen_open_fd;
static int g_seen_close_rc;
static int g_seen_seek_rc;
static int g_seen_readdir_rc;
static int g_seen_read_rc;             /* nonzero: read() reports a failure */
static uint8_t g_seen_read_base;       /* first byte read() hands back */
static size_t g_seen_read_left;        /* bytes the "file" has left */
static size_t g_seen_read_pos;         /* offset into the counting pattern */
static int g_seen_readdir_inode;
static char g_seen_getcwd[64];

static void seen_reset(void)
{
    const char *which = g_seen.which;

    memset(&g_seen, 0, sizeof(g_seen));
    g_seen.which = which;
    g_seen_open_fd = 3;
    g_seen_readdir_rc = 0;
    g_seen_read_base = 0x40u;
    g_seen_read_left = (size_t)-1;
    g_seen_read_pos = 0;
    g_seen_getcwd[0] = '/';
}

static void seen_enter(const char *which)
{
    if (g_seen.calls == 0) {
        g_seen.which = which;
    }
    g_seen.calls++;
}

static void seen_path(const char *which, const char *path, const char *path2)
{
    seen_enter(which);
    if (path) {
        strncpy(g_seen.path, path, SEEN_PATH_MAX - 1);
        g_seen.path[SEEN_PATH_MAX - 1] = 0;
    }
    if (path2) {
        strncpy(g_seen.path2, path2, SEEN_PATH_MAX - 1);
        g_seen.path2[SEEN_PATH_MAX - 1] = 0;
    }
}

static int seen_reached(const char *which)
{
    return g_seen.calls > 0 && g_seen.which && strcmp(g_seen.which, which) == 0;
}

/* ------------------------------------------------------ caller-side helpers */

static void put_user(uint16_t addr, const void *src, size_t n)
{
    memcpy(&muzix_host_user_space()[addr], src, n);
}

static void put_str(uint16_t addr, const char *s)
{
    put_user(addr, s, strlen(s) + 1);
}

static void user_get(uint16_t addr, void *dst, size_t n)
{
    memcpy(dst, &muzix_host_user_space()[addr], n);
}

static int user_eq(uint16_t addr, const void *src, size_t n)
{
    return memcmp(&muzix_host_user_space()[addr], src, n) == 0;
}

static int32_t call_user(int32_t num, int32_t a1, int32_t a2, int32_t a3)
{
    return muzix_handle_userspace_syscall(&g_ctx, num, a1, a2, a3);
}

/* --------------------------------------------------------- the exec loader */

/* The handler's only outward dependency that is a plain function pointer, and
 * therefore the one place where "what did the handler receive" can be asked
 * directly.  This is also where last week's argv bug would have been caught
 * without a board: the loader is told the path and the arguments, and both come
 * from the caller's address space. */
static int g_loader_calls;
static char g_loader_path[SEEN_PATH_MAX];
static int g_loader_argc;
static char g_loader_arg0[SEEN_PATH_MAX];
static char g_loader_arg1[SEEN_PATH_MAX];
static int g_loader_slot;

static int test_exec_loader(const char *path, const char *const *argv, int slot)
{
    g_loader_calls++;
    g_loader_slot = slot;
    g_loader_argc = 0;
    g_loader_path[0] = 0;
    g_loader_arg0[0] = 0;
    g_loader_arg1[0] = 0;
    if (path) {
        strncpy(g_loader_path, path, SEEN_PATH_MAX - 1);
        g_loader_path[SEEN_PATH_MAX - 1] = 0;
    }
    if (argv) {
        while (g_loader_argc < 2 && argv[g_loader_argc]) {
            const char *a = argv[g_loader_argc];

            strncpy(g_loader_argc == 0 ? g_loader_arg0 : g_loader_arg1, a,
                    SEEN_PATH_MAX - 1);
            (g_loader_argc == 0 ? g_loader_arg0 : g_loader_arg1)[SEEN_PATH_MAX - 1] = 0;
            g_loader_argc++;
        }
    }
    return 0;
}

/* --------------------------------------------- the memory-manager seam ---- */

typedef struct {
    int calls;                         /* transfers attempted */
    int ok;                            /* transfers the primitive accepted */
    uint16_t addr[SEEN_MAX_CHUNKS];
    uint16_t len[SEEN_MAX_CHUNKS];
    uint16_t max_len;                  /* the longest single transfer asked for */
} xfer_t;

static xfer_t g_to_user;
static xfer_t g_from_user;

static void xfer_note(xfer_t *x, uint16_t addr, uint16_t len)
{
    if (x->calls < SEEN_MAX_CHUNKS) {
        x->addr[x->calls] = addr;
        x->len[x->calls] = len;
    }
    if (len > x->max_len) {
        x->max_len = len;
    }
    x->calls++;
}

static void xfer_reset(xfer_t *x)
{
    memset(x, 0, sizeof(*x));
}

/* Every one of these forwards to the tree's own host model of the copy
 * primitives.  Nothing here decides whether a transfer is allowed - that is
 * zeta_copy_*'s refusal, in test_host/zeta_host.c - so a case below that
 * expects a refusal is exercising the same rule the target runs. */
int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm, uint8_t src_proc,
                                 uint16_t src_vir, void *kernel_buf,
                                 uint16_t len)
{
    if (!kernel_buf) {
        return -1;
    }
    int rc;

    xfer_note(&g_from_user, src_vir, len);
    rc = zeta_copy_user_to_kernel(&mm->zeta,
                                  &g_loop.system.startup.proc_table
                                       .slots[src_proc & (MUZIX_PROC_TABLE_MAX - 1)]
                                       .map,
                                  src_vir, (uint8_t *)kernel_buf, len);
    if (rc == 0) {
        g_from_user.ok++;
    }
    return rc;
}

int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm, uint8_t dst_proc,
                                 uint16_t dst_vir, const void *kernel_buf,
                                 uint16_t len)
{
    if (!kernel_buf) {
        return -1;
    }
    int rc;

    xfer_note(&g_to_user, dst_vir, len);
    rc = zeta_copy_kernel_to_user(&mm->zeta,
                                  &g_loop.system.startup.proc_table
                                       .slots[dst_proc & (MUZIX_PROC_TABLE_MAX - 1)]
                                       .map,
                                  dst_vir, (const uint8_t *)kernel_buf, len);
    if (rc == 0) {
        g_to_user.ok++;
    }
    return rc;
}

/* The fork stack copy's other two halves - muzix_mm_copy_map_to_kernel() and
 * muzix_mm_copy_kernel_to_map() - are the tree's own and reach these two
 * directly, with no message hop, so the chunked stack copy below is real code
 * running against the real primitive. */

/* The memory manager's own restatements.  Excluding mm/mm_service.c from the
 * closure leaves these undefined and none of them is on a path this test cares
 * about; the two that are - the page allocator and its inverse - forward to
 * platform/zeta-v2/kernel_bank_model.c's, which is in the pool and is the real
 * allocator.  A stub that handed back a made-up page number would let the fork
 * case pass without a page ever existing. */
void muzix_mm_service_init(muzix_mm_service_t *mm,
                           struct muzix_system_task_t *system_task,
                           uint8_t source)
{
    if (!mm) {
        return;
    }
    memset(mm, 0, sizeof(*mm));
    mm->system_task = system_task;
    mm->source = source;
}

void muzix_mm_memory_init(muzix_mm_service_t *mm, const process_map_t *kernel_map)
{
    if (!mm || !kernel_map) {
        return;
    }
    mm->kernel_map = *kernel_map;
    zeta_init(&mm->zeta);
    zeta_memory_init(kernel_map);
}

int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page)
{
    (void)mm;
    return zeta_memory_alloc_page(page);
}

void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page)
{
    (void)mm;
    zeta_memory_release_page(page);
}

/* Not reached by anything below; each refuses rather than pretending, so a
 * future case that needs one reports "not covered here" instead of "worked". */
int muzix_mm_load_kernel_map(muzix_mm_service_t *mm, const process_map_t *map)
{
    (void)mm; (void)map;
    return -1;
}

int muzix_mm_cross_bank_call(muzix_mm_service_t *mm, const process_map_t *map,
                             int (*func)(void *), void *arg)
{
    (void)mm; (void)map; (void)func; (void)arg;
    return -1;
}

int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm, uint8_t page,
                                 uint16_t offset, void *dst, uint16_t len)
{
    (void)mm; (void)page; (void)offset; (void)dst; (void)len;
    return -1;
}

int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm, uint8_t page,
                                     uint16_t offset, void *dst, uint16_t len)
{
    (void)mm; (void)page; (void)offset; (void)dst; (void)len;
    return -1;
}

/* ------------------------------------------------- the filesystem seam ---- */

int muzix_fs_service_open_path_flags(muzix_fs_service_t *fs, const char *path,
                                     uint16_t flags, uint16_t uid, uint16_t gid)
{
    (void)fs;
    seen_path("open", path, 0);
    g_seen.flags = flags;
    g_seen.uid = uid;
    g_seen.gid = gid;
    return g_seen_open_fd;
}

int muzix_fs_service_creat_path(muzix_fs_service_t *fs, const char *path,
                                uint16_t mode, uint16_t umask)
{
    (void)fs;
    seen_path("creat", path, 0);
    g_seen.mode = mode;
    g_seen.umask = umask;
    return 0;
}

int muzix_fs_service_mknod_path(muzix_fs_service_t *fs, const char *path,
                                uint16_t mode, uint16_t device, uint16_t euid,
                                uint16_t umask)
{
    (void)fs;
    seen_path("mknod", path, 0);
    g_seen.mode = mode;
    g_seen.device = device;
    g_seen.uid = euid;
    g_seen.umask = umask;
    return 0;
}

int muzix_fs_service_mkdir_path(muzix_fs_service_t *fs, const char *path,
                                uint16_t mode, uint16_t umask, uint16_t *inode)
{
    (void)fs;
    seen_path("mkdir", path, 0);
    g_seen.mode = mode;
    g_seen.umask = umask;
    if (inode) {
        *inode = 0x0111u;      /* a fixed inode, so the value is checkable */
    }
    return 0;
}

int muzix_fs_service_rmdir_path(muzix_fs_service_t *fs, const char *path)
{
    (void)fs;
    seen_path("rmdir", path, 0);
    return 0;
}

int muzix_fs_service_chdir(muzix_fs_service_t *fs, const char *path)
{
    (void)fs;
    seen_path("chdir", path, 0);
    return 0;
}

int muzix_fs_service_unlink_path(muzix_fs_service_t *fs, const char *path)
{
    (void)fs;
    seen_path("unlink", path, 0);
    return 0;
}

int muzix_fs_service_rename_path(muzix_fs_service_t *fs, const char *old_path,
                                 const char *new_path)
{
    (void)fs;
    seen_path("rename", old_path, new_path);
    return 0;
}

int muzix_fs_service_link_path(muzix_fs_service_t *fs, const char *old_path,
                               const char *new_path)
{
    (void)fs;
    seen_path("link", old_path, new_path);
    return 0;
}

int muzix_fs_service_chmod_path(muzix_fs_service_t *fs, const char *path,
                                uint16_t mode, uint16_t uid)
{
    (void)fs;
    seen_path("chmod", path, 0);
    g_seen.mode = mode;
    g_seen.uid = uid;
    return 0;
}

int muzix_fs_service_access_path(muzix_fs_service_t *fs, const char *path,
                                 uint16_t requested, uint16_t uid, uint16_t gid)
{
    (void)fs;
    seen_path("access", path, 0);
    g_seen.mode = requested;
    g_seen.uid = uid;
    g_seen.gid = gid;
    return 0;
}

int muzix_fs_service_getcwd(muzix_fs_service_t *fs, char *buffer, size_t size)
{
    (void)fs;
    seen_enter("getcwd");
    if (!buffer || size == 0) {
        return -1;
    }
    strncpy(buffer, g_seen_getcwd, size - 1);
    buffer[size - 1] = 0;
    return 0;
}

int muzix_fs_service_close(muzix_fs_service_t *fs, int handle)
{
    (void)fs;
    seen_enter("close");
    g_seen.fd = handle;
    return g_seen_close_rc;
}

int muzix_fs_service_dup(muzix_fs_service_t *fs, int handle)
{
    (void)fs;
    seen_enter("dup");
    g_seen.fd = handle;
    return handle + 10;
}

int muzix_fs_service_pipe(muzix_fs_service_t *fs, int fds[2])
{
    (void)fs;
    seen_enter("pipe");
    if (!fds) {
        return -1;
    }
    fds[0] = 5;
    fds[1] = 6;
    return 0;
}

int muzix_fs_service_seek(muzix_fs_service_t *fs, int handle, uint32_t position)
{
    (void)fs;
    seen_enter("seek");
    g_seen.fd = handle;
    g_seen.position = position;
    return g_seen_seek_rc;
}

int muzix_fs_service_sync(muzix_fs_service_t *fs)
{
    (void)fs;
    seen_enter("sync");
    return 0;
}

int muzix_fs_service_ioctl(muzix_fs_service_t *fs, int handle, uint16_t request,
                           void *argument)
{
    (void)fs;
    (void)argument;
    seen_enter("ioctl");
    g_seen.fd = handle;
    g_seen.flags = request;
    return 0;
}

/* read() hands back a counting pattern and write() keeps everything it was
 * given.  A run of consecutive values is what proves the handler placed each
 * chunk at the right offset: a handler that re-read from arg2 for every chunk,
 * or dropped one, cannot produce a run. */
int muzix_fs_service_read(muzix_fs_service_t *fs, int handle, uint8_t *buffer,
                          size_t length, size_t *result)
{
    size_t i;
    size_t n;

    (void)fs;
    seen_enter("read");
    g_seen.fd = handle;
    if (g_seen.calls - 1 < SEEN_MAX_CHUNKS) {
        g_seen.chunk_len[g_seen.calls - 1] = (uint16_t)length;
    }
    if (g_seen_read_rc || !buffer || !result) {
        if (result) {
            *result = 0;
        }
        return -1;
    }
    /* A file that runs out says so with a zero result, which is what stops the
     * handler's loop without being mistaken for an error. */
    n = (length > g_seen_read_left) ? g_seen_read_left : length;
    for (i = 0; i < n; i++) {
        buffer[i] = (uint8_t)(g_seen_read_base + g_seen_read_pos + i);
    }
    g_seen_read_pos += n;
    g_seen_read_left -= n;
    g_seen.sink_len += n;
    *result = n;
    return 0;
}

int muzix_fs_service_write(muzix_fs_service_t *fs, int handle,
                           const uint8_t *buffer, size_t length, size_t *result)
{
    (void)fs;
    seen_enter("write");
    g_seen.fd = handle;
    if (g_seen.calls - 1 < SEEN_MAX_CHUNKS) {
        g_seen.chunk_len[g_seen.calls - 1] = (uint16_t)length;
    }
    if (!buffer || !result) {
        return -1;
    }
    if (g_seen.sink_len + length <= sizeof(g_seen.sink)) {
        memcpy(&g_seen.sink[g_seen.sink_len], buffer, length);
    }
    g_seen.sink_len += length;
    *result = length;
    return 0;
}

int muzix_fs_service_stat_path(muzix_fs_service_t *fs, const char *path,
                               muzix_fs_inode_t *stat)
{
    (void)fs;
    seen_path("stat", path, 0);
    if (stat) {
        memset(stat, 0xA7, sizeof(*stat));
    }
    return 0;
}

int muzix_fs_service_fstat(muzix_fs_service_t *fs, int handle,
                           muzix_fs_inode_t *stat)
{
    (void)fs;
    seen_enter("fstat");
    g_seen.fd = handle;
    if (stat) {
        memset(stat, 0x5C, sizeof(*stat));
    }
    return 0;
}

/* 0 = an entry was filled in, 1 = end of directory, -1 = a failure.  The three
 * are distinguishable only in the return value, which is why the handler's
 * test on it is worth a case of its own: it used to be `drc <= 0`, which took
 * the success path out and left `ls` printing the directory it had read. */
int muzix_fs_service_readdir(muzix_fs_service_t *fs, int handle, uint16_t index,
                             muzix_fs_dirent_t *entry)
{
    (void)fs;
    seen_enter("readdir");
    g_seen.fd = handle;
    g_seen.index = index;
    if (entry && g_seen_readdir_rc == 0) {
        entry->inode = (uint16_t)g_seen_readdir_inode;
        strncpy(entry->name, "hw-notes", sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = 0;
    }
    return g_seen_readdir_rc;
}

/* Five more, for the same reason: excluding fs/fs_service.c leaves them
 * undefined and nothing below reaches them.  They exist for the closure, not
 * for any assertion here. */
void muzix_fs_service_init(muzix_fs_service_t *fs, muzix_system_task_t *task,
                           uint8_t source)
{
    if (!fs) {
        return;
    }
    memset(fs, 0, sizeof(*fs));
    fs->system_task = task;
    fs->source = source;
}

void muzix_fs_service_attach_volume(muzix_fs_service_t *fs, muzix_fs_volume_t *volume)
{
    if (fs) {
        fs->volume = volume;
    }
}

void muzix_fs_service_attach_tty(muzix_fs_service_t *fs, muzix_tty_device_t *tty)
{
    if (fs) {
        fs->tty = tty;
    }
}

int muzix_fs_service_open_path(muzix_fs_service_t *fs, const char *path)
{
    (void)fs;
    seen_path("open_path", path, 0);
    return g_seen_open_fd;
}

int muzix_fs_service_open_root(muzix_fs_service_t *fs, const char *name)
{
    (void)fs;
    seen_path("open_root", name, 0);
    return g_seen_open_fd;
}

int muzix_fs_service_open_tty(muzix_fs_service_t *fs)
{
    (void)fs;
    seen_enter("open_tty");
    return 0;
}

/* ------------------------------------------------------------ the fixture */

static void chip_write(uint8_t port, uint8_t value)
{
    muzix_host_ds1302_write(port, value);
}

static uint8_t chip_read(uint8_t port)
{
    return muzix_host_ds1302_read(port);
}

static void setup(void)
{
    int i;

    muzix_host_user_space_clear();
    muzix_host_port_attach(chip_write, chip_read);
    muzix_host_ds1302_reset();
    seen_reset();
    xfer_reset(&g_to_user);
    xfer_reset(&g_from_user);

    /* The memory manager for real, including its page allocator: the fork case
     * needs a page and a fake that always refused would silently take the
     * "child shares the parent's stack" branch instead - the very thing the
     * chunking fix exists to stop. */
    muzix_mm_service_init(&g_mm, &g_task, 0);
    muzix_mm_memory_init(&g_mm, &g_kmap);

    g_loader_calls = 0;
    strcpy(g_seen_getcwd, "/");
    g_seen_close_rc = 0;
    g_seen_seek_rc = 0;
    g_seen_read_rc = 0;
    g_seen_readdir_rc = 0;
    g_seen_read_base = 0x40u;
    g_seen_read_left = (size_t)-1;
    g_seen_read_pos = 0;
    muzix_boot_rtc_state = MUZIX_BOOT_RTC_UNTRIED;
    muzix_fork_stack_len = 0;

    for (i = 0; i < 2; i++) {
        g_maps[i] = g_kmap;
        g_maps[i].pages[1] = (uint8_t)(0x30 + i);
        g_maps[i].pages[2] = (uint8_t)(0x32 + i);
        muzix_mproc_init(&g_mp[i], 0x0000u, 0x1000u, 0x2000u, 0x4000u, 0x4000u,
                         0x4000u, (uint8_t)(i + 1));
        /* SYS_COPY resolves (segment, virtual address) through the mproc, so
         * each segment has to describe the window it stands for before any
         * address in it is in range.  The arrangement is exec_loader's: window
         * 1 holds the stack segment and window 2 the text and data segments,
         * which is also what muzix_mproc_to_zeta_map() derives. */
        muzix_mproc_set_seg(&g_mp[i], MUZIX_SEG_STACK, 0x4000u, 0x30u, 0x4000u);
        muzix_mproc_set_seg(&g_mp[i], MUZIX_SEG_TEXT, 0x8000u, 0x32u, 0x4000u);
        muzix_mproc_set_seg(&g_mp[i], MUZIX_SEG_DATA, 0x8000u, 0x32u, 0x4000u);
    }

    muzix_kernel_loop_init(&g_loop, &g_kmap, 1, &g_maps[0]);
    g_loop.mm = &g_mm;
    muzix_kernel_loop_register(&g_loop, 0, 1, &g_mp[0], &g_maps[0],
                               MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    muzix_kernel_loop_register(&g_loop, 1, 2, &g_mp[1], &g_maps[1],
                               MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    g_loop.system.startup.proc_table.current = 0;
    g_loop.system.startup.proc_table.slots[0].p_flags = 0;
    /* Distinctive, so that a value reaching the caller can be traced to this
     * row of the table rather than to a memset. */
    g_loop.system.startup.proc_table.slots[0].parent_pid = 7;
    g_loop.system.startup.proc_table.slots[0].user_time = 11u;
    g_loop.system.startup.proc_table.slots[0].uid = 100;
    g_loop.system.startup.proc_table.slots[0].gid = 200;
    g_loop.system.startup.proc_table.slots[0].euid = 100;
    g_loop.system.startup.proc_table.slots[0].egid = 200;
    g_loop.system.startup.proc_table.slots[0].umask = 022;
    g_loop.current_pid = 1;
    muzix_switch_pending = 0;

    memset(&g_kctx, 0, sizeof(g_kctx));
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_kctx.loop = &g_loop;
    g_kctx.mm = &g_mm;
    g_kctx.process_table = &g_loop.system.startup.proc_table;
    g_ctx.kernel = &g_kctx;
    g_ctx.fs = &g_fs;
    g_ctx.pid = 1;
    g_ctx.exec_loader = test_exec_loader;
}

/* ============================== case 1: the calling convention ============ */

/*
 * Asserted directly, because it cannot be asserted indirectly.
 *
 * Everything above depends on a mapping that is not written down anywhere the
 * compiler reads.  These modules are compiled with plain `-mz80` and no
 * --sdcccall, so SDCC passes the first parameter in HL, the second in DE and
 * the third on the stack; the SDCC library linked alongside them is sdcccall(1)
 * and does not share that mapping.  Both conventions therefore exist in one
 * image, and the name of either is no guide to which is in force.  A wrong
 * guess is invisible whenever the values happen to agree - which is why a stub
 * that pushed its arguments left argv holding a stale register and `ls` and
 * `cat` worked or failed depending on what the previous function had left in
 * DE.
 *
 * Two halves, and both are checks rather than comments:
 *
 *   1. The convention itself, measured.  A three-parameter probe is compiled
 *      with the same compiler and the same -mz80 the tree uses, and the
 *      generated code is read for the register each literal lands in.  This
 *      fails if the compiler's mapping changes, and it is the only assertion
 *      in the tree that would notice.
 *   2. The one call site that depends on it.  lib/crt0.s is where `main` is
 *      called from assembly, so it is where the mapping is consumed.  The
 *      check requires argv to be loaded into DE and argc into HL before
 *      `call _main`, and no push of either between them - the exact shape of
 *      the defect, which passed argc by accident while DE kept
 *      s__INITIALIZED.
 */

/* Collapses runs of whitespace and tabs to single spaces, so the checks below
 * do not depend on how SDCC happened to lay the listing out. */
static void squeeze(char *s)
{
    char *w = s;

    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            if (w != s && *(w - 1) != ' ') {
                *w++ = ' ';
            }
            continue;
        }
        *w++ = *s;
    }
    *w = 0;
}

/* Offset just past "ld <reg>, <prefix><lit>" at or after `from`, or -1.  The
 * prefix is "#" for an immediate and "(" for a memory reference; the literal is
 * matched with or without a 0x in front of it, because that is the one part of
 * a generated listing that has differed between SDCC builds. */
static long find_load(const char *text, long from, const char *reg,
                      const char *prefix, const char *lit)
{
    const char *p = text + from;

    while (p && *p) {
        const char *hit = strstr(p, "ld ");
        const char *q;
        size_t n = strlen(reg);
        size_t pre = strlen(prefix);

        if (!hit) {
            return -1;
        }
        if (strncmp(hit + 3, reg, n) == 0 && hit[3 + n] == ',') {
            q = hit + 3 + n + 1;
            while (*q == ' ') {
                q++;
            }
            if (strncmp(q, prefix, pre) == 0) {
                q += pre;
                if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
                    q += 2;
                }
                if (strncmp(q, lit, strlen(lit)) == 0) {
                    return (long)(q - text) + (long)strlen(lit);
                }
            }
        }
        p = hit + 3;
    }
    return -1;
}

static long find_ld(const char *text, long from, const char *reg, const char *lit)
{
    return find_load(text, from, reg, "#", lit);
}

static long find_after(const char *text, const char *needle)
{
    const char *hit = strstr(text, needle);

    return hit ? (long)(hit - text) + (long)strlen(needle) : -1;
}

static char *slurp(const char *path)
{
    static char buf[262144];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

/* The project root, found by walking up from the directory this file was
 * compiled from until there is a Makefile that builds the kernel. */
static const char *repo_root(void)
{
    static char dir[512];
    char probe[600];
    char *slash;
    int depth;

    strncpy(dir, __FILE__, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    slash = strrchr(dir, '/');
    if (!slash) {
        return 0;
    }
    *slash = 0;

    for (depth = 0; depth < 8; depth++) {
        FILE *f;

        snprintf(probe, sizeof(probe), "%s/Makefile", dir);
        f = fopen(probe, "rb");
        if (f) {
            char head[4096];
            size_t n = fread(head, 1, sizeof(head) - 1, f);

            fclose(f);
            head[n] = 0;
            if (strstr(head, "KERNEL_C_MODULES")) {
                return dir;
            }
        }
        slash = strrchr(dir, '/');
        if (slash) {
            if (slash == dir) {
                dir[1] = 0;          /* the filesystem root */
            } else {
                *slash = 0;
            }
        } else if (strcmp(dir, ".") == 0) {
            break;
        } else {
            /* One component left: this is it, and there is no parent to try. */
            strcpy(dir, ".");
        }
    }
    return 0;
}

/* Half 1: compile a probe with the tree's compiler and read the registers. */
static int measure_the_convention(char *report, size_t report_len)
{
    static const char probe[] =
        "unsigned int muzix_cc_probe(unsigned int p1, unsigned int p2,\n"
        "                           unsigned int p3)\n"
        "{ return p1 + p2 + p3; }\n"
        "void muzix_cc_probe_caller(void)\n"
        "{ (void)muzix_cc_probe(0x1111u, 0x2222u, 0x3333u); }\n";
    const char *root = repo_root();
    const char *compiler = getenv("SDCC");
    char c_path[600];
    char r_path[600];
    char a_path[600];
    char *asm_text;
    long at, p3, p2, p1, call;
    const char *body;

    if (!root) {
        snprintf(report, report_len,
                 "cannot locate the project root from __FILE__=%s", __FILE__);
        return 1;
    }
    /* build/hosttests is where the runner put this binary's objects and it
     * exists for as long as the binary does. */
    snprintf(c_path, sizeof(c_path), "%s/build/hosttests/muzix_cc_probe.c", root);
    snprintf(r_path, sizeof(r_path), "%s/build/hosttests/muzix_cc_probe.rel", root);
    snprintf(a_path, sizeof(a_path), "%s/build/hosttests/muzix_cc_probe.asm", root);

    {
        FILE *f = fopen(c_path, "wb");

        if (!f) {
            snprintf(report, report_len, "cannot write %s", c_path);
            return 1;
        }
        fwrite(probe, 1, sizeof(probe) - 1, f);
        fclose(f);
    }
    char cmd[512];

    if (!compiler || !*compiler) {
        compiler = "sdcc";
    }
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -mz80 -c \"%s\" -o \"%s\" >/dev/null 2>&1",
             compiler, c_path, r_path);
    if (system(cmd) != 0) {
        /* The command goes in via `cmd`, not `report`: formatting into `report`
         * while reading it through the %s is undefined behaviour, and it is
         * read after system() has already run, so the command is still live in
         * the local and there is no reason to alias the two.  cppcheck called
         * this one: "Variable 'report' is used as parameter and destination in
         * snprintf()". */
        snprintf(report, report_len,
                 "sdcc could not compile the calling-convention probe (%s). "
                 "Without it this test cannot measure the convention, and a "
                 "gate that cannot measure must not report a pass.",
                 cmd);
        return 1;
    }
    asm_text = slurp(a_path);
    if (!asm_text) {
        snprintf(report, report_len, "sdcc produced no listing at %s", a_path);
        return 1;
    }
    squeeze(asm_text);

    body = strstr(asm_text, "_muzix_cc_probe_caller::");
    if (!body) {
        snprintf(report, report_len, "%s has no caller for the probe", a_path);
        return 1;
    }

    at = find_ld(body, 0, "hl", "3333");
    if (at >= 0) {
        long pushed = find_after(body + at, "push hl");

        p3 = pushed >= 0 ? at + pushed : -1;
    } else {
        p3 = -1;
    }
    p2 = find_ld(body, 0, "de", "2222");
    p1 = find_ld(body, 0, "hl", "1111");
    call = find_after(body, "call _muzix_cc_probe");

    if (at < 0 || p3 < 0 || p2 < 0 || p1 < 0 || call < 0) {
        snprintf(report, report_len,
                 "%s does not contain the probe's parameter loads at all", a_path);
        return 1;
    }
    if (!(at < p3 && p3 < p2 && p2 < p1 && p1 < call)) {
        snprintf(report, report_len,
                 "the probe's parameters are not laid out as HL/DE/stack: "
                 "load 0x3333 at %ld, push at %ld, load 0x2222 at %ld, "
                 "load 0x1111 at %ld, call at %ld",
                 at, p3, p2, p1, call);
        return 1;
    }
    /* HL is loaded twice; the last load before the call is the one the callee
     * reads, so it has to be the first parameter. */
    if (find_ld(body, p1, "hl", "3333") >= 0 &&
        find_ld(body, p1, "hl", "3333") < call) {
        snprintf(report, report_len,
                 "the third parameter is loaded into HL after the first, so HL "
                 "is not param1 at the call");
        return 1;
    }
    return 0;
}

/* Half 2: the assembly that consumes it. */
static int check_crt0_hands_over_registers(char *report, size_t report_len)
{
    const char *root = repo_root();
    char path[600];
    char *text;
    long argv_at;
    long argc_at;
    long call;
    const char *push;

    if (!root) {
        snprintf(report, report_len, "cannot locate the project root");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/lib/crt0.s", root);
    text = slurp(path);
    if (!text) {
        snprintf(report, report_len, "cannot read %s", path);
        return 1;
    }
    squeeze(text);

    call = find_after(text, "call _main");
    argv_at = find_load(text, 0, "de", "(", "MUZIX_EXEC_ARGV_ADDR)");
    argc_at = find_load(text, 0, "hl", "(", "MUZIX_EXEC_ARGC_ADDR)");
    if (call < 0 || argv_at < 0 || argc_at < 0) {
        snprintf(report, report_len,
                 "%s does not load argv into DE and argc into HL before "
                 "`call _main` (argv at %ld, argc at %ld, call at %ld)",
                 path, argv_at, argc_at, call);
        return 1;
    }
    if (!(argv_at < argc_at && argc_at < call)) {
        snprintf(report, report_len,
                 "%s loads them in the order argv@%ld argc@%ld call@%ld; DE is "
                 "the second parameter, so it must be loaded first",
                 path, argv_at, argc_at, call);
        return 1;
    }
    /* The pushed-arguments form.  It left argc in HL by accident - the last
     * `ld hl,(...)` happened to be the argc load - while DE kept whatever the
     * preceding copy loop had put there. */
    push = strstr(text + argc_at, "push ");
    if (push && (long)(push - text) < call) {
        snprintf(report, report_len,
                 "%s pushes an argument after loading them: `%.*s`",
                 path, (int)(call - (push - text)), push);
        return 1;
    }
    return 0;
}

static int test_calling_convention(void)
{
    char report[1024];

    if (measure_the_convention(report, sizeof(report)) != 0) {
        printf("cc probe: %s\n", report);
        return 1;
    }
    if (check_crt0_hands_over_registers(report, sizeof(report)) != 0) {
        printf("crt0: %s\n", report);
        return 2;
    }
    return 0;
}

/* ============================== case 2: dispatch and identity ============== */

static int test_dispatch_and_identity(void)
{
    muzix_syscall_t call;

    CHECK(call_user(MUZIX_USER_SYS_GETPID, 0, 0, 0) == 1, 1);
    CHECK(call_user(MUZIX_USER_SYS_GETPPID, 0, 0, 0) == 7, 2);

    /* The identity comes from the process table on every call, not from a
     * cache.  Changing the slot has to change the answer, and the slot has to
     * be the one the table says is current. */
    CHECK(call_user(MUZIX_USER_SYS_GETUID, 0, 0, 0) == 100, 3);
    g_loop.system.startup.proc_table.slots[0].uid = 101;
    CHECK(call_user(MUZIX_USER_SYS_GETUID, 0, 0, 0) == 101, 4);
    g_loop.system.startup.proc_table.current = 1;
    CHECK(call_user(MUZIX_USER_SYS_GETPID, 0, 0, 0) == 2, 5);
    CHECK(call_user(MUZIX_USER_SYS_GETPPID, 0, 0, 0) == 0, 6);
    CHECK(call_user(MUZIX_USER_SYS_GETUID, 0, 0, 0) == 0, 7);
    /* Left as it is found for the rest of the file; nothing below depends on
     * the table pointing at a particular slot, and everything above does. */

    memset(&call, 0, sizeof(call));
    call.call = MUZIX_SYS_YIELD;
    call.pid = 1;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_OK, 6);
    call.call = MUZIX_SYS_EXIT;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_OK, 7);

    /* The refusals.  A NULL context and a NULL call are both -1, not a
     * dispatch into whatever the registers happened to hold. */
    CHECK(muzix_handle_syscall(0, &call) == MUZIX_SYSCALL_FAIL, 8);
    CHECK(muzix_handle_syscall(&g_kctx, 0) == MUZIX_SYSCALL_FAIL, 9);
    {
        muzix_syscall_context_t detached = g_kctx;

        detached.loop = 0;
        call.call = MUZIX_SYS_YIELD;
        CHECK(muzix_handle_syscall(&detached, &call) == MUZIX_SYSCALL_FAIL, 1);
        call.call = MUZIX_SYS_EXIT;
        CHECK(muzix_handle_syscall(&detached, &call) == MUZIX_SYSCALL_FAIL, 2);
    }
    call.call = 0xEE;                     /* not a call this kernel has */
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 3);

    /* SYS_COPY resolves both pids through the table and hands the map-to-map
     * copy the segment numbers and the virtual addresses it was given. */
    memset(&call, 0, sizeof(call));
    call.call = MUZIX_SYS_COPY;
    call.copy_request = 0;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 4);
    return 0;
}

static int test_sys_copy_arguments_reach_the_memory_manager(void)
{
    static muzix_copy_request_t req;
    static uint8_t payload[64];
    muzix_syscall_t call;
    size_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xC0u + i);
    }

    memset(&call, 0, sizeof(call));
    call.call = MUZIX_SYS_COPY;
    call.copy_request = &req;

    /* Nothing to copy through. */
    call.copy_request = 0;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 1);
    call.copy_request = &req;

    req.src_proc = 1;
    req.dst_proc = 2;
    req.src_space = MUZIX_SEG_STACK;     /* window 1 */
    req.dst_space = MUZIX_SEG_TEXT;      /* window 2 */
    req.src_vir = U_W1;
    req.dst_vir = U_BUF;
    req.bytes = sizeof(payload);

    /* The whole map-to-map path: the pids are resolved through the table, the
     * segment and virtual address through muzix_mproc_umap(), and the bytes
     * land in the destination's window.  None of it is stubbed, so this is the
     * case that would catch a segment argument being validated and then
     * discarded - which is what SYS_COPY used to do, having silently copied
     * nothing while reporting success. */
    put_user(U_W1, payload, sizeof(payload));
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(payload));
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_OK, 2);
    CHECK(user_eq(U_BUF, payload, sizeof(payload)), 3);

    /* An unknown pid on either side is refused before any copy is attempted. */
    req.dst_proc = 99;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 4);
    req.dst_proc = 2;
    req.src_proc = 99;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 5);
    req.src_proc = 1;

    /* An address outside the segment that was named, a zero length, and a
     * length the segment cannot cover: all EFAULT. */
    req.src_space = MUZIX_SEG_STACK;
    req.src_vir = U_BUF;                  /* in window 2, asked for as stack */
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 6);
    req.src_vir = U_W1;
    req.bytes = 0;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 7);
    req.bytes = sizeof(payload);

    /* The window discipline again, on the internal path: a source or a
     * destination in one of the kernel's two windows is refused here, before
     * the primitive is reached, rather than as a silent zero-byte transfer. */
    req.src_vir = U_W0;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 8);
    req.src_vir = U_W3;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 9);
    req.src_vir = U_W1;
    req.dst_vir = U_W0;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 1);
    req.dst_vir = U_W3;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 2);
    req.dst_vir = U_BUF;

    /* And more than the staging buffer is a refusal, not a split: this entry
     * point has always been all-or-nothing, which is why the read and write
     * handlers above have to split it themselves. */
    req.bytes = MUZIX_ZETA_COPY_STAGING + 1u;
    CHECK(muzix_handle_syscall(&g_kctx, &call) == MUZIX_SYSCALL_FAIL, 3);
    req.bytes = sizeof(payload);
    return 0;
}

/* ============================== case 3: the path arguments ================= */

/*
 * The shape of every `ls` and `cat` failure this week: the caller's path was
 * read from an address the kernel could not copy out of, and the refusal was
 * reported as a memory map rather than as the argument that caused it.
 */
static int test_paths_are_copied_from_the_caller(void)
{
    int32_t rc;

    seen_reset();
    put_str(U_PATH, "/bin/ls");
    rc = call_user(MUZIX_USER_SYS_OPEN, (int32_t)U_PATH,
                   MUZIX_FS_OPEN_RDONLY, 0);
    CHECK(rc == 3, 1);
    CHECK(seen_reached("open"), 2);
    CHECK(strcmp(g_seen.path, "/bin/ls") == 0, 3);
    CHECK(g_seen.flags == MUZIX_FS_OPEN_RDONLY, 4);
    CHECK(g_seen.uid == 100 && g_seen.gid == 200, 5);

    /* Every other path-taking call, and both arguments of the two that take
     * two.  A rename with its arguments the wrong way round is a silent
     * rename in the wrong direction, which no return value would report. */
    seen_reset();
    put_str(U_PATH, "/tmp");
    put_str(U_ARG1, "/tmp");
    put_str(U_ARG2, "/tmp/old");
    CHECK(call_user(MUZIX_USER_SYS_RENAME, (int32_t)U_ARG1,
                    (int32_t)U_ARG2, 0) == 0, 6);
    CHECK(strcmp(g_seen.path, "/tmp") == 0 && strcmp(g_seen.path2, "/tmp/old") == 0,
          7);

    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_LINK, (int32_t)U_ARG1,
                    (int32_t)U_ARG2, 0) == 0, 8);
    CHECK(strcmp(g_seen.path, "/tmp") == 0 && strcmp(g_seen.path2, "/tmp/old") == 0,
          9);

    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_MKDIR, (int32_t)U_PATH, 0755, 0) == 0, 1);
    CHECK(seen_reached("mkdir") && g_seen.mode == 0755 &&
          g_seen.umask == 022, 2);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_CREAT, (int32_t)U_PATH, 0644, 0) == 0, 3);
    CHECK(seen_reached("creat") && g_seen.mode == 0644, 4);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_CHMOD, (int32_t)U_PATH, 0600, 0) == 0, 5);
    CHECK(seen_reached("chmod") && g_seen.mode == 0600 && g_seen.uid == 100, 6);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_ACCESS, (int32_t)U_PATH,
                    MUZIX_FS_ACCESS_READ, 0) == 0, 7);
    CHECK(seen_reached("access") && g_seen.mode == MUZIX_FS_ACCESS_READ &&
          g_seen.uid == 100 && g_seen.gid == 200, 8);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_MKNOD, (int32_t)U_PATH, 0644, 9) == 0, 9);
    CHECK(seen_reached("mknod") && g_seen.device == 9, 1);

    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_UNLINK, (int32_t)U_PATH, 0, 0) == 0, 2);
    CHECK(seen_reached("unlink"), 3);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_RMDIR, (int32_t)U_PATH, 0, 0) == 0, 4);
    CHECK(seen_reached("rmdir"), 5);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_CHDIR, (int32_t)U_PATH, 0, 0) == 0, 6);
    CHECK(seen_reached("chdir"), 7);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_STAT, (int32_t)U_PATH,
                    (int32_t)U_BUF, 0) == 0, 8);
    CHECK(seen_reached("stat"), 9);

    /* Descriptor and flag arguments, straight through. */
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_CLOSE, 4, 0, 0) == 0, 1);
    CHECK(seen_reached("close") && g_seen.fd == 4, 2);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_DUP, 4, 0, 0) == 14, 3);
    CHECK(seen_reached("dup") && g_seen.fd == 4, 4);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_SEEK, 4, 1024, 0) == 0, 5);
    CHECK(seen_reached("seek") && g_seen.fd == 4 && g_seen.position == 1024u, 6);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_SYNC, 0, 0, 0) == 0, 7);
    CHECK(seen_reached("sync"), 8);

    {
        int fds[2];

        user_get(U_BUF, fds, sizeof(fds));
        seen_reset();
        CHECK(call_user(MUZIX_USER_SYS_PIPE, (int32_t)U_BUF, 0, 0) == 0, 9);
        user_get(U_BUF, fds, sizeof(fds));
        CHECK(seen_reached("pipe") && fds[0] == 5 && fds[1] == 6, 1);
    }
    {
        uint8_t params[8];

        memset(params, 0x5A, sizeof(params));
        put_user(U_BUF, params, sizeof(params));
        seen_reset();
        CHECK(call_user(MUZIX_USER_SYS_IOCTL, 0, MUZIX_TTY_IOCTL_GETP,
                        (int32_t)U_BUF) == 0, 2);
        CHECK(seen_reached("ioctl") && g_seen.fd == 0 &&
              g_seen.flags == MUZIX_TTY_IOCTL_GETP, 3);
        /* An unknown request is refused before anything is copied. */
        seen_reset();
        CHECK(call_user(MUZIX_USER_SYS_IOCTL, 0, 0xBEEF, (int32_t)U_BUF) == -1, 4);
        CHECK(!seen_reached("ioctl"), 5);
    }
    return 0;
}

/*
 * The refusal.  A path the kernel will not copy in must be refused *before* the
 * filesystem is asked for anything, or the filesystem is left holding a pointer
 * into the middle of the kernel's own text.
 */
static int test_a_path_that_will_not_copy_in_is_refused(void)
{
    uint8_t before[16];

    memcpy(before, &muzix_host_user_space()[U_BUF], sizeof(before));

    put_str(U_W0, "/bin/ls");
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_OPEN, (int32_t)U_W0, 0, 0) == -1, 1);
    CHECK(!seen_reached("open"), 2);
    CHECK(call_user(MUZIX_USER_SYS_STAT, (int32_t)U_W0, (int32_t)U_BUF, 0) == -1, 3);
    CHECK(!seen_reached("stat"), 4);

    put_str(U_W3, "/bin/ls");
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_OPEN, (int32_t)U_W3, 0, 0) == -1, 5);
    CHECK(!seen_reached("open"), 6);
    CHECK(call_user(MUZIX_USER_SYS_RENAME, (int32_t)U_W3, (int32_t)U_PATH,
                    0) == -1, 7);
    CHECK(!seen_reached("rename"), 8);

    /* The same refusal applies to the second path of a two-path call, so a
     * rename cannot half-succeed. */
    put_str(U_PATH, "/tmp/old");
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_RENAME, (int32_t)U_PATH, (int32_t)U_W0,
                    0) == -1, 9);
    CHECK(!seen_reached("rename"), 1);

    /* A path that is not NUL-terminated inside the kernel's buffer is a fault,
     * not an unbounded read, and it is refused. */
    {
        char long_path[64];
        size_t i;

        for (i = 0; i < sizeof(long_path) - 1; i++) {
            long_path[i] = 'a';
        }
        long_path[sizeof(long_path) - 1] = 0;
        put_user(U_PATH, long_path, sizeof(long_path));
        seen_reset();
        CHECK(call_user(MUZIX_USER_SYS_OPEN, (int32_t)U_PATH, 0, 0) == -1, 2);
        CHECK(!seen_reached("open"), 3);
    }

    /* A NULL path. */
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_OPEN, 0, 0, 0) == -1, 4);
    CHECK(!seen_reached("open"), 5);

    /* And nothing moved that was not asked to move. */
    CHECK(memcmp(before, &muzix_host_user_space()[U_BUF], sizeof(before)) == 0, 6);
    return 0;
}

/* ============================== case 4: buffers and counts ================= */

static int test_read_and_write_carry_the_bytes(void)
{
    static uint8_t payload[349];
    static uint8_t got[349];
    size_t i;
    int32_t rc;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0x80u + i);
    }

    /* WRITE: 349 bytes is more than the MM's staging buffer, so this is the
     * case that used to come back -1 and take `cat` with it.  It has to arrive
     * as more than one chunk, at consecutive addresses, with nothing dropped. */
    xfer_reset(&g_from_user);
    seen_reset();
    put_user(U_BUF, payload, sizeof(payload));
    rc = call_user(MUZIX_USER_SYS_WRITE, 3, (int32_t)U_BUF, (int32_t)sizeof(payload));
    CHECK(rc == (int32_t)sizeof(payload), 1);
    CHECK(seen_reached("write") && g_seen.fd == 3, 2);
    CHECK(g_seen.calls == 2, 3);                    /* chunked, not refused */
    CHECK(g_seen.chunk_len[0] == MUZIX_ZETA_COPY_STAGING &&
          g_seen.chunk_len[1] == sizeof(payload) - MUZIX_ZETA_COPY_STAGING, 4);
    CHECK(g_seen.sink_len == sizeof(payload) &&
          memcmp(g_seen.sink, payload, sizeof(payload)) == 0, 5);
    CHECK(g_from_user.max_len <= MUZIX_ZETA_COPY_STAGING, 6);

    /* READ: the same shape in the other direction, and the bytes have to land
     * at consecutive addresses in the caller's buffer. */
    xfer_reset(&g_to_user);
    seen_reset();
    g_seen_read_base = 0x40u;
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    rc = call_user(MUZIX_USER_SYS_READ, 3, (int32_t)U_BUF,
                   (int32_t)sizeof(payload));
    CHECK(rc == (int32_t)sizeof(payload), 7);
    CHECK(seen_reached("read") && g_seen.calls == 2, 8);
    user_get(U_BUF, got, sizeof(got));
    for (i = 0; i < sizeof(got); i++) {
        CHECK(got[i] == (uint8_t)(0x40u + i), 9);
    }
    CHECK(g_to_user.max_len <= MUZIX_ZETA_COPY_STAGING, 1);

    /* A file with 100 bytes left answers 100 for a request of 1000 and stops,
     * rather than pretending it reached the count it was asked for. */
    seen_reset();
    g_seen_read_left = 100;
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    CHECK(call_user(MUZIX_USER_SYS_READ, 3, (int32_t)U_BUF, 1000) == 100, 2);

    /* A zero-length read is a read of nothing, and it is not an error. */
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_READ, 3, (int32_t)U_BUF, 0) == 0, 3);

    /* The refusals. */
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_READ, 3, (int32_t)U_BUF, -1) == -1, 4);
    CHECK(!seen_reached("read"), 5);
    CHECK(call_user(MUZIX_USER_SYS_WRITE, 3, (int32_t)U_BUF, -1) == -1, 6);
    CHECK(!seen_reached("write"), 7);

    /* A buffer in a window the kernel owns, in either direction. */
    put_user(U_W0, payload, 16);
    seen_reset();
    CHECK(call_user(MUZIX_USER_SYS_WRITE, 3, (int32_t)U_W0, 16) == -1, 8);
    CHECK(!seen_reached("write"), 9);
    /* The two directions are not symmetric.  A read fills a kernel buffer
     * first and refuses the copy to the caller afterwards, so the filesystem
     * IS reached and the bytes are discarded; a write copies the caller's
     * buffer in first and so never reaches it.  Asserting that neither reached
     * the filesystem would have been asserting the wrong half of this. */
    seen_reset();
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_READ, 3, (int32_t)U_W3, 16) == -1, 1);
    CHECK(seen_reached("read"), 2);
    CHECK(g_to_user.calls == 1 && g_to_user.ok == 0, 3);

    /* And the filesystem's own failure reaches the caller as -1, with no
     * partial transfer left in the caller's buffer. */
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    seen_reset();
    g_seen_read_rc = 1;
    CHECK(call_user(MUZIX_USER_SYS_READ, 3, (int32_t)U_BUF, 64) == -1, 3);
    g_seen_read_rc = 0;
    CHECK(muzix_host_user_space()[U_BUF] == 0, 4);
    return 0;
}

/* ============================== case 5: the window discipline ============== */

static int test_windows_zero_and_three_are_the_kernels(void)
{
    struct proc_info info;

    /* SYS_TIMES reports the caller's own slot; SYS_PROCTAB reports a slot the
     * caller names.  Both hand the front of the slot straight to the copy, so
     * both are one window check away from publishing the process table into
     * the boot stub.  This is the syscall that took the region-0 write that
     * made a ring-3 call able to overwrite the syscall vector at 0x0030. */
    memset(&info, 0, sizeof(info));
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(info));
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_TIMES, (int32_t)U_BUF, 0, 0) == 0, 1);
    user_get(U_BUF, &info, sizeof(info));
    CHECK(info.pid == 1, 2);
    CHECK(info.ppid == 7, 3);
    CHECK(g_to_user.calls == 1 && g_to_user.addr[0] == U_BUF, 4);

    /* Window 1 is the process's, so it is accepted there too. */
    memset(&muzix_host_user_space()[U_W1], 0, sizeof(info));
    CHECK(call_user(MUZIX_USER_SYS_TIMES, (int32_t)U_W1, 0, 0) == 0, 5);
    user_get(U_W1, &info, sizeof(info));
    CHECK(info.pid == 1, 6);

    /* Window 0 and window 3 are the kernel's.  Refused, and - the part that
     * matters - refused *before* anything is transferred. */
    memset(&muzix_host_user_space()[U_BUF], 0xEE, sizeof(info));
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_TIMES, (int32_t)U_W0, 0, 0) == -1, 7);
    CHECK(g_to_user.calls == 1 && g_to_user.ok == 0, 8);
    CHECK(muzix_host_user_space()[U_BUF] == 0xEE, 9);
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_TIMES, (int32_t)U_W3, 0, 0) == -1, 1);
    CHECK(g_to_user.calls == 1 && g_to_user.ok == 0, 2);
    CHECK(muzix_host_user_space()[U_BUF] == 0xEE, 3);

    /* SYS_PROCTAB names the slot in arg2, and only arg2. */
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(info));
    CHECK(call_user(MUZIX_USER_SYS_PROCTAB, (int32_t)U_BUF, 1, 0) == 0, 4);
    user_get(U_BUF, &info, sizeof(info));
    CHECK(info.pid == 2, 5);
    CHECK(call_user(MUZIX_USER_SYS_PROCTAB, (int32_t)U_BUF,
                    MUZIX_PROC_TABLE_MAX, 0) == -1, 6);
    CHECK(call_user(MUZIX_USER_SYS_PROCTAB, (int32_t)U_BUF, -1, 0) == -1, 7);
    CHECK(call_user(MUZIX_USER_SYS_PROCTAB, 0, 0, 0) == -1, 8);
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_PROCTAB, (int32_t)U_W0, 0, 0) == -1, 9);
    CHECK(g_to_user.calls == 1 && g_to_user.ok == 0, 1);

    /* SYS_WAIT copies an exit status out to the caller.  The child's slot is
     * made a zombie the same way SYS_EXIT makes it, so this is the real
     * sequence and not a hand-built arrangement. */
    g_loop.system.startup.proc_table.slots[1].uid = 100;
    g_loop.system.startup.proc_table.slots[1].euid = 100;
    CHECK(muzix_proc_table_exit_status(&g_loop.system.startup.proc_table, 1, 1, 7)
          == 0, 2);
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(int32_t));
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_WAIT, (int32_t)U_BUF, 0, 0) == 2, 3);
    {
        int32_t status = 0;

        user_get(U_BUF, &status, sizeof(status));
        CHECK(status == 7, 4);
    }
    CHECK(g_to_user.calls == 1 && g_to_user.len[0] == sizeof(int32_t), 5);
    /* The zombie is gone, so a second wait finds nothing. */
    CHECK(call_user(MUZIX_USER_SYS_WAIT, (int32_t)U_BUF, 0, 0) == -1, 6);
    return 0;
}

/* ============================== case 6: the clock ========================= */

/*
 * SYS_GETRTC, SYS_SETRTC and SYS_TIME all copy a seven-byte struct between the
 * kernel and the caller, so they are the syscall path's shortest version of the
 * whole week's argument story: if the destination address is wrong the handler
 * reports "cannot read the clock" about a clock that answered.  The refusal
 * cases below are therefore the interesting ones, and the accepted cases exist
 * to show the copy is a copy and not a refusal.
 *
 * The write-back is kernel/test_syscall_setrtc.c's case and is not restated
 * here: that file drives the real driver over the real chip model and is the
 * one to read for it.  What is checked here is that the two agree - that the
 * write-back happens in this handler and not in the test.
 */
static int test_the_clock_arguments(void)
{
    muzix_rtc_time_t want;
    muzix_rtc_time_t got;

    memset(&want, 0, sizeof(want));

    /* Accepted in window 2. */
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(want));
    want.second = 11u;
    want.minute = 22u;
    want.hour   = 3u;
    want.day    = 4u;
    want.month  = 5u;
    want.year   = 26u;
    want.weekday = 0u;
    put_user(U_BUF, &want, sizeof(want));
    xfer_reset(&g_from_user);
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_SETRTC, (int32_t)U_BUF, 0, 0) == 0, 1);
    user_get(U_BUF, &got, sizeof(got));
    /* The chip stores Sunday for a request that did not name a weekday, and
     * the struct has to come back holding what the chip says. */
    CHECK(got.second == 11u && got.minute == 22u && got.hour == 3u &&
          got.day == 4u && got.month == 5u && got.year == 26u, 2);
    CHECK(got.weekday == MUZIX_DS1302_DOW_SUN, 3);
    CHECK(g_to_user.calls == 1 && g_to_user.len[0] == MUZIX_RTC_STRUCT_SIZE, 4);

    /* Refused in window 0 and window 3, with nothing copied in either way. */
    put_user(U_W0, &want, sizeof(want));
    xfer_reset(&g_from_user);
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_SETRTC, (int32_t)U_W0, 0, 0) == -1, 5);
    CHECK(g_from_user.calls == 1 && g_from_user.ok == 0, 6);
    CHECK(g_to_user.calls == 0, 9);
    put_user(U_W3, &want, sizeof(want));
    xfer_reset(&g_from_user);
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_SETRTC, (int32_t)U_W3, 0, 0) == -1, 7);
    CHECK(g_from_user.calls == 1 && g_from_user.ok == 0, 8);
    CHECK(g_to_user.calls == 0, 9);
    CHECK(call_user(MUZIX_USER_SYS_SETRTC, 0, 0, 0) == -1, 1);

    /* SYS_GETRTC hands the reading back whether or not the chip agreed. */
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_GETRTC, (int32_t)U_BUF, 0, 0) == 0, 1);
    CHECK(g_to_user.calls == 1 && g_to_user.len[0] == MUZIX_RTC_STRUCT_SIZE, 2);
    user_get(U_BUF, &got, sizeof(got));
    CHECK(got.second == 11u && got.hour == 3u, 3);
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    CHECK(call_user(MUZIX_USER_SYS_GETRTC, (int32_t)U_W0, 0, 0) == -1, 4);
    CHECK(call_user(MUZIX_USER_SYS_GETRTC, (int32_t)U_W3, 0, 0) == -1, 5);
    CHECK(call_user(MUZIX_USER_SYS_GETRTC, 0, 0, 0) == -1, 6);

    /* SYS_TIME is the same reading.  It answered with the scheduler's tick
     * counter, which stands still for as long as a process runs - so a program
     * that read it twice got the same number and one that looped on it spun. */
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    g_loop.tick = 4242u;
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_TIME, (int32_t)U_BUF, 0, 0) == 0, 7);
    user_get(U_BUF, &got, sizeof(got));
    CHECK(got.second == 11u && got.hour == 3u, 8);

    /* arg2 asks for the boot reading as well, which is what makes uptime
     * possible.  Without a good baseline it is refused rather than handing
     * back a dead chip's bytes, because subtracting those would produce a
     * large and entirely fictional answer. */
    muzix_boot_rtc_state = MUZIX_BOOT_RTC_REFUSED;
    CHECK(call_user(MUZIX_USER_SYS_GETRTC, (int32_t)U_BUF, (int32_t)U_BUF, 0)
          == -1, 9);
    muzix_boot_rtc_state = MUZIX_BOOT_RTC_GOOD;
    muzix_boot_rtc.second = 1u;
    muzix_boot_rtc.minute = 2u;
    muzix_boot_rtc.hour = 4u;
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_GETRTC, (int32_t)U_BUF, (int32_t)U_BUF, 0)
          == 0, 1);
    CHECK(g_to_user.calls == 2, 2);
    user_get(U_BUF, &got, sizeof(got));
    CHECK(got.hour == 4u, 3);
    return 0;
}

/* ============================== case 7: credentials ======================== */

static int test_credentials(void)
{
    muzix_kernel_proc_table_t *t = &g_loop.system.startup.proc_table;

    CHECK(call_user(MUZIX_USER_SYS_GETUID, 0, 0, 0) == 100, 1);
    CHECK(call_user(MUZIX_USER_SYS_GETGID, 0, 0, 0) == 200, 2);

    /* Only the caller's own id, or root, may change it. */
    CHECK(call_user(MUZIX_USER_SYS_SETUID, 101, 0, 0) == -1, 3);
    CHECK(call_user(MUZIX_USER_SYS_SETUID, -1, 0, 0) == -1, 4);
    CHECK(call_user(MUZIX_USER_SYS_SETUID, 0x10000, 0, 0) == -1, 5);
    CHECK(call_user(MUZIX_USER_SYS_SETUID, 100, 0, 0) == 0, 6);
    CHECK(g_ctx.uid == 100 && g_ctx.euid == 100, 7);
    CHECK(t->slots[0].uid == 100 && t->slots[0].euid == 100, 8);

    /* UMASK answers with the old mask and stores the new one on the slot, so
     * it survives the next call being attributed to the process table. */
    CHECK(call_user(MUZIX_USER_SYS_UMASK, 077, 0, 0) == 022, 9);
    CHECK(t->slots[0].umask == 077, 1);
    CHECK(call_user(MUZIX_USER_SYS_UMASK, 0022, 0, 0) == 077, 2);
    CHECK(t->slots[0].umask == 0022, 3);
    /* The mask is truncated to the permission bits rather than refused, so
     * 0xFFFF leaves 0777 and a negative argument leaves everything but the
     * low nine bits.  This is stated as it is: a mask with anything set above
     * 0777 is not something libc will produce, and the handler chose to mask
     * rather than to spend the kernel's two bytes of _CODE on a range check. */
    CHECK(call_user(MUZIX_USER_SYS_UMASK, 0xFFFF, 0, 0) == 0022, 4);
    CHECK(t->slots[0].umask == 0777, 5);
    CHECK(call_user(MUZIX_USER_SYS_UMASK, -1, 0, 0) == 0777, 6);
    CHECK(t->slots[0].umask == 0777, 7);

    /* Root may change either, and the answer is written back to the slot so
     * that the next call, which refreshes the context from the table, agrees.
     * Note that ctx->euid cannot be set directly to mean "root" here: the
     * handler's prologue overwrites it from the process slot on every call,
     * which is the property these cases exist to hold on to. */
    t->slots[0].euid = 0;
    t->slots[0].egid = 0;
    CHECK(call_user(MUZIX_USER_SYS_SETUID, 300, 0, 0) == 0, 7);
    CHECK(g_ctx.uid == 300 && g_ctx.euid == 300, 8);
    CHECK(t->slots[0].uid == 300 && t->slots[0].euid == 300, 9);
    CHECK(call_user(MUZIX_USER_SYS_GETUID, 0, 0, 0) == 300, 1);

    /* Back to root for the group.  Stated because it reads as a bug: the
     * SETUID above left euid at 300, and a non-root process may not change a
     * group it is not in, so the privilege has to be restored in the process
     * table rather than left behind in the context. */
    t->slots[0].euid = 0;
    t->slots[0].egid = 0;
    CHECK(call_user(MUZIX_USER_SYS_SETGID, 400, 0, 0) == 0, 2);
    CHECK(call_user(MUZIX_USER_SYS_GETGID, 0, 0, 0) == 400, 3);

    /* And a non-root process may not change either.  The group guard tests
     * euid rather than egid, so a process whose effective USER id is still
     * root can change its group while one whose effective GROUP id is root
     * cannot.  That is what the handler does; the two checks below pin it. */
    t->slots[0].euid = 300;
    t->slots[0].egid = 400;
    CHECK(call_user(MUZIX_USER_SYS_SETGID, 401, 0, 0) == -1, 4);
    CHECK(call_user(MUZIX_USER_SYS_SETUID, 302, 0, 0) == -1, 5);
    CHECK(t->slots[0].gid == 400 && t->slots[0].uid == 300, 6);
    t->slots[0].euid = 0;
    t->slots[0].egid = 0;
    CHECK(call_user(MUZIX_USER_SYS_SETGID, 402, 0, 0) == 0, 7);
    CHECK(t->slots[0].gid == 402, 8);
    return 0;
}

/* ============================== case 8: signals =========================== */

static int test_signals(void)
{
    /* The target is named by pid, not by slot - the two coincide at boot and
     * stop coinciding the moment a pid >= MUZIX_PROC_TABLE_MAX exists. */
    g_loop.system.startup.proc_table.slots[1].uid = 100;
    g_loop.system.startup.proc_table.slots[1].euid = 100;
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 2, 2, 0) == 0, 1);
    CHECK(g_loop.system.startup.proc_table.slots[1].pending_signals == 0x0002u, 2);
    CHECK(g_loop.system.startup.proc_table.slots[1].pending_signals ==
          (1u << (2 - 1)), 3);

    /* Signal 0 is not a signal and the count is bounded. */
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 2, 0, 0) == -1, 3);
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 2, 17, 0) == -1, 4);
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 2, -1, 0) == -1, 5);
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 99, 2, 0) == -1, 6);
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 0, 2, 0) == -1, 7);

    /* A process that does not own the target may not signal it. */
    g_loop.system.startup.proc_table.slots[1].uid = 0;
    g_loop.system.startup.proc_table.slots[1].euid = 0;
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 2, 3, 0) == -1, 8);
    CHECK(g_loop.system.startup.proc_table.slots[1].pending_signals == 0x0002u, 9);
    /* Root may.  In the process table, not in the context: the handler
     * refreshes ctx->euid from the slot on every call and a value set on the
     * context alone would be gone before the check that needs it. */
    g_loop.system.startup.proc_table.slots[0].euid = 0;
    /* Signal N is bit N-1, so 2 and 4 are 0x0002 and 0x0008.  The refused
     * signal 3 above left no trace at all, which is the property worth having:
     * a signal refused for want of privilege must not be delivered later. */
    CHECK(call_user(MUZIX_USER_SYS_SIGNAL, 2, 4, 0) == 0, 1);
    CHECK(g_loop.system.startup.proc_table.slots[1].pending_signals == 0x000Au, 2);
    return 0;
}

/* ============================== case 9: exec's arguments =================== */

/*
 * This is the syscall the argv bug went through, so it gets the closest
 * look.  The handler has to copy the path, then each argv element, then the
 * element itself, before it may call the loader - and the loader is the test's
 * own function, so it can be asked what it received.
 */
static int test_exec_passes_the_paths_and_the_arguments(void)
{
    static uint16_t argv[3];

    put_str(U_PATH, "/bin/cat");
    put_str(U_ARG1, "readme");
    put_str(U_ARG2, "hw-notes");
    argv[0] = U_ARG1;
    argv[1] = U_ARG2;
    argv[2] = 0;
    put_user(0x8040u, argv, sizeof(argv));

    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_PATH, (int32_t)0x8040u, 0x7F00)
          == 0, 1);
    CHECK(g_loader_calls == 1, 2);
    CHECK(strcmp(g_loader_path, "/bin/cat") == 0, 3);
    CHECK(g_loader_argc == 2, 4);
    CHECK(strcmp(g_loader_arg0, "readme") == 0, 5);
    CHECK(strcmp(g_loader_arg1, "hw-notes") == 0, 6);
    CHECK(g_loader_slot == 0, 7);
    CHECK((g_loop.system.startup.proc_table.slots[0].p_flags & MUZIX_PROC_NO_MAP)
          == 0, 8);
    CHECK(g_loop.system.startup.proc_table.slots[0].stack_ptr == 0x7F00, 9);

    /* One argument. */
    put_str(U_ARG1, "ls");
    argv[0] = U_ARG1;
    argv[1] = 0;
    put_user(0x8040u, argv, sizeof(argv));
    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_PATH, (int32_t)0x8040u, 0)
          == 0, 1);
    CHECK(g_loader_argc == 1 && strcmp(g_loader_arg0, "ls") == 0, 2);

    /* No argv at all: the loader must be handed a NULL vector rather than a
     * pointer to an empty one, because exec_load_args walks it. */
    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_PATH, 0, 0) == 0, 3);
    CHECK(g_loader_calls == 1 && g_loader_argc == 0, 4);

    /* An empty argv vector terminates at the first null, and nothing beyond it
     * is read. */
    argv[0] = 0;
    put_user(0x8040u, argv, sizeof(argv));
    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_PATH, (int32_t)0x8040u, 0)
          == 0, 5);
    CHECK(g_loader_argc == 0, 6);

    /* The refusals.  A path in window 0, an argument vector in window 0, an
     * argument in window 3, a NULL path: each is refused before the loader is
     * reached.  The first of these is the argv bug exactly - argv holding an
     * address below 0x4000 and the kernel being asked to read a string from
     * its own text window. */
    put_str(U_W0, "/bin/ls");
    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_W0, 0, 0) == -1, 7);
    CHECK(g_loader_calls == 0, 8);
    CHECK(call_user(MUZIX_USER_SYS_EXEC, 0, 0, 0) == -1, 9);
    put_str(U_PATH, "/bin/cat");
    put_str(U_ARG1, "readme");
    argv[0] = U_ARG1;
    argv[1] = 0;
    put_user(U_W0, argv, sizeof(argv));
    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_PATH, (int32_t)U_W0, 0) == -1, 1);
    CHECK(g_loader_calls == 0, 2);
    put_str(U_W3, "readme");
    argv[0] = U_W3;
    put_user(0x8040u, argv, sizeof(argv));
    g_loader_calls = 0;
    CHECK(call_user(MUZIX_USER_SYS_EXEC, (int32_t)U_PATH, (int32_t)0x8040u, 0) == -1, 3);
    CHECK(g_loader_calls == 0, 4);
    return 0;
}

/* ============================== case 10: fork ============================= */

/*
 * The fork handler's own job, as distinct from the resume bookkeeping, which is
 * kernel/test_proc_context.c's.  What belongs here is the stack copy: the
 * parent's active stack has to be read out of the parent's page and written
 * into a page of the child's own, in pieces, because it is larger than one
 * transfer.  When it was not, the copy was refused, the code quietly fell back
 * to letting the two share a page, and the child ran its own call on top of
 * the frames its parent was suspended in.
 */
static int test_fork_copies_the_stack_into_a_page_of_the_childs(void)
{
    muzix_proc_slot_t *parent = &g_loop.system.startup.proc_table.slots[0];
    muzix_proc_slot_t *child;
    int32_t rc;

    /* What the trap leaves behind for the handler to record: where the process
     * was when it made the call, and how much of its stack is in use.  349
     * bytes is the figure measured on the board for an ordinary fork of the
     * shell - it is over the 256-byte staging cap, which is the whole point. */
    muzix_resume_pc = 0x83BEu;
    muzix_resume_sp = 0x7FAEu;
    muzix_fork_stack_len = 349u;
    muzix_switch_pending = 0;

    xfer_reset(&g_to_user);
    xfer_reset(&g_from_user);
    rc = call_user(MUZIX_USER_SYS_FORK, 0, 0, 0);
    CHECK(rc == 3, 1);                                  /* slot 2 is taken */

    child = &g_loop.system.startup.proc_table.slots[2];
    CHECK(child->active && child->pid == 3, 2);
    CHECK(child->parent_pid == 1, 3);

    /* The child got a page of its own for its stack, and it is recorded as the
     * child's so that exiting it returns that page rather than the parent's.
     *
     * This single assertion is the chunking.  muzix_fork_stack_len is 349,
     * which is more than the 256-byte staging cap, so one transfer would be
     * refused by the primitive in test_host/zeta_host.c - the same refusal the
     * target makes - the loop would give the page straight back, and the two
     * processes would share a stack again.  They did: the child then ran its
     * own `call exec` over the frames its parent was suspended in, and when
     * the exec'd program exited the parent came back at the instruction after
     * the *child's* exec() and reported that every command had failed. */
    CHECK(child->map.pages[1] != parent->map.pages[1], 4);
    CHECK(child->map.pages[1] >= 0x20u && child->map.pages[1] <= 0x3Fu, 5);
    CHECK(child->owned_count == 1 && child->owned_pages[0] == child->map.pages[1], 6);
    CHECK(parent->owned_count == 0 || parent->owned_pages[0] != child->map.pages[1],
          7);

    /* Both sides resume at the same instruction and the same stack pointer,
     * because that is where each of them left off and the child's stub has to
     * find the return addresses its parent's would have found. */
    CHECK(child->resume_pc == 0x83BEu && child->resume_sp == 0x7FAEu, 8);
    CHECK(child->resume_valid == 1, 9);

    /* And the value each of them will return from the syscall it is sitting
     * in: the child returns 0, because the call it is inside was made by the
     * parent, and the parent returns the child's pid.  These two are one bit
     * apart and swapping them is what made a child believe it had forked, so
     * both are asserted here as well as from the other side in
     * kernel/test_proc_context.c. */
    CHECK(child->resume_ret == 0, 1);
    CHECK(parent->resume_ret == 3, 2);
    CHECK(parent->resume_pc == 0x83BEu && parent->resume_sp == 0x7FAEu &&
          parent->resume_valid == 1, 3);

    /* And the fork left the table pointing at the child, because the child is
     * what the CPU goes on to. */
    CHECK(g_loop.system.startup.proc_table.current == 2, 1);

    /* The next fork takes the next free slot, with its own page again. */
    g_loop.system.startup.proc_table.current = 0;
    CHECK(call_user(MUZIX_USER_SYS_FORK, 0, 0, 0) == 4, 4);
    child = &g_loop.system.startup.proc_table.slots[3];
    CHECK(child->active && child->pid == 4, 5);
    CHECK(child->map.pages[1] != parent->map.pages[1] &&
          child->map.pages[1] != g_loop.system.startup.proc_table.slots[2].map.pages[1],
          6);
    CHECK(child->resume_ret == 0, 7);
    CHECK(parent->resume_ret == 4, 8);

    /* And a fork with nowhere to put a child is refused rather than stealing a
     * live slot: every one of the four is in use now. */
    g_loop.system.startup.proc_table.current = 0;
    CHECK(call_user(MUZIX_USER_SYS_FORK, 0, 0, 0) == -1, 9);

    /* A stack the entry could not measure leaves the child sharing the parent's
     * page rather than being a table entry with nowhere to run.  That is the
     * fallback every implementation had before the copy existed, and it has to
     * stay reachable: the assertions above are about the copy succeeding, not
     * about the fallback being impossible. */
    g_loop.system.startup.proc_table.slots[3].active = 0;
    g_loop.system.startup.proc_table.slots[3].pid = 0;
    muzix_fork_stack_len = 0;
    g_loop.system.startup.proc_table.current = 0;
    CHECK(call_user(MUZIX_USER_SYS_FORK, 0, 0, 0) == 4, 1);
    child = &g_loop.system.startup.proc_table.slots[3];
    CHECK(child->active && child->map.pages[1] == parent->map.pages[1], 2);
    CHECK(child->owned_count == 0, 3);
    CHECK(child->resume_valid == 1 && child->resume_pc == 0x83BEu &&
          child->resume_sp == 0x7FAEu, 4);
    CHECK(child->resume_ret == 0, 5);
    return 0;
}

/* ============================== case 11: the null cases =================== */

/*
 * Every one of these is a value the handler reads out of a context it was
 * handed.  A NULL kernel is the case that mattered this week: the copy helpers
 * resolve the running process by walking ctx->kernel->loop, so a handler that
 * does not check has already lost the argument by the time it starts.
 */
static int test_a_null_context_is_refused_everywhere(void)
{
    muzix_userspace_syscall_context_t saved = g_ctx;

    /* Three groups, because the two NULLs in the context fail for different
     * reasons and lumping them together would have this file asserting
     * refusals the handler has no reason to make. */

    /* Syscalls that have to resolve the running process, and so cannot answer
     * without a kernel behind the context. */
    static const int32_t needs_kernel[] = {
        MUZIX_USER_SYS_EXIT,   MUZIX_USER_SYS_FORK,   MUZIX_USER_SYS_EXEC,
        MUZIX_USER_SYS_WAIT,   MUZIX_USER_SYS_GETRTC, MUZIX_USER_SYS_SETRTC,
        MUZIX_USER_SYS_TIMES,  MUZIX_USER_SYS_PROCTAB, MUZIX_USER_SYS_SIGNAL,
        MUZIX_USER_SYS_OPEN,   MUZIX_USER_SYS_READ,   MUZIX_USER_SYS_WRITE,
        MUZIX_USER_SYS_MKDIR,  MUZIX_USER_SYS_GETCWD, MUZIX_USER_SYS_STAT,
        MUZIX_USER_SYS_PIPE,   MUZIX_USER_SYS_IOCTL
    };

    /* Syscalls that consult the filesystem.  These are refused without one,
     * which is the point of checking ctx->fs before the chain rather than
     * inside each arm of it. */
    static const int32_t needs_fs[] = {
        MUZIX_USER_SYS_OPEN,   MUZIX_USER_SYS_CLOSE,  MUZIX_USER_SYS_READ,
        MUZIX_USER_SYS_WRITE,  MUZIX_USER_SYS_SEEK,   MUZIX_USER_SYS_MKDIR,
        MUZIX_USER_SYS_RMDIR,  MUZIX_USER_SYS_CHDIR,  MUZIX_USER_SYS_GETCWD,
        MUZIX_USER_SYS_UNLINK, MUZIX_USER_SYS_RENAME, MUZIX_USER_SYS_STAT,
        MUZIX_USER_SYS_FSTAT,  MUZIX_USER_SYS_CREAT,  MUZIX_USER_SYS_LINK,
        MUZIX_USER_SYS_DUP,    MUZIX_USER_SYS_CHMOD,  MUZIX_USER_SYS_ACCESS,
        MUZIX_USER_SYS_PIPE,   MUZIX_USER_SYS_IOCTL,  MUZIX_USER_SYS_SYNC
    };

    /* Syscalls that touch neither: a descriptor passed straight down, or a
     * number answered from the context. */
    static const int32_t kernel_free[] = {
        MUZIX_USER_SYS_CLOSE, MUZIX_USER_SYS_DUP, MUZIX_USER_SYS_SYNC
    };
    size_t i;

    /* The context itself, for every class. */
    CHECK(muzix_handle_userspace_syscall(0, MUZIX_USER_SYS_OPEN, 0, 0, 0) == -1, 1);
    CHECK(muzix_handle_userspace_syscall(0, MUZIX_USER_SYS_GETPID, 0, 0, 0) == -1, 2);

    g_ctx.kernel = 0;
    for (i = 0; i < sizeof(needs_kernel) / sizeof(needs_kernel[0]); i++) {
        seen_reset();
        g_loader_calls = 0;
        xfer_reset(&g_to_user);
        /* arg3 is 1 rather than 0 so that a read or a write of zero bytes
         * cannot pass as a refusal: a zero-length read is a successful read of
         * nothing, and a loop that never runs would satisfy any expectation
         * of -1 that did not look at the copy. */
        CHECK(muzix_handle_userspace_syscall(&g_ctx, needs_kernel[i],
                                             (int32_t)U_BUF, (int32_t)U_BUF, 1)
              == -1, 3);
        /* Refused before a byte reached the caller, before the exec loader was
         * reached, and before the table moved. */
        CHECK(g_to_user.ok == 0, 4);
        CHECK(g_loader_calls == 0, 5);
        CHECK(g_loop.system.startup.proc_table.current == 0, 6);
    }
    /* The three that are independent of the kernel still run, because the
     * filesystem is present and nothing about them needs a process.  A test
     * that lumped them in with the list above would be asserting a refusal
     * that would cost the kernel bytes to add. */
    for (i = 0; i < sizeof(kernel_free) / sizeof(kernel_free[0]); i++) {
        seen_reset();
        CHECK(muzix_handle_userspace_syscall(&g_ctx, kernel_free[i], 3,
                                             (int32_t)U_BUF, 0) >= 0, 7);
        CHECK(g_seen.calls == 1, 8);
    }
    CHECK(muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_GETPID, 0, 0, 0)
          == 1, 9);
    g_ctx = saved;

    /* No filesystem. */
    g_ctx.fs = 0;
    for (i = 0; i < sizeof(needs_fs) / sizeof(needs_fs[0]); i++) {
        seen_reset();
        g_loader_calls = 0;
        xfer_reset(&g_to_user);
        CHECK(muzix_handle_userspace_syscall(&g_ctx, needs_fs[i], (int32_t)U_BUF,
                                             (int32_t)U_BUF, 1) == -1, 1);
        CHECK(g_seen.calls == 0, 2);
        CHECK(g_to_user.calls == 0, 3);
        CHECK(g_loader_calls == 0, 4);
    }
    /* SYS_TIMES and SYS_PROCTAB report the table to the caller and do not
     * consult a filesystem, so a filesystem-less context does not stop them.
     * SYS_EXEC is the same: it loads through the loader, which reads the image
     * out of ROM. */
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(struct proc_info));
    CHECK(muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_TIMES,
                                         (int32_t)U_BUF, 0, 0) == 0, 5);
    g_loader_calls = 0;
    CHECK(muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_EXEC,
                                         (int32_t)U_PATH, 0, 0) == 0, 6);
    CHECK(g_loader_calls == 1, 7);

    CHECK(muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_GETPID, 0, 0, 0)
          == 1, 8);
    CHECK(muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_GETUID, 0, 0, 0)
          == 100, 9);
    g_ctx = saved;
    return 0;
}

/* ============================== case 12: readdir ========================== */

/*
 * muzix_fs_service_readdir answers 0 for an entry, 1 for the end of the
 * directory and -1 for a failure, and only the first of those is worth copying
 * back.  The handler tested `drc <= 0`, which took the success path out: the
 * root directory's ten entries were all reported to `ls` as read, every one
 * arrived as whatever the caller's uninitialised struct dirent already held,
 * and `ls` printed ten blank lines.  The return value was correct throughout.
 */
static int test_readdir_copies_the_entry_when_it_is_an_entry(void)
{
    muzix_fs_dirent_t dirent;

    memset(&dirent, 0, sizeof(dirent));
    dirent.inode = 0xBEEFu;
    strcpy(dirent.name, "previous");
    put_user(U_BUF, &dirent, sizeof(dirent));

    seen_reset();
    g_seen_readdir_rc = 0;
    g_seen_readdir_inode = 7;
    CHECK(call_user(MUZIX_USER_SYS_READDIR, 3, 0, (int32_t)U_BUF) == 0, 1);
    CHECK(seen_reached("readdir") && g_seen.fd == 3 && g_seen.index == 0, 2);
    user_get(U_BUF, &dirent, sizeof(dirent));
    CHECK(dirent.inode == 7, 3);
    CHECK(strcmp(dirent.name, "hw-notes") == 0, 4);

    /* The end of the directory is 1, not an error, and nothing is copied. */
    g_seen_readdir_rc = 1;
    CHECK(call_user(MUZIX_USER_SYS_READDIR, 3, 1, (int32_t)U_BUF) == 1, 5);

    /* A failure is -1 and the caller's buffer is left as it was. */
    g_seen_readdir_rc = -1;
    user_get(U_BUF, &dirent, sizeof(dirent));
    CHECK(call_user(MUZIX_USER_SYS_READDIR, 3, 2, (int32_t)U_BUF) == -1, 6);
    {
        muzix_fs_dirent_t after;

        user_get(U_BUF, &after, sizeof(after));
        CHECK(strcmp(after.name, "hw-notes") == 0, 7);
    }

    /* The caller's buffer is read as well as written: an entry is seeded by the
     * caller and the handler must not require it to be zero. */
    g_seen_readdir_rc = 0;
    CHECK(call_user(MUZIX_USER_SYS_READDIR, 3, 3, (int32_t)U_W0) == -1, 8);
    return 0;
}

/* ============================== case 13: getcwd =========================== */

static int test_getcwd(void)
{
    char got[64];

    strcpy(g_seen_getcwd, "/bin");
    memset(&muzix_host_user_space()[U_BUF], 0, sizeof(got));
    CHECK(call_user(MUZIX_USER_SYS_GETCWD, (int32_t)U_BUF, 32, 0) == 0, 1);
    user_get(U_BUF, got, sizeof(got));
    CHECK(strcmp(got, "/bin") == 0, 2);

    /* The length is required, and a non-positive one is refused. */
    CHECK(call_user(MUZIX_USER_SYS_GETCWD, 0, 32, 0) == -1, 3);
    CHECK(call_user(MUZIX_USER_SYS_GETCWD, (int32_t)U_BUF, 0, 0) == -1, 4);
    CHECK(call_user(MUZIX_USER_SYS_GETCWD, (int32_t)U_BUF, -1, 0) == -1, 5);

    /* A buffer in a window the kernel owns is refused.  The filesystem is
     * reached first and its answer is discarded, which is the same asymmetry as
     * SYS_READ: the kernel buffer it was written into is not the caller's. */
    seen_reset();
    xfer_reset(&g_to_user);
    CHECK(call_user(MUZIX_USER_SYS_GETCWD, (int32_t)U_W0, 32, 0) == -1, 6);
    CHECK(seen_reached("getcwd"), 7);
    CHECK(g_to_user.calls == 1 && g_to_user.ok == 0, 8);
    return 0;
}

/* ============================================================== the file === */

/*
 * WHAT THE HOST CANNOT REACH, IN THIS FILE
 * -----------------------------------------
 * The trap itself: platform/zeta-v2/syscall_entry.s, which builds the frame,
 * captures muzix_resume_pc/sp and muzix_fork_stack_len, and calls this handler.
 * It is window-0 assembly, and the reason muzix_fork_stack_len exists at all.
 * The handler only reads what it was handed, so a test that sets those cells
 * exercises the handler and not the capture.
 *
 * Which page an address resolves to.  The flat 64 KiB in test_host has no
 * banks, so the window refusals are exercised and the page arithmetic behind
 * them is not.
 *
 * The window-0 handoff muzix_zeta_enter_userspace() performs after a successful
 * exec.  test_host/z80_io_host.c makes it a no-op, so the exec case proves the
 * loader was called with the right arguments and nothing about what the CPU did
 * next.
 *
 * The register convention as the compiler applies it to a real module: the
 * calling-convention case measures it for a probe, which is the mapping, but
 * what a given call site generated has to be read off that site's .lst.
 *
 * And fs/test_fs_service_io.c, which is the only test that would reach
 * muzix_fs_service_* for real.  It needs 64 KiB of fake user space, a live MM,
 * a live process slot and a volume mounted on a block device.  That is a
 * harness, not a port of anything here, and this file's seam cannot stand in
 * for it: the seam exists so a test can read what the handler passes down, and
 * it therefore cannot also be the thing that decides whether an open succeeds.
 */

int main(void)
{
    static int (*const cases[])(void) = {
        test_calling_convention,
        test_dispatch_and_identity,
        test_sys_copy_arguments_reach_the_memory_manager,
        test_paths_are_copied_from_the_caller,
        test_a_path_that_will_not_copy_in_is_refused,
        test_read_and_write_carry_the_bytes,
        test_windows_zero_and_three_are_the_kernels,
        test_the_clock_arguments,
        test_credentials,
        test_signals,
        test_exec_passes_the_paths_and_the_arguments,
        test_fork_copies_the_stack_into_a_page_of_the_childs,
        test_a_null_context_is_refused_everywhere,
        test_readdir_copies_the_entry_when_it_is_an_entry,
        test_getcwd
    };
    unsigned i;
    int rc;

    /* A fresh fixture per case, not one shared by all of them.  Two of the
     * cases above deliberately leave the loop with nothing running - a YIELD
     * and a switch-to-kernel both clear it - and a case that inherited that
     * would be testing a handler that cannot resolve a process, which is a
     * true statement about the code and a false one about the case. */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup();
        rc = cases[i]();
        if (rc != 0) {
            /* case number * 10 + what went wrong, so the exit status names it */
            return (int)(i + 1) * 10 + rc;
        }
    }
    return 0;
}
