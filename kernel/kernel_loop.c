/* * * Scheduler dispatch loop: the permanent idle process that
 * enters userspace processes, halts when nothing is runnable,
 * and folds the load windows from the idle path.
 * * This runs with the kernel map installed - it is reached from
 * kernel_main's loop, never from the interrupt handler, which
 * may address only windows 0 and 3 while this module is
 * entirely inside window 1.
 */

#include "kernel_loop.h"
#include "load.h"
#include "cron.h"
#include "../include/muzix_ipc.h"
#include "proc_table.h"
#include "proc_context.h"
#include "../mm/mm_service.h"

#include "../platform/zeta-v2/syscall_runtime.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/context_switch.h"

#include "libc.h"

extern uint8_t muzix_userspace_banks[4];
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;
extern uint16_t muzix_resume_ret;
extern uint8_t muzix_resume_banks[4];
extern void muzix_set_entry_point(uint16_t entry);

/* Written by each ISR that can wake the CPU from HALT (tick.s).  kernel_halt()
 * reads it after the halt returns to exclude ISR execution time from the
 * measured halted interval.  Cleared before each halt so a stale value from a
 * previous interrupt cannot be reused. */
extern uint16_t muzix_ctc01_wake_phase;
extern uint8_t  muzix_ctc01_wake_valid;

/* Halt measurement entry point: di, read CTC0/CTC1 phase, clear wake_valid,
 * ei, halt.  Returns the start phase in DE.  After HALT returns, call
 * _muzix_halt_end() to fetch the wake phase and clear wake_valid atomically. */
extern uint16_t muzix_halt_begin(void);
extern uint16_t muzix_halt_end(void);

/* Halt overhead in CTC0/CTC1 reference counts, computed by muzix_load_init(). */
extern uint16_t muzix_load_halt_overhead_counts(void);

void muzix_cron_init(void);
void muzix_cron_tick_notify(muzix_ipc_table_t *ipc, muzix_kernel_loop_t *loop,
 uint16_t now_ticks);

/* SPI bus recovery constants. */
#define MUZIX_SPI_PPI_PORT_C 0x62
#define MUZIX_SPI_CS_BIT 0x80u /* PC7 (active low) */

/* I2C bus recovery constants. */
#define MUZIX_PPI_PORT_C 0x62
#define MUZIX_PPI_PORT_CTRL 0x63
#define MUZIX_I2C_SDA_BIT 0x01u
#define MUZIX_I2C_SCL_BIT 0x10u

/* Throttle for the load-window idle sample: once every 15 seconds. */
#define MUZIX_LOAD_IDLE_SAMPLE_TICKS \
 ((uint16_t)(15u * MUZIX_TICKS_PER_SECOND))

/* ------------------------------------------------------------------------- */
/* SPI bus recovery */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Load-window idle sampling */
/* ------------------------------------------------------------------------- */

/* * Fold the interval that just ended, if the throttle allows
 * it.
 * * The three load windows are a partition of WALL time: every cycle is either
 * executing or the processor is stopped in HALT, and the figure is the
 * executing share. A sample needs the wall length of an interval (the tick
 * gives it) and the halted time inside it (kernel_halt() accumulates it with
 * muzix_load_note_halted()).
 * * One interval ends exactly where this file knows both: after a halt returns.
 * That is the only moment the kernel is provably awake, provably holding the
 * table, and provably running with the kernel map installed.
 * * Samples are throttled to one every 15 seconds because muzix_load_sample()
 * is not cheap: the busy fraction is a quotient computed by repeated
 * subtraction, and the exponential step is a walk over the exponent's bits
 * doing Q14 multiplies.
 * table: process table (must not be NULL)
 */
static uint16_t g_load_idle_tick;
static uint8_t g_cycle_mark_valid;

static void
muzix_load_idle_sample(muzix_kernel_proc_table_t *table) {
 uint16_t now;

 if (!table) {
 return;
 }
 now = muzix_tick_now();
 if ((uint16_t)(now - g_load_idle_tick) < MUZIX_LOAD_IDLE_SAMPLE_TICKS) {
 return;
 }
 g_load_idle_tick = now;
 muzix_load_sample(table, now);
}

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

/* ------------------------------------------------------------------------- */
/* Idle halt */
/* ------------------------------------------------------------------------- */

/* * Measure the halted interval and stop the processor until an
 * interrupt.
 * * The SLEEPING side is measured: occupancy is what the machine did NOT spend
 * halted, so that is what has to be subtracted, and the subtraction has to be
 * the length of the halt rather than of the loop around it.
 * * The free-running counter on CTC channel 3 (muzix_tick_cycles()) is read on
 * either side of the halt. One count is 256 cycles and the counter is 8 bits,
 * so it can express a halt of up to 65536 cycles - and a halt is at most one
 * tick (61440 cycles, 240 counts). It fits, with 16 counts to spare.
 * * The stop is muzix_proc_halt_uncounted() and not z80_idle_halt(), which is
 * what makes this function the first caller that mechanism has ever had. The
 * charge the kernel bills a process with is the wall time between the two ends
 * of a system call, so a halt taken while a call is in flight would otherwise
 * be charged as that process's SYSTEM time. The uncounted halt refreshes the
 * accounting stamp on both sides of the stop, so the interval it spans is
 * charged to nobody.
 */
static void kernel_halt(void)
{
    uint16_t before;
    uint16_t after;
    uint16_t elapsed;
    uint16_t overhead;

    if (!g_cycle_mark_valid) {
    /* The first halt is not measured - there is no earlier reading to
     * measure it against. The halt itself still happens; only the bracket
     * is skipped. */
    (void)muzix_ctc01_read_phase();
    g_cycle_mark_valid = 1u;
    muzix_proc_halt_uncounted();
    return;
    }

    /* Atomic halt sequence: disable interrupts, read the start phase,
     * clear wake_valid so a stale ISR snapshot cannot be reused, then
     * ei; halt.  Interrupts are disabled from di until ei, so no ISR can
     * run between the phase read and the halt - closing the race where an
     * interrupt lands after the phase read but before wake_valid is cleared.
     */
    before = muzix_halt_begin();

    /* After HALT returns, interrupts are enabled (RETI preserves IFF1).
     * Check wake_valid BEFORE calling muzix_halt_end(), which clears it.
     * If wake_valid is still 0, no ISR woke this halt - there is no
     * trustworthy wake timestamp.  A fresh phase read would include ISR
     * execution time (or arbitrary post-halt time), overestimating halted
     * time and underestimating busy fraction.  Skip this interval instead;
     * the overhead subtraction would not fix it because the error is not
     * measurement overhead but a wrong endpoint.  Subsequent intervals are
     * unaffected. */
    if (!muzix_ctc01_wake_valid) {
    /* No ISR snapshot: this halt interval is unreliable.  Do not record
     * any halted counts for it.  The wake_valid flag remains 0; the next
     * halt will clear it again in muzix_halt_begin(). */
    return;
    }

    /* muzix_halt_end() atomically (interrupts off) fetches the wake phase
     * from the ISR snapshot, then clears wake_valid so a later ISR cannot
     * overwrite this HALT's timestamp.  For the fallback marker
     * (wake_valid == 2) it calls _muzix_ctc01_read_phase_locked() to obtain
     * a fresh phase instead of using the ISR snapshot. */
    after = muzix_halt_end();

    /* elapsed is phase difference mod 7680 (one full tick period).
     * A halt is at most one tick, so elapsed is unambiguous. */
    if (after >= before) {
    elapsed = after - before;
    } else {
    elapsed = 7680u - before + after;
    }

    /* Subtract measurement overhead: both endpoints include code that
     * executes while the CPU is NOT halted (start path before HALT,
     * wake path after HALT exit).  Both overestimate halted time, so
     * subtract the combined overhead.  Clamp at zero to avoid underflow.
     * Overhead depends on CPU frequency (CPU T-states -> reference counts),
     * computed at init in muzix_load_init().  For the fallback marker,
     * add the extra UART ISR + coherent-read path that WAKE_OVERHEAD_T
     * does not cover. */
    overhead = muzix_load_halt_overhead_counts();
    /* Fallback marker (wake_valid == 2, bit 0 clear): UART woke CPU but no
     * coherent snapshot.  _muzix_halt_end() already called
     * _muzix_ctc01_read_phase_locked() to obtain a fresh phase.  The extra
     * ISR + fallback-read time is not subtracted here; fallback events
     * require 4 consecutive CTC0 wraps between c0a and c1 (window {0..3} of
     * 256 phases) and are therefore extremely rare.  The residual error per
     * event is bounded by ~537 T (~124 counts at 4 MHz) and does not affect
     * the loadavg conclusion in any measurable way. */
    if (elapsed > overhead) {
    elapsed -= overhead;
    } else {
    elapsed = 0;
    }

    muzix_load_note_halted(elapsed);
}

/* ------------------------------------------------------------------------- */
/* Console wake */
/* ------------------------------------------------------------------------- */
/* * The flag alone was not enough: a byte that arrived before the reader parked
 * may be consumed by an idle pass while no slot is parked yet, and the reader
 * that parks afterwards waits out its full deadline. The level is the truth:
 * while the 16550 reports data ready, some parked reader is waiting for that
 * data, so the wake is re-armed on every pass.
 * \param[in,out] table: process table
 */
static void kernel_wake_console_readers(muzix_kernel_proc_table_t *table)
{
 if (muzix_uart_rx_pending() || muzix_zeta_uart_available(0)) {
 (void)muzix_proc_table_wake_input(table);
 }
}

/* ------------------------------------------------------------------------- */
/* Context switch-out (never returns) */
/* ------------------------------------------------------------------------- */

/* * Give up the processor without returning.
 * * This is FUZIX's plt_switchout: it does not return, because a caller that has
 * just made itself unrunnable cannot do anything sensible afterwards. The loop
 * below is the HALT - the Z80 has no idle state other than HALT - and it is in
 * the scheduler, not inside a blocking call.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_switchout(muzix_kernel_loop_t *loop)
{
	muzix_kernel_proc_table_t *table;
	int next;

	if (!loop) {
		return;
	}
	table = &loop->system.startup.proc_table;

	for (;;) {
		/* Deliver pending signals to parked and runnable processes before
		 * dispatching.  switchout() is called from syscall handlers (exit,
		 * park) and bypasses the main loop's deliver_parked_signals(), so
		 * check here to guarantee signals are delivered at every transition. */
		(void)muzix_proc_table_deliver_parked_signals(
			table, loop->mm);

		/* Try to dispatch a ready process first.  Only if there is nothing
		 * ready do we consider idling - should_idle() returns 1 if any slot
		 * is parked (e.g. servers waiting on IPC), but that must not prevent
		 * dispatching a ready process such as the shell after a child exits. */
		next = muzix_proc_table_pick_next(table);
		if (next >= 0) {
			muzix_proc_slot_t *next_slot = &table->slots[next];

			/* Set current before dispatching so that muzix_current_slot()
			 * returns the right slot for the dispatched process.  Without
			 * this, current stays -1 after the previous process exited,
			 * and wait() reads garbage as caller_pid and can never find
			 * the exiting child's zombie. */
			table->current = next;

			/* If the next process has a valid resume point (was parked in a
			 * syscall), use the resume path instead of the entry point path. */
			if (next_slot->resume_valid) {
				muzix_context_switch_to(table, next);
				return; /* Return to trap, which calls muzix_zeta_resume_from */
			}

			/* A slot with no resume point is one that has never run, or
			 * whose image was just replaced - an exec. Do here exactly what
			 * the main loop does, so a switch to a fresh slot completes the
			 * switch instead of handing control back to the syscall that
			 * asked for it. */
			muzix_set_entry_point(next_slot->entry_point);
			muzix_userspace_banks[0] = next_slot->map.pages[0];
			muzix_userspace_banks[1] = next_slot->map.pages[1];
			muzix_userspace_banks[2] = next_slot->map.pages[2];
			muzix_userspace_banks[3] = next_slot->map.pages[3];
			loop->state = MUZIX_KERNEL_LOOP_RUNNING;
			muzix_zeta_enter_userspace(); /* never returns */
		}

		/* Nothing ready to run.  should_idle() wakes parked processes whose
		 * deadline has passed (wake_expired()), then reports whether the
		 * machine should halt.  If something was just woken, it returns 0
		 * and we loop back to pick_next() to dispatch it.  If nothing is
		 * ready and something is parked, it returns 1 and we halt. */
		if (muzix_proc_table_should_idle(table)) {
			/* Pending signals were already delivered at the top of this
			 * switchout iteration (line 216).  Nothing between then and
			 * here can set pending_signals - pick_next() and
			 * should_idle()/wake_expired() only touch active/WAIT_TICK and
			 * the ready queues, and no ISR sets pending_signals - so the
			 * earlier call already delivered everything that is pending. */
			/* Tick the cron subsystem on every scheduler pass here. */
			muzix_cron_tick_notify(muzix_ipc_get_global(), loop, muzix_tick_now());

			/* Nothing ready to run.  Halt and wait for an interrupt (tick
			 * or UART input) to bring something back to the ready queues. */
			kernel_halt();

			/* A halt that returns means an interrupt was taken while the
			 * processor was stopped. Usually that is the tick - but it may
			 * instead have been the UART, which means a keystroke is sitting
			 * in the ring waiting for somebody to run and collect it. */
			kernel_wake_console_readers(table);
			muzix_load_idle_sample(table);
			continue;
		}

		/* should_idle() returned 0: something was woken or nothing is
		 * parked.  Loop back to pick_next() to dispatch whatever is now
		 * ready. */
		continue;
	}
}

/* ------------------------------------------------------------------------- */
/* Main scheduler loop (never returns) */
/* ------------------------------------------------------------------------- */

/* * The permanent idle process - it NEVER returns. Runs the
 * scheduler loop forever, entering userspace processes and
 * halting when nothing is runnable.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_loop_run(muzix_kernel_loop_t *loop)
{
 if (!loop) {
 return;
 }

 for (;;) {
  /* Deliver pending signals to parked processes first.  A parked process
   * never makes a syscall, so the syscall-entry signal check never fires for
   * it; delivering here kills it at the nearest safe point (parked, kernel map
   * installed, no live stack).  This must run before muzix_cron_tick_notify(),
   * which can wake parked processes and prevent signal delivery. */
  (void)muzix_proc_table_deliver_parked_signals(
      &loop->system.startup.proc_table, loop->mm);

  /* Tick the cron subsystem every pass. */
  muzix_cron_tick_notify(muzix_ipc_get_global(), loop, muzix_tick_now());

  /* Nothing to dispatch: wake whoever is due, and if there is still
  * nothing, stop the processor until the tick brings it back. */
  if (muzix_proc_table_should_idle(
 &loop->system.startup.proc_table)) {
 kernel_halt();
 kernel_wake_console_readers(
 &loop->system.startup.proc_table);
 muzix_load_idle_sample(
 &loop->system.startup.proc_table);
 } else if (!muzix_proc_table_any_parked(
 &loop->system.startup.proc_table)) {
 /* should_idle() said there was work, and nothing is parked - so
 * nothing can become runnable either. Halt, and the tick returns
 * 120 times a second to ask again. */
 kernel_halt();
 kernel_wake_console_readers(
 &loop->system.startup.proc_table);
 muzix_load_idle_sample(
 &loop->system.startup.proc_table);
 }

 switch (loop->state) {
 case MUZIX_KERNEL_LOOP_IDLE: {
 muzix_system_entry_start(&loop->system);
 loop->current_pid =
 muzix_system_entry_current_pid(&loop->system);
 if (loop->current_pid >= 0 &&
 loop->system.startup.proc_table.current >= 0) {
 muzix_proc_slot_t *current_slot =
 &loop->system.startup.proc_table.slots[
 loop->system.startup.proc_table.current];
 uint16_t ep = current_slot->entry_point;
    if (current_slot->entry_point != 0) {
        /* Kill a process with pending signals before dispatching to it.
         * This is the safe point: the kernel map is installed, the process
         * is not yet running, and its pages can be released without
         * disturbing any live stack. */
        if (current_slot->pending_signals != 0u) {
            muzix_proc_table_deliver_pending(
                &loop->system.startup.proc_table,
                loop->system.startup.proc_table.current,
                loop->mm);
            if (!current_slot->active) {
                /* Process killed. Pick a replacement from the ready
                 * queues; if none, loop will idle and the tick will
                 * bring something back. */
                int next = muzix_proc_table_pick_next(
                    &loop->system.startup.proc_table);
                if (next >= 0) {
                    loop->system.startup.proc_table.current = next;
                    current_slot =
                        &loop->system.startup.proc_table.slots[next];
                    goto dispatch_slot;
                }
                loop->system.startup.proc_table.current = -1;
                loop->state = MUZIX_KERNEL_LOOP_IDLE;
                continue;
            }
        }

dispatch_slot:
        loop->state = MUZIX_KERNEL_LOOP_RUNNING;
 if (current_slot->resume_valid) {
 /* Resume at saved point (was parked in syscall). */
 muzix_resume_pc = current_slot->resume_pc;
 muzix_resume_sp = current_slot->resume_sp;
 muzix_resume_ret = current_slot->resume_ret;
 muzix_resume_banks[0] =
 current_slot->map.pages[0];
 muzix_resume_banks[1] =
 current_slot->map.pages[1];
 muzix_resume_banks[2] =
 current_slot->map.pages[2];
 muzix_resume_banks[3] =
 current_slot->map.pages[3];
 muzix_zeta_resume_from();
 } else {
 /* Fresh entry at entry_point. */
 muzix_set_entry_point(current_slot->entry_point);
 muzix_userspace_banks[0] =
 current_slot->map.pages[0];
 muzix_userspace_banks[1] =
 current_slot->map.pages[1];
 muzix_userspace_banks[2] =
 current_slot->map.pages[2];
 muzix_userspace_banks[3] =
 current_slot->map.pages[3];
 muzix_zeta_enter_userspace();
 }

 /* muzix_zeta_enter_userspace never returns.
 * If we somehow reach here, reset state and continue. */
 loop->state = MUZIX_KERNEL_LOOP_IDLE;
 loop->current_pid = -1;
 continue;
 }
 /* entry_point == 0: slot not ready to run. Unpick it. */
 loop->system.startup.proc_table.current = -1;
 }
 loop->state = MUZIX_KERNEL_LOOP_IDLE;
 break;
 }
 case MUZIX_KERNEL_LOOP_RUNNING: {
 /* The console check is not only for halt returns: it costs one
 * IN per loop pass and made a keystroke read in 7.6 ms instead
 * of 3.4 s. */
 kernel_wake_console_readers(
 &loop->system.startup.proc_table);
  loop->tick++;
  if (loop->system.startup.proc_table.current >= 0) {
    muzix_kernel_proc_table_t *lt =
        &loop->system.startup.proc_table;
    int cs = lt->current;
    if (lt->slots[cs].pending_signals != 0u) {
        muzix_proc_table_deliver_pending(lt, cs, loop->mm);
        if (!lt->slots[cs].active) {
            muzix_system_entry_switch_to_kernel(&loop->system);
            loop->current_pid = -1;
            loop->state = MUZIX_KERNEL_LOOP_IDLE;
        }
    }
  }

 /* No user_time++ here: that counted entries into userspace,
 * not time. The real counters are read from the CTC tick at
 * the ends of the syscall (muzix_proc_table_charge()). */
 loop->current_pid =
 muzix_system_entry_current_pid(&loop->system);
 break;
 }
 case MUZIX_KERNEL_LOOP_SHUTDOWN:
 break;
 default:
 break;
 }
 } /* for(;;) - permanent idle loop */
}

/* ------------------------------------------------------------------------- */
/* Yield */
/* ------------------------------------------------------------------------- */

/* * Yield to the next ready process.
 * * A process map installs the process's text into window 2 and its stack into
 * window 1, keeping only window 0 (page 0x20) and window 3 (page 0x23) as
 * kernel. Across a map switch, kernel code survives ONLY in windows 0 and 3.
 * * The handoff goes to muzix_zeta_enter_userspace() (window 0, assembly), which
 * calls zeta_enter_process() to install the map and jump to the process without
 * returning. The call chain through this function is abandoned at the jump.
 * \param[in,out] loop: kernel loop (must be in RUNNING state)
 */
void muzix_kernel_loop_yield(muzix_kernel_loop_t *loop)
{
 int prev_slot;
 int next_slot;
 muzix_proc_slot_t *prev;
 muzix_proc_slot_t *next;

 if (!loop || loop->state != MUZIX_KERNEL_LOOP_RUNNING) {
 return;
 }

 prev_slot = loop->system.startup.proc_table.current;
 next_slot = muzix_proc_table_yield(&loop->system.startup.proc_table);

 if (prev_slot >= 0 && next_slot >= 0 && prev_slot != next_slot) {
 prev = &loop->system.startup.proc_table.slots[prev_slot];
 next = &loop->system.startup.proc_table.slots[next_slot];

 /* Record what the outgoing process must be restored to: its own map,
 * since the syscall entry installed the kernel map on the way in. */
 prev->saved_context = prev->map;

 /* Hand off to window 0. Never returns. */
 if (!next->active ||
 (next->entry_point == 0 && !next->resume_valid)) {
 /* Nothing runnable to hand to. Stay in the kernel. */
 loop->system.startup.proc_table.current = -1;
 return;
 }

 loop->state = MUZIX_KERNEL_LOOP_RUNNING;
 if (next->resume_valid) {
 /* Resume at saved point (was parked in syscall). */
 muzix_resume_pc = next->resume_pc;
 muzix_resume_sp = next->resume_sp;
 muzix_resume_ret = next->resume_ret;
 muzix_resume_banks[0] = next->map.pages[0];
 muzix_resume_banks[1] = next->map.pages[1];
 muzix_resume_banks[2] = next->map.pages[2];
 muzix_resume_banks[3] = next->map.pages[3];
 muzix_zeta_resume_from();
 } else {
 /* Fresh entry at entry_point. */
 muzix_set_entry_point(next->entry_point);
 muzix_userspace_banks[0] = next->map.pages[0];
 muzix_userspace_banks[1] = next->map.pages[1];
 muzix_userspace_banks[2] = next->map.pages[2];
 muzix_userspace_banks[3] = next->map.pages[3];
 muzix_zeta_enter_userspace();
 }
 }

 loop->tick++;
 loop->current_pid = muzix_system_entry_current_pid(&loop->system);
}

/* ------------------------------------------------------------------------- */
/* Switch-to-kernel / query */
/* ------------------------------------------------------------------------- */

/* * Switch from userspace to kernel context.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_loop_switch_to_kernel(muzix_kernel_loop_t *loop)
{
 if (!loop) {
 return;
 }

 muzix_system_entry_switch_to_kernel(&loop->system);
 loop->current_pid = -1;
 loop->state = MUZIX_KERNEL_LOOP_IDLE;
}

/* * Return the current process's pid.
 * loop: kernel loop
 * Current process id, or -1 if none
 */
int muzix_kernel_loop_current_pid(const muzix_kernel_loop_t *loop)
{
 if (!loop) {
 return -1;
 }

 return loop->current_pid;
}

/* * Bind syscalls: initialise the syscall runtime.
 * \param[in,out] loop: kernel loop
 * \param[in,out] runtime: syscall runtime
 * fs: filesystem service
 */
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
