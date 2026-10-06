#include "kernel_loop.h"
#include "load.h"

#include "../platform/zeta-v2/syscall_runtime.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/context_switch.h"
#include "proc_table.h"
#include "proc_context.h"
#include "../mm/mm_service.h"

#include <string.h>

extern uint16_t muzix_userspace_entry;
extern uint8_t muzix_userspace_banks[4];

extern uint16_t g_bind_slot;
extern uint16_t g_bind_pid;

void muzix_kernel_loop_init(muzix_kernel_loop_t *loop,
                            const process_map_t *kernel_map,
                            uint8_t init_pid,
                            const process_map_t *init_map)
{
    if (!loop) {
        return;
    }

    memset(loop, 0, sizeof(*loop));
    loop->state = MUZIX_KERNEL_LOOP_IDLE;
    loop->tick = 0;
    loop->current_pid = -1;
    muzix_system_entry_init(&loop->system, kernel_map, init_pid, init_map);
}

void muzix_kernel_loop_register(muzix_kernel_loop_t *loop,
                                int slot,
                                uint8_t pid,
                                const muzix_mproc_t *mp,
                                const process_map_t *map,
                                uint8_t q,
                                uint8_t flags)
{
    if (!loop || !mp || !map) {
        return;
    }

    muzix_system_entry_register(&loop->system, slot, pid, mp, map, q, flags);
}


/* Give up the processor without returning.  Never comes back.
 *
 * This is FUZIX's plt_switchout, and its whole trick is that it does not return:
 *
 *     _plt_switchout:                          tricks.s
 *         ld hl, #0
 *         push hl                  ; the value the pending call reports
 *         push ix
 *         push iy
 *         ld (_udata + U_DATA__U_SP), sp   ; SP, and the PC is in the frame
 *         ...
 *         call _getproc                      ; LOOPS if nothing is runnable
 *         push hl
 *         call _switchin                     ; never returns
 *
 * and getproc says of itself, in Kernel/process.c:
 *
 *     "It is actually the scheduler.  If there are none, it loops.  This is the
 *      only time-wasting loop in the system."
 *
 * Those two facts are the whole of how a single-CPU cooperative system stops
 * being busy: a process that has nothing to do is switched out, and if that left
 * nothing runnable the scheduler stops the processor instead of looking for work.
 * The Z80 has no idle state other than HALT, so the loop below is that HALT - and
 * it is in the scheduler, not inside a blocking call, which is where FUZIX puts it
 * and the only place on this board where it is known to work.
 *
 * A caller reaches this from a context it must not return to - the park handler,
 * which has already recorded where to resume the process.  Going back to the caller
 * would return to a process that is no longer in any ready queue, which would run
 * exactly once more and then be stranded.
 */
/* The gap the kernel spent awake, then stop the processor until an interrupt.
 *
 * The measurement is the AWAKE side and not the sleeping side, and that is the
 * whole content of this function's reason to exist.  The first version of this
 * bracketed the halt with muzix_tick_now() and reported the difference, which is
 * wrong in a way worth writing down:
 *
 * A halt is ended BY the tick, so tick_after - tick_before is 1 for every halt
 * however long the halt really was, and the true length is uniform in (0, 1
 * tick) because the processor enters one at a random phase within the tick
 * period.  Recording each halt as a whole tick therefore counts roughly twice the
 * time that actually elapsed, which turns a busy fraction of b into 2b - 1.  For
 * anything under 50% that is negative and clamps to zero, and measured against an
 * independent count of halted cycles the figure read 0.01 where the truth was
 * 0.077.  A tick is not a clock fine enough to measure this in: an idle wakeup
 * costs about 5500 cycles and a tick is 61440.
 *
 * So the awake gap is measured instead, with the free-running counter on CTC
 * channel 3 (muzix_tick_cycles()).  That gap cannot wrap - 8.9 ms of kernel work
 * would be a different fault - so it needs no wrap handling, and it is the number
 * the busy fraction is actually made of.
 *
 * Every idle halt goes through here rather than calling z80_halt() directly: a
 * halt nobody measured is a wake the load figure cannot see, and that is what it
 * used to be blind to.
 *
 * muzix_tick_cycles() reads a port, which is not memory and so is not affected by
 * any map, and this runs from ordinary kernel context rather than from the
 * handler - so it sits on the safe side of the argument in
 * platform/zeta-v2/tick.s, not across it. */
/* Whether g_cycle_mark holds a reading yet.
 *
 * The counter has not been read at boot, so the first difference taken against an
 * unset mark would be a reading measured from zero - an arbitrary number up to 255
 * counts, which is up to 1.1 ms of CPU attributed to nothing in particular.  The
 * first sample is assigned rather than folded, so it would show up in it.  One bit
 * to say "not yet" is cheaper than being wrong in the first number anybody reads.
 */
static uint8_t g_cycle_mark_valid;

/* How long the processor was stopped, then stop it until an interrupt.
 *
 * The SLEEPING side is the one measured, and the reason is the whole content of
 * this function: occupancy is what the machine did NOT spend halted, so that is
 * what has to be subtracted, and the subtraction has to be the length of the halt
 * rather than of the loop around it.
 *
 * The version before this measured the AWAKE gap - the loop between one halt
 * returning and the next halt being entered - and it produced a figure that was
 * confidently, smoothly and completely wrong in the direction that matters.  It
 * counted only the scheduler's own housekeeping, so a machine running a process at
 * full tilt recorded nothing at all: the idle loop is not reached, nothing is
 * reported, and the figure read 0.00 on a machine that was 100% busy.  Measured
 * with `top` running: 0.05 reported against 24.75% actually busy.  A statistic
 * that goes to zero when the machine gets busy is not a statistic about the
 * machine.
 *
 * The awake gap is not measurable by the tick either, and that is not a detail: a
 * halt is ENDED BY THE TICK, so bracketing one with muzix_tick_now() reads 1
 * however long the halt was, and the figure comes out as 2b - 1, which is zero for
 * anything under half.  That was tried and measured at 0.01 against 7.63% true.
 *
 * What works is the free-running counter on CTC channel 3, read either side of the
 * halt.  One count is 256 cycles and the counter is 8 bits, so it can express a
 * halt of up to 65536 cycles - and a halt is at most one tick, 61440 cycles, 240
 * counts.  It fits, with 16 counts to spare.  The one thing that would not fit is
 * a halt LONGER than a tick, which means the tick was missed, which means the
 * machine was already broken; that wraps, and it wraps towards "idle", so the
 * failure is a too-low figure on a machine that needs looking at rather than a
 * wrong-but-plausible one.  It is counted DOWN, so elapsed is mark - now.
 *
 * Every idle halt goes through here rather than calling z80_halt() directly: a
 * halt nobody measured is an interval the figure cannot see.
 *
 * muzix_tick_cycles() reads a port, which is not memory and so is unaffected by
 * any map, and this runs from ordinary kernel context rather than from the
 * handler - so it sits on the safe side of the argument in
 * platform/zeta-v2/tick.s, not across it. */
static void kernel_halt(void)
{
    uint8_t before;
    uint8_t after;

    if (!g_cycle_mark_valid) {
        /* The first halt is not measured - there is no earlier reading to measure
         * it against, and a difference against zero would be an arbitrary number
         * up to 255 counts, which is up to 1.1 ms of CPU attributed to nothing in
         * particular.  The halt itself still happens; only the bracket is skipped,
         * and the halt that ends the boot sequence is followed by the idle loop
         * being measured from then on. */
        (void)muzix_tick_cycles();
        g_cycle_mark_valid = 1u;
        z80_idle_halt();
        return;
    }

    before = muzix_tick_cycles();
    z80_idle_halt();
    after = muzix_tick_cycles();
    /* uint8_t, NOT uint16_t.  The counter is 8 bits and wraps, so the elapsed
     * time is the difference taken modulo 256 - and 1 - 255 across a wrap is -254,
     * which cast to sixteen bits is 65282.  Every wrap would then add 65282
     * counts instead of 2, and since the counter wraps every 65536 cycles (8.9 ms)
     * that is a 255-fold overcount arriving ten times a second: the accumulated
     * "halted" time passed the whole interval, was clamped to it, and the figure
     * read 0.00 on a machine the emulator independently measured at 22% busy.  A
     * statistic pinned at zero by its own overflow is worse than one that is
     * merely approximate, because it looks like an answer.
     *
     * The modulo is not handled here - it falls out of the cast, which is why the
     * width is the whole content of the expression. */
    muzix_load_note_halted((uint8_t)(before - after));
}

void muzix_kernel_switchout(muzix_kernel_loop_t *loop)
{
    muzix_kernel_proc_table_t *table = &loop->system.startup.proc_table;
    int next;

    for (;;) {
        /* should_idle() wakes expired deadlines as it goes, so the wait and the
         * decision are one step and a slot that became runnable while the
         * processor was stopped is seen here. */
        if (muzix_proc_table_should_idle(table)) {
            kernel_halt();
            /* A halt that returns means an interrupt was taken while the processor
             * was stopped.  Usually that is the tick and there is nothing to do but
             * go round again - but it may instead have been the UART, which means a
             * keystroke is sitting in the ring waiting for somebody to run and
             * collect it.
             *
             * The check is here rather than in the handler because of the map
             * argument in platform/zeta-v2/tick.s: the handler may touch windows 0
             * and 3 only, and requeueing a slot needs the ready queues.  So the
             * handler raises a flag in _DATA and this turns it into a running
             * process, from ordinary kernel context, on the next pass - which it is
             * taking anyway.
             *
             * Without this the park deadline is the only thing that ends a wait, and
             * that is two seconds: lib/libc.c's read() parks 240 ticks, so a
             * character typed half a second into a park was not looked at for one and
             * a half seconds.  That is what made a machine that was finally no longer
             * burning the processor still feel like it was busy. */
            if (muzix_uart_rx_pending()) {
                (void)muzix_proc_table_wake_input(table);
            }
            continue;
        }
        next = muzix_proc_table_pick_next(table);
        if (next >= 0) {
            break;
        }
        /* should_idle() said there was work and pick_next() found none.  Those
         * two cannot disagree while the table is sound: should_idle() returns 0
         * either because a deadline expired - and wake_expired() requeues through
         * the same muzix_proc_table_ready() that pick_next() reads - or because a
         * queue is non-empty, which pick_next() then pops from.
         *
         * So reaching here means a slot is unreachable rather than merely
         * unready, and the honest response is to stop burning the processor, not
         * to spin.  That was the second half of the reported saturation: with
         * parking broken by the type confusion at kernel/syscalls.c, the shell
         * was lost exactly this way, and the loop retried forever at full tilt -
         * no HALT, no tick, nothing charged to anyone, and a machine that looked
         * busy while being useless.
         *
         * HALT rather than give up, for two reasons.  It keeps the machine
         * responsive instead of dead: the tick returns 120 times a second, each
         * pass re-asks, and a slot that becomes reachable is dispatched with no
         * other delay.  And it costs nothing - a slot that is lost stays lost,
         * but it is now a machine at 0% that a lost-process bug can hide in
         * rather than a machine at 100% that hides everything else.
         *
         * There is no sleep here to bound: HALT is bounded by the interrupt, and
         * a wait that cannot end is not a wait. */
        kernel_halt();
        if (muzix_uart_rx_pending()) {
            (void)muzix_proc_table_wake_input(table);
        }
    }
    loop->state = MUZIX_KERNEL_LOOP_RUNNING;
    muzix_context_switch_to(table, next);     /* never returns */
}

void muzix_kernel_loop_run(muzix_kernel_loop_t *loop)
{
    if (!loop) {
        return;
    }

    /* Nothing to dispatch: wake whoever is due, and if there is still nothing,
     * stop the processor until the tick brings it back.
     *
     * `halt` is the right instruction for this on a Z80 and not a refinement.  It
     * issues no bus cycles at all - no fetch, no read, no write - so the CPU is
     * genuinely idle rather than spinning through a loop, which is the whole
     * difference between a machine at rest and one burning power to look for work
     * that is not there.  Any spin-wait alternative here would keep the load
     * figure at 1.00 and describe nothing.
     *
     * It wakes on the 120 Hz CTC tick or a UART receive interrupt. Both are
     * vectored through the CTC in IM2; the UART interrupt sets a pending flag
     * which this loop turns into a process wake in ordinary kernel context.
     * Neither interrupt preempts a running userspace process.
     *
     * muzix_proc_table_should_idle() refuses to say yes unless something is parked
     * AND nothing is runnable, so the pre-boot state and the after-exit state both
     * return instead of stopping.  See its comment for why that distinction is the
     * difference between idling and hanging.
     *
     * This runs with the kernel map installed - it is reached from kernel_main's
     * loop, never from the interrupt handler, which may address only windows 0 and
     * 3 while this module is entirely inside window 1. */
    if (muzix_proc_table_should_idle(&loop->system.startup.proc_table)) {
        kernel_halt();
        if (muzix_uart_rx_pending()) {
            (void)muzix_proc_table_wake_input(&loop->system.startup.proc_table);
        }
    } else if (!muzix_proc_table_any_parked(&loop->system.startup.proc_table)) {
        /* should_idle() said there was work, and nothing is parked - so nothing can
         * become runnable either, because a deadline needs a park to expire.  That is
         * the state the machine is in once the last process has exited, and it is a
         * spin unless it is treated as the idleness it is: halt, and the tick
         * returns 120 times a second to ask again.
         *
         * Same reasoning, and the same cost, as the halt in
         * muzix_kernel_switchout() for the same state.  Both existed because
         * should_idle() is a scheduler's question - "is there work, or may I rest"
         * - and this is a different one, which is why it needed a second test
         * rather than a change to the first. */
        kernel_halt();
        if (muzix_uart_rx_pending()) {
            (void)muzix_proc_table_wake_input(&loop->system.startup.proc_table);
        }
    }

    switch (loop->state) {
    case MUZIX_KERNEL_LOOP_IDLE:
        muzix_system_entry_start(&loop->system);
        loop->current_pid = muzix_system_entry_current_pid(&loop->system);
        if (loop->current_pid >= 0 &&
            loop->system.startup.proc_table.current >= 0) {
            muzix_proc_slot_t *current_slot =
                &loop->system.startup.proc_table.slots[
                    loop->system.startup.proc_table.current];
            uint16_t ep = current_slot->entry_point;
            if (current_slot->entry_point != 0) {
;
                loop->state = MUZIX_KERNEL_LOOP_RUNNING;
                muzix_userspace_entry = current_slot->entry_point;
                muzix_userspace_banks[0] = current_slot->map.pages[0];
                muzix_userspace_banks[1] = current_slot->map.pages[1];
                muzix_userspace_banks[2] = current_slot->map.pages[2];
                muzix_userspace_banks[3] = current_slot->map.pages[3];
                muzix_zeta_enter_userspace();
                loop->state = MUZIX_KERNEL_LOOP_IDLE;
                loop->current_pid = -1;
                return;
            }
        }
        if (loop->current_pid < 0) {
            loop->state = MUZIX_KERNEL_LOOP_IDLE;
        } else {
            loop->state = MUZIX_KERNEL_LOOP_RUNNING;
        }
        break;
    case MUZIX_KERNEL_LOOP_RUNNING:
        loop->tick++;
        if (loop->system.startup.proc_table.current >= 0 &&
            muzix_proc_table_deliver_pending(
                &loop->system.startup.proc_table,
                loop->system.startup.proc_table.current) != 0) {
            muzix_system_entry_switch_to_kernel(&loop->system);
            loop->current_pid = -1;
            loop->state = MUZIX_KERNEL_LOOP_IDLE;
        }
        /* No user_time++ here, and its absence is deliberate.  It used to sit
         * in this branch, and what it counted was how many times the kernel had
         * entered userspace - not time.  It read zero for a process that was
         * never preempted, which on a system with no preemption is every
         * process that ever ran, so the column looked like a broken counter
         * rather than a truthful one.  The 120 Hz CTC tick makes the real
         * quantity measurable, and the two charges that read it are at the
         * ends of the syscall: muzix_proc_table_charge(), called from
         * muzix_handle_userspace_syscall().  Charging here as well would
         * double-count - the interval since the last charge was the kernel's,
         * and the syscall exit already books it as system time. */
        loop->current_pid = muzix_system_entry_current_pid(&loop->system);
        break;
    case MUZIX_KERNEL_LOOP_SHUTDOWN:
        break;
    default:
        break;
    }
}

/* Yield to the next ready process.
 *
 * A process map installs the process's text into window 2 and its stack into
 * window 1, keeping only window 0 (page 0x20) and window 3 (page 0x23) as
 * kernel. So across a map switch, kernel code survives ONLY in windows 0 and 3.
 *
 * The kernel is 0x0098..0xEA24 - it spans all four windows, and this function
 * is at 0x50B5, window 1. So this function cannot install a map and then keep
 * running: the moment the process map is installed, the rest of this function
 * and its caller are a banked-out page. That is why the switch used to go
 * through muzix_mm_context_switch() and land in muzix_system_service_context_switch(),
 * which sits in window 1 too and refused - the switch silently did nothing.
 *
 * The fix is not to shrink the kernel into windows 0 and 3. It is to hand the
 * handoff to code that lives in window 0 and never comes back:
 * muzix_zeta_enter_userspace() (0x00B5) is assembly, deliberately in window 0,
 * and calls zeta_enter_process() (0x166F), which installs the map and jumps to
 * the process without returning. The call chain through this function is simply
 * abandoned at the jump - which is fine, because it is not supposed to
 * continue. This is the same mechanism the IDLE path in muzix_kernel_loop_run()
 * already uses for the very first entry into userspace.
 *
 * muzix_userspace_entry and muzix_userspace_banks[] are in _DATA (window 3) and
 * must be written BEFORE the handoff: after the map change window 3 is still
 * kernel only because pages[3] is 0x23, and the entry point is stashed into a
 * window-0 cell inside the handoff for the same reason. */
void muzix_kernel_loop_yield(muzix_kernel_loop_t *loop)
{
    if (!loop || loop->state != MUZIX_KERNEL_LOOP_RUNNING) {
        return;
    }

    int prev_slot = loop->system.startup.proc_table.current;
    int next_slot = muzix_proc_table_yield(&loop->system.startup.proc_table);

    if (prev_slot >= 0 && next_slot >= 0 && prev_slot != next_slot) {
        muzix_proc_slot_t *prev = &loop->system.startup.proc_table.slots[prev_slot];
        muzix_proc_slot_t *next = &loop->system.startup.proc_table.slots[next_slot];

        /* Record what the outgoing process must be restored to: its own map,
         * since the syscall entry installed the kernel map on the way in. */
        prev->saved_context = prev->map;

        /* Hand off to window 0. Never returns, so nothing after this point in
         * the kernel runs until the process next traps. See the note above. */
        if (!next->active || next->entry_point == 0) {
            /* Nothing runnable to hand to. Stay in the kernel rather than jump
             * to address 0 and die in the boot stub. */
            return;
        }

        loop->state = MUZIX_KERNEL_LOOP_RUNNING;
        muzix_userspace_entry = next->entry_point;
        muzix_userspace_banks[0] = next->map.pages[0];
        muzix_userspace_banks[1] = next->map.pages[1];
        muzix_userspace_banks[2] = next->map.pages[2];
        muzix_userspace_banks[3] = next->map.pages[3];
        muzix_zeta_enter_userspace();
    }

    loop->tick++;
    /* The user_time++ that stood here counted entries into userspace, not time.
     * See the same note in muzix_kernel_loop_run() above; the real counters are
     * read from the CTC tick at the ends of the syscall. */
    loop->current_pid = muzix_system_entry_current_pid(&loop->system);
}

void muzix_kernel_loop_switch_to_kernel(muzix_kernel_loop_t *loop)
{
    if (!loop) {
        return;
    }

    muzix_system_entry_switch_to_kernel(&loop->system);
    loop->current_pid = -1;
    loop->state = MUZIX_KERNEL_LOOP_IDLE;
}

int muzix_kernel_loop_current_pid(const muzix_kernel_loop_t *loop)
{
    if (!loop) {
        return -1;
    }

    return loop->current_pid;
}

void muzix_kernel_loop_bind_syscalls(
    muzix_kernel_loop_t *loop,
    struct muzix_zeta_syscall_runtime *runtime,
    struct muzix_fs_service *fs)
{
    if (!loop || !runtime) {
        return;
    }

    muzix_zeta_syscall_runtime_init(runtime, loop, fs);
}
