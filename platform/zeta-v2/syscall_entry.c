#include "syscall_entry.h"
#include "trace.h"

#include "../../kernel/syscalls.h"
#include "context_switch.h"

/* Not function pointers any more - see muzix_z80_dispatch_frame.  These are
 * configuration flags: "a context is bound" and "the map hooks are wired up".
 * That is all the NULL checks in the dispatcher actually want to know. */
static muzix_z80_syscall_handler_t syscall_handler;
static muzix_z80_mapping_hook_t enter_kernel_hook;
static muzix_z80_mapping_hook_t restore_user_hook;

/* The process map, saved across the handler.  It lives here rather than in
 * syscall_runtime.c because the dispatcher now performs the map switch itself:
 * the hooks could not be called from here, and calling them indirectly was what
 * broke. */
static zeta_saved_context_t g_saved_user_context;
static uint16_t g_syscall_result;

/* args parked in memory for the same reason g_syscall_result is: it arrives in
 * HL - sdcccall(1) puts param1 there - and two calls sit between the entry and
 * the handler, one of which is inline assembly that loads HL with an address.
 * The compiler has no way to know that, so a value left in HL across
 * muzix_trace_stack_mark() or zeta_kernel_enter() is simply lost. Which
 * processes survived was not stable between runs, because it came out of the
 * register allocation for this build. */
static muzix_z80_syscall_args_t *g_syscall_args;

/* Published by the kernel layer; see the note on the restore hook below. */
extern uint16_t muzix_switch_pending;
static zeta_state_t *g_zeta_state;
static muzix_userspace_syscall_context_t *userspace_context;

void muzix_z80_set_syscall_handler(muzix_z80_syscall_handler_t handler)
{
    syscall_handler = handler;
}

void muzix_z80_set_mapping_hooks(muzix_z80_mapping_hook_t enter_kernel,
                                 muzix_z80_mapping_hook_t restore_user)
{
    enter_kernel_hook = enter_kernel;
    restore_user_hook = restore_user;
}

/* The cell the assembly handoff reads. It is a 2-byte .dw holding a
 * zeta_state_t*, so C must treat it as a single pointer. kernel_main used to
 * declare it `zeta_state_t **` and pass &g_zeta_state_ptr, which made the
 * cell end up holding a pointer to itself; the handoff then saw a state of
 * zero, refused, and spun forever. Publish the value directly instead. */
extern zeta_state_t *g_zeta_state_ptr;

void muzix_z80_set_zeta_state(zeta_state_t *state)
{
    g_zeta_state = state;
    g_zeta_state_ptr = state;
}

void muzix_z80_set_zeta_state_ptr(zeta_state_t **ptr)
{
    /* Retained for API compatibility. The shared cell is a single pointer;
     * treating it as a pointer-to-pointer is what left the userspace handoff
     * reading a state of zero, refusing, and spinning forever. */
    (void)ptr;
    if (g_zeta_state) {
        g_zeta_state_ptr = g_zeta_state;
    }
}

void muzix_z80_bind_userspace_context(
    struct muzix_userspace_syscall_context *context)
{
    userspace_context = context;
    if (context) {
        syscall_handler = muzix_z80_userspace_handler;
    } else {
        syscall_handler = 0;
    }
}

uint16_t muzix_z80_userspace_handler(muzix_z80_syscall_args_t *args)
{
    if (!userspace_context || !args) {
        return 0xffffu;
    }
    /* The number arrives as a byte the entry parked below the argument block.
     * It is read the same way lib/user_syscall.s builds it - the low byte of
     * `num`, with the high word already known to be zero for every call in the
     * table. */
    return (uint16_t)muzix_handle_userspace_syscall(
        userspace_context,
        (int32_t)args->number,
        (int32_t)args->arg[0],
        (int32_t)args->arg[1],
        (int32_t)args->arg[2]);
}

int32_t muzix_z80_userspace_syscall_entry(int32_t syscall_num,
                                          int32_t arg1,
                                          int32_t arg2,
                                          int32_t arg3)
{
    if (!userspace_context) {
        return -1;
    }
    return muzix_handle_userspace_syscall(
        userspace_context, syscall_num, arg1, arg2, arg3);
}

uint16_t muzix_z80_dispatch_frame(zeta_state_t *state,
                                  muzix_z80_syscall_args_t *args)
{
    g_syscall_args = args;

    muzix_trace_stack_mark();

    if (!syscall_handler) {
        return 0xffffu;
    }
    if (enter_kernel_hook) {
        zeta_kernel_enter(state, &g_saved_user_context);
    }

    /* Every call on this path is direct.  An indirect one goes through
     * ___sdcc_call_iy, which platform/zeta-v2/muzix_sdccall.peep rewrites to a
     * hand-written `jp (iy)` because --peep-file replaces SDCC's entire default
     * peephole set.  That rewrite does not work: the generated sequence is
     *
     *     ld  hl, #_enter_kernel_hook
     *     or  a, (hl)              ; NULL test
     *     call _muzix_sdz80_call_iy
     *
     * with no `ld iy, #_enter_kernel_hook` anywhere, so IY still held whatever
     * the previous call left there and `jp (iy)` transferred into _DATA.  The
     * first userspace syscall never reached the handler, the handler never
     * reached the filesystem, and the process died in the middle of the kernel's
     * own variables.  Both the handler and the two map hooks live in modules
     * this file can call directly, so the indirection is simply gone. */
    /* The result is parked in memory across the map restore, not just in a
     * local.  The return value is the 16-bit result in HL, and the restore hook
     * is a full C call underneath us: SDCC left HL holding whatever that call
     * produced, so a read that had copied one byte correctly still handed the
     * process 35 instead of 1, and getchar() read that as end of file.  This is
     * the same failure as the entry clobbering HL with `ld hl,(user_sp)`, one
     * level up, and the lesson is the same: do not leave anything that matters
     * in a register across a call this build does not get right. */
    g_syscall_result = muzix_z80_userspace_handler(g_syscall_args);

    /* Restore the user's map only if we are going back to *this* process.
     *
     * A handler may have asked for a context switch, in which case control is
     * about to leave for a different process and the switch installs that
     * process's map itself. Restoring the user's map first would hand window 3
     * back to the calling process while the kernel still has its switch
     * parameters to read: muzix_resume_pc/sp/ret, muzix_resume_banks and
     * _g_zeta_state_ptr all live in _DATA, which is window 3. The trap's exit
     * path then reads them out of a process page instead of the kernel's, got
     * a zero state pointer, and zeta_resume_process refused - the process
     * waiting to be resumed was the one that hung.
     *
     * So the switch has to happen with the kernel map still installed, which
     * is what skipping this does. A refused switch still returns here with
     * switch_pending cleared, so the normal restore is not lost. */
    if (restore_user_hook && !muzix_switch_pending) {
        zeta_kernel_exit(state, &g_saved_user_context);
    }
    return g_syscall_result;
}

