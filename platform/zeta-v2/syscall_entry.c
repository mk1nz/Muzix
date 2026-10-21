#include "syscall_entry.h"
#include "trace.h"

#include "../../kernel/syscalls.h"
#include "../../kernel/proc_table.h"
#include "context_switch.h"

/*
 * The two window-0 cells platform/zeta-v2/kernel_entry.s defines and the trap
 * fills.  Declared here because this file is the first C code to read them, and
 * a file that must not own the storage still has to be able to name it.
 */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;

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
/* The process map the trap displaced, in _DATA.  The syscall entry writes it
 * itself, after it has installed the kernel map, so this is written from
 * window-0 assembly and by nothing else - which is why it is not static: the
 * entry names it directly.  zeta_kernel_exit() is its only reader. */
zeta_saved_context_t g_saved_user_context;
/* The syscall result, parked across the map restore.  The storage is the
 * window-0 cell _g_syscall_result in syscall_entry.s, and it MUST be there,
 * not in _DATA: this file used to define it as a `static uint16_t`, which put
 * it in _DATA, and _DATA is window 2 ($8000-$BFFF) - under the kernel map that
 * is kernel page 0x22, but under a process map it is the process's own page.
 * dispatch_frame stores the handler's result, then calls restore_user_hook
 * (which installs the calling process's map through zeta_kernel_exit()), and
 * only then reads the cell back to return it to userspace.  With the cell in
 * _DATA the read came out of the process's page: measured over five identical
 * emulator runs, the store wrote 0x0000 while the reload at the dispatcher's
 * return (the `ld de,(_g_syscall_result)` at kernel PC 0x3EDA, after the hook
 * at 0x3ED7) read DE=0x4D98 with the FS server's map installed (banks 20 25 24
 * 23) - the FS server's own _g_fs_tty.send pointer at the same offset in its
 * page.  handle_write's `sys_ipc_srv_copy_from_client(...) != 0` therefore
 * read true, it returned -1 before muzix_tty_write, g_reply was never set, and
 * the shell's write() returned 0: no banner, no `uptime` output, then exit.
 * The read() result that reaches a parked caller never depended on this cell -
 * it rides the resume path through _muzix_resume_ret, window 0, which is why
 * reads and echo kept working while every ordinary-return syscall value was
 * garbage.  Window 0 is the one window every process map keeps as the
 * kernel's, per mm/mproc_model.c and tools/check_window0.rb. */
extern uint16_t g_syscall_result;

/* The current zeta_state_t, as one cell in window 0 (see the storage note in
 * syscall_entry.s).  The trap reads it before the kernel map is installed, so
 * it cannot live in _DATA - _DATA is window 2, and window 2 is the calling
 * process's own text page at that moment.  The storage is in syscall_entry.s;
 * this is the declaration the rest of the kernel uses.  (An earlier version of
 * this note pinned "_DATA starts at 0xA000"; the _DATA base is derived from the
 * linked _CODE extent by tools/kernel_data_base.rb and is not a number to copy
 * by hand.  The window-2 argument does not depend on the value.) */
extern zeta_state_t *g_zeta_state_ptr;

/* args parked in memory for the same register-clobber reason g_syscall_result
 * is: it arrives in HL - sdcccall(1) puts param1 there - and two calls sit
 * between the entry and the handler, one of which is inline assembly that loads
 * HL with an address. The compiler has no way to know that, so a value left in
 * HL across muzix_trace_stack_mark() or zeta_kernel_enter() is simply lost.
 * Which processes survived was not stable between runs, because it came out of
 * the register allocation for this build.  Unlike g_syscall_result this cell is
 * only ever read at trap entry, before the map switch, so its _DATA placement
 * (window 2) is safe - it does not have to survive the restore. */
static muzix_z80_syscall_args_t *g_syscall_args;

/* Published by the kernel layer; see the note on the restore hook below. */
extern uint16_t muzix_switch_pending;
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

void muzix_z80_set_zeta_state(zeta_state_t *state)
{
    g_zeta_state_ptr = state;
}

void muzix_z80_set_zeta_state_ptr(zeta_state_t **ptr)
{
    /* Retained for API compatibility. The shared cell is a single pointer;
     * treating it as a pointer-to-pointer is what left the userspace handoff
     * reading a state of zero, refusing, and spinning forever. */
    (void)ptr;
    /* g_zeta_state_ptr is already the global; no-op for compatibility */
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
    /*
     * CLAIM THIS PROCESS'S RESUME POINT, FIRST THING.
     *
     * The trap has just written two window-0 cells, _muzix_resume_pc and
     * _muzix_resume_sp, while window 1 still held the calling process's own
     * page - the only moment at which the user's return address can be read at
     * all, because after the four bank OUTs window 1 is kernel page 0x21 and
     * that word is the kernel's own stack.
     *
     * Those two cells are GLOBAL.  They are also rewritten on every other
     * switch: muzix_context_switch_to() publishes the NEXT slot's values into
     * them, and it runs whenever the kernel gives the processor away.  So a
     * process that parks is exposed for the whole span between the trap storing
     * its values and muzix_proc_table_park() reading them, and anything that
     * dispatches another process in that window leaves this process holding the
     * other's resume point.
     *
     * The consequence is not a clean failure.  muzix_proc_table_park() stores
     * whatever is in the cells as this slot's resume_pc, muzix_context_switch_to
     * publishes it back, and zeta_resume_process() jumps there with the
     * process's map installed.  The value is a KERNEL stack address, so the
     * process resumes inside window 3 - the kernel's own page, where no code is
     * linked - and executes the pushed register words of the map-switch path as
     * code.  Measured over 200,000 watchpoint hits: PC pinned in that region,
     * SP the shell's own stack, never unwinding, with BC carrying the bank page
     * set 0x20..0x23.
     *
     * Writing it into the slot here closes the window entirely.  The cells are
     * read once, on the first C instruction after the trap, while nothing has
     * had the chance to touch them, and the slot becomes the only record from
     * then on.  The global cells keep their job as the transfer register into
     * the assembly - muzix_context_switch_to() still publishes into them and
     * zeta_resume_process() still jumps from them - but no parked process's
     * resume point depends on them surviving a context switch.
     */
    if (userspace_context->kernel && userspace_context->kernel->loop) {
        muzix_kernel_proc_table_t *table =
            &userspace_context->kernel->loop->system.startup.proc_table;
        int slot = muzix_current_slot(userspace_context);
        if (slot >= 0 && slot < MUZIX_PROC_TABLE_MAX) {
            muzix_proc_table_set_resume(table, slot,
                                        (uint16_t)muzix_resume_pc,
                                        (uint16_t)muzix_resume_sp,
                                        0);
        }
    }
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

    /* The map is already the kernel's: the trap switched it before calling
     * here.  It has to, and not this function - the kernel map comes from
     * state->bank_reg[], which is _DATA, and _DATA is window 2, and window 2 is
     * the calling process's own page until the map has been switched.  Calling
     * zeta_kernel_enter() from here asked the process for the kernel's map.
     *
     * `state` is now unused on this path; it stays in the signature because the
     * trap passes it and the exit path needs its own copy. */
    (void)state;

    if (!syscall_handler) {
        return 0xffffu;
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
     * local.  The return value is the 16-bit result in DE, and the restore hook
     * is a full C call underneath us: SDCC left DE holding whatever that call
     * produced, so a read that had copied one byte correctly still handed the
     * process 35 instead of 1, and getchar() read that as end of file.  This is
     * the same failure as the entry clobbering HL with `ld hl,(user_sp)`, one
     * level up, and the lesson is the same: do not leave anything that matters
     * in a register across a call this build does not get right.
     *
     * The parked cell is _g_syscall_result in syscall_entry.s, and the reason
     * it lives out there rather than here is the second half of the same
     * lesson: parking it in _DATA did not survive the thing it was parked for.
     * _DATA is window 2, restore_user_hook() re-points window 2 at the calling
     * process's page before line 243 reads the cell back, and the value that
     * reached userspace was the process's word at 0xB457 (measured 0x4D98, not
     * 0).  window 0 is the only window the restore does not touch. */
    g_syscall_result = muzix_z80_userspace_handler(g_syscall_args);

    /* Restore the user's map only if we are going back to *this* process.
     *
     * A handler may have asked for a context switch, in which case control is
     * about to leave for a different process and the switch installs that
     * process's map itself. Restoring the user's map first would hand window 3
     * back to the calling process while the kernel still has its switch
     * parameters to read: muzix_resume_pc/sp/ret and muzix_resume_banks live in
     * kernel_entry.s's _CODE, and _g_zeta_state_ptr in syscall_entry.s's _CODE,
     * which is window 0 - the one window every map keeps as the kernel's. The
     * trap's exit path then reads them out of a process page instead of the
     * kernel's, got a zero state pointer, and zeta_resume_process refused - the
     * process waiting to be resumed was the one that hung.
     *
     * They used to sit in _DATA, which is window 2, not window 3 as an earlier
     * version of this note said - _DATA starts at 0xA000. That was the same
     * wrong belief that put the trap's own message and UART helper in _DATA.
     *
     * So the switch has to happen with the kernel map still installed, which
     * is what skipping this does. A refused switch still returns here with
     * switch_pending cleared, so the normal restore is not lost. */
    if (restore_user_hook && !muzix_switch_pending) {
        restore_user_hook(state);
    }
    return g_syscall_result;
}

/* The restore hook's signature.  dispatch_frame() tests restore_user_hook and
 * calls through it, so this is the one place that puts the process's map back.
 * It has to be a wrapper: zeta_kernel_exit() takes the saved context too, and
 * the hook carries only the state. */
void muzix_z80_restore_user_map(zeta_state_t *state)
{
    zeta_kernel_exit(state, &g_saved_user_context);
}

