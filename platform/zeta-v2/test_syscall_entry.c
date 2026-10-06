#include <string.h>

#include "syscall_entry.h"

#include "../../kernel/kernel_loop.h"
#include "../../kernel/proc_table.h"
#include "../../kernel/syscalls.h"
#include "../../mm/mm_service.h"
#include "../../lib/syscall.h"

/*
 * Rewritten, because the API it was written against no longer exists.
 *
 * It tested `muzix_z80_syscall_frame_t` - a saved-AF frame - through
 * `muzix_z80_dispatch_frame(frame)` and `muzix_z80_read_syscall_args(frame,
 * args)`.  The frame is gone; what the entry stages in kernel memory now is
 * `muzix_z80_syscall_args_t` {number, pad, arg[3]}, and the header says why: the
 * handler runs with window 1 mapped to the kernel page, so the process's own
 * stack cannot be relied on to hold anything.  `muzix_z80_read_syscall_args` has
 * no definition anywhere in the tree - only this file mentioned it - and
 * `muzix_z80_dispatch_frame` now takes a zeta_state_t as well.  So this file had
 * not compiled since that change, which is how it came to be one of the tests in
 * this tree that nobody had ever run.
 *
 * What is below is the surface that IS reachable from a host, and it is the part
 * worth having.  muzix_z80_userspace_handler() is where a number staged by
 * lib/user_syscall.s becomes a kernel dispatch, so it is the seam every syscall
 * crosses, SYS_LOAD included.  What arrives there is three words and a byte, and
 * getting the byte to be a byte is worth asserting on a host where it can be
 * asserted at all.
 *
 * What is NOT here, and why
 * -------------------------
 * muzix_z80_dispatch_frame() with a handler installed calls zeta_kernel_enter(),
 * which programs the bank registers and then zeta_read_visible()/
 * zeta_write_visible() at real 16-bit addresses.  That is the same wall as
 * platform/zeta-v2/test_kernel/test_bank_model.c and
 * platform/zeta-v2/test_kernel/test_minix_adapter.c, which say so themselves in
 * their own host-test-not-portable comments: on the target the window is a
 * hardware register the CPU addresses and there is nothing to model it with, and
 * on a 64-bit host the same expression is an access at address 0x0100 that
 * faults.  The old test asserted the map hooks ran; that assertion is gone and
 * the frame path is covered on the target instead.
 *
 * The one thing about dispatch_frame a host CAN see is asserted below: with no
 * handler installed it must return 0xffff before it touches the map at all,
 * because 0xffff is what the entry reads as "no result".
 */

static muzix_kernel_loop_t g_loop;
static muzix_mm_service_t g_mm;
static muzix_syscall_context_t g_kctx;
static muzix_mproc_t g_mp;
static process_map_t g_map;

/* Slot 0 is pid 7 with parent 1.  GETPID has to answer 7 whatever is in the
 * struct the caller filled in, because muzix_handle_userspace_syscall()
 * overwrites ctx->pid from the process table - which is the point: a pid read
 * from the table is the one that moves with the context switch. */
static void bind(void)
{
    memset(&g_loop, 0, sizeof(g_loop));
    memset(&g_mm, 0, sizeof(g_mm));
    memset(&g_kctx, 0, sizeof(g_kctx));
    memset(&g_mp, 0, sizeof(g_mp));
    memset(&g_map, 0, sizeof(g_map));

    muzix_mm_service_init(&g_mm, 0, 0);
    muzix_kernel_loop_init(&g_loop, &g_map, 7, &g_map);
    muzix_kernel_loop_register(&g_loop, 0, 7, &g_mp, &g_map,
                               MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    g_loop.mm = &g_mm;
    g_loop.current_pid = 7;
    g_loop.system.startup.proc_table.current = 0;
    g_loop.system.startup.proc_table.slots[0].pid = 7;
    g_loop.system.startup.proc_table.slots[0].parent_pid = 1;

    g_kctx.loop = &g_loop;
    g_kctx.mm = &g_mm;
    g_kctx.process_table = &g_loop.system.startup.proc_table;

    muzix_z80_bind_userspace_context(0);
}

/* No context bound: every entry point has to say so rather than follow a null
 * pointer.  0xffff is what the assembly reads as "no result"; -1 is what the
 * int32 entry - the one lib/syscall.c's SYSCALL() reaches through the installed
 * pointer - returns, and it is the value test_host/syscall_host.c then leaves in
 * syscall_result. */
static int test_unbound(void)
{
    muzix_z80_syscall_args_t args;

    memset(&args, 0, sizeof(args));

    muzix_z80_bind_userspace_context(0);
    muzix_z80_set_syscall_handler(0);

    if (muzix_z80_userspace_handler(0) != 0xffffu) {
        return 1;
    }
    if (muzix_z80_userspace_handler(&args) != 0xffffu) {
        return 2;
    }
    if (muzix_z80_userspace_syscall_entry(SYS_GETPID, 0, 0, 0) != -1) {
        return 3;
    }

    /* No handler installed, so dispatch must answer 0xffff BEFORE it programs a
     * bank register.  This is the one frame-level behaviour a host can reach,
     * and reaching it is the check that the early return is where it is. */
    if (muzix_z80_dispatch_frame(0, &args) != 0xffffu) {
        return 4;
    }
    return 0;
}

/* Bound: the number and the arguments cross into the kernel.  The number is the
 * interesting one - the entry writes it as a BYTE and `number` is a uint8_t, so
 * a word read here would pick up whatever the linker placed above it. */
static int test_bound(void)
{
    muzix_userspace_syscall_context_t ctx;
    muzix_z80_syscall_args_t args;

    bind();

    memset(&ctx, 0, sizeof(ctx));
    ctx.kernel = &g_kctx;
    ctx.fs = 0;
    muzix_z80_bind_userspace_context(&ctx);
    muzix_z80_set_syscall_handler(muzix_z80_userspace_handler);

    memset(&args, 0, sizeof(args));
    args.number = SYS_GETPID;
    if (muzix_z80_userspace_handler(&args) != 7u) {
        return 1;
    }
    if (muzix_z80_userspace_handler(0) != 0xffffu) {
        return 2;
    }

    args.number = SYS_GETPPID;
    if (muzix_z80_userspace_handler(&args) != 1u) {
        return 3;
    }

    /* The 16-bit result is the whole of the contract here: the entry reads the
     * handler's return as a 16-bit value, so anything wider is truncated on the
     * way back and a caller comparing against the full pid would see a mismatch
     * that no host test of the dispatcher alone would explain. */
    args.number = SYS_GETPID;
    args.arg[0] = 0xffffffffu;
    if (muzix_z80_userspace_handler(&args) != 7u) {
        return 4;
    }

    /* Unbinding takes effect at once: the handler is left installed but has
     * nothing to dispatch through. */
    muzix_z80_bind_userspace_context(0);
    if (muzix_z80_userspace_handler(&args) != 0xffffu) {
        return 5;
    }

    /* And a context that has no kernel loop behind it cannot resolve a slot, so
     * the pid it reports is the one in the struct - here zero, not 7. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.fs = 0;
    ctx.pid = 9;
    muzix_z80_bind_userspace_context(&ctx);
    if (muzix_z80_userspace_handler(&args) != 9u) {
        return 6;
    }

    muzix_z80_bind_userspace_context(0);
    muzix_z80_set_syscall_handler(0);
    return 0;
}

int main(void)
{
    int rc = test_unbound();

    if (rc != 0) {
        return rc;
    }
    return test_bound();
}