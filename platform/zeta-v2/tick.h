#ifndef MUZIX_ZETA_TICK_H
#define MUZIX_ZETA_TICK_H

#include <stdint.h>

#include "../../lib/proc_info.h"

/* The CTC tick, and nothing else.
 *
 * There is no preemption here and none is planned in this header.  The tick
 * counts; a deadline is the only thing anything is allowed to do with it.  A
 * scheduler switch out of the interrupt handler would need the handler to know
 * which map is installed, and this handler deliberately does not - see the
 * safety argument in platform/zeta-v2/tick.s, which is the whole reason the
 * thing is safe, and which a scheduler switch would invalidate.
 */

/* 120 ticks per second, exactly.  The single definition is
 * MUZIX_PROC_TICK_HZ in lib/proc_info.h, and this is the name the kernel uses;
 * the two are the same number by construction rather than by agreement, which
 * is why platform/zeta-v2/test_tick.c can check the assembler's time constant
 * against it and mean something.
 *
 * Derived in platform/zeta-v2/tick.s from the board's UART_CLK/2 -> CTC0
 * /256 -> CTC1 /30 chain: 921600 / (256 * 30) = 120. One second is 120 ticks, so
 * seconds are ticks / 120 and a millisecond is not an integer number of them
 * (1.2 ticks), which is why the call sites below express their budgets in
 * seconds or in whole ticks and never in milliseconds. */
#define MUZIX_TICKS_PER_SECOND MUZIX_PROC_TICK_HZ

/* CTC channel 2: interrupt enabled, rising-edge counter, time constant next. */
#define MUZIX_CTC_UART_IRQ_CONTROL 0xD7u

/* Program the CTC, install the interrupt vectors, enter IM2 and enable
 * interrupts.
 *
 * Interrupts must already be disabled when this is called, which they are: the
 * kernel has run with `di` since platform/zeta-v2/kernel_entry.s.  The routine
 * re-asserts that and leaves interrupts enabled, so the caller's next
 * instruction is already interruptible.  There is deliberately no matching
 * "disable" routine: disabling the tick would silently break every deadline in
 * the system by turning a bounded wait into the unbounded one it replaced.
 *
 * IM2, not IM1: the board wires the CTC as a vectored interrupt controller and
 * says so, the tick and the UART each need their own vector to be told apart
 * without re-testing every source inside one handler, and FUZIX's port for this
 * board's family does the same.  An IM1 JP to the tick handler is still planted
 * at 0x0038 as a floor under the mode the boot stub leaves behind; the reasoning
 * is in platform/zeta-v2/tick.s. */
void muzix_tick_init(void);
/* Measure CTC3 counts per 120 Hz tick over 32 ticks at startup. Returns 0 if
 * the timer/tick measurement fails or exceeds 8-bit range; kernel HALT deltas
 * are currently modulo-256. */
uint16_t muzix_tick_calibrate_ctc3(void);
/* Set by muzix_tick_calibrate_ctc3(): 0=success, 1=tick timeout,
 * 2=CTC3 did not advance, 3=measured rate exceeds the 8-bit HALT range. */
extern volatile uint8_t muzix_tick_calibration_status;
/* Number of tick interrupts observed before calibration failed (0..65535). */
extern volatile uint16_t muzix_tick_calibration_elapsed_ticks;

/* Read the tick counter, low first in memory and high first here.
 *
 * The 16-bit result comes back in DE, NOT HL.  That is the plain -mz80
 * convention this tree's modules are compiled with, and it is the opposite of
 * the sdcccall(1) rule AGENTS.md quotes - so the return is also left in HL, to
 * satisfy a caller written to either.  Returning it only in HL looks entirely
 * reasonable and is silently wrong: the caller reads DE, computes its deadline
 * from whatever was in that register, and every bounded wait in the system
 * fires at once.  The full story, and the measurement that settles which
 * convention applies, are on the routine in tick.s;
 * tools/check_z80_call_convention.rb compiles a probe and fails the build if
 * this ever stops being the register the compiler actually reads.
 *
 * The high byte is read before the low byte, which is the pairing that makes
 * the read safe against a tick landing in the middle of it.  See the ordering
 * argument in tick.s; a reader that took the low byte first could see a value
 * 65 536 lower than either the old or the new one. */
uint16_t muzix_tick_now(void);

/* Has `deadline` passed?  Returns 0 while it is still in the future and 1 once
 * it is not.
 *
 * A routine and not a macro, deliberately: as a macro this is a call to
 * muzix_tick_now() plus a subtract and a compare at every call site, which
 * measured 20 bytes of _CODE each across the five sites in this tree.  The
 * comparison itself is 17 bytes of assembly, once.  The kernel does not have
 * the 64 KiB budget to spell the same subtraction out five times.
 *
 * The comparison is the wrap-safe one, so a deadline up to 32767 ticks ahead
 * (272 seconds at 120 Hz) is usable and the 16-bit counter wrapping every 546
 * seconds is a non-event.  See the routine for why. */
uint8_t muzix_tick_expired(uint16_t deadline);

/* A deadline `seconds` from now, for muzix_tick_expired().
 *
 * The addition is deliberately left to wrap: a deadline of 0xFFF0 is a
 * perfectly good deadline nine ticks from the wrap, because the test is a
 * subtraction.  `seconds` is 0 to 255; at 120 Hz the largest value that still
 * leaves the deadline inside the half-range the test can see is 255 seconds,
 * and the half-range is 273, so the expression is safe for every `seconds` it
 * can be given. */
#define MUZIX_TICK_DEADLINE(seconds) \
    ((uint16_t)(muzix_tick_now() + MUZIX_TICKS_PER_SECOND * (uint16_t)(seconds)))

/* Book the time since *stamp against a 32-bit tick counter, and restamp.
 *
 * `counter` is a uint32_t because struct proc_info says the published columns
 * are, and the addition is in assembly because SDCC 4.5's 32-bit add through a
 * stack frame measured 247 bytes of _CODE against 53 for the operation itself
 * (platform/zeta-v2/tick.s has the disassembly and the argument).
 *
 * This is the only writer of the process accounting, and it is called from the
 * two ends of the syscall rather than from the tick handler - so the numbers
 * are a count of ticks between boundaries the process itself crossed, not a
 * sample of a running clock.  That distinction is why a bounded tick and real
 * UTIME/STIME are the same change, and why preemptive accounting is not. */
void muzix_tick_add_process_time(uint32_t *counter, uint16_t *stamp);

/* Set *stamp to now: start a process's accounting clock.
 *
 * A slot created with a zero stamp would be charged for every tick since boot
 * by its first call - time it did not exist for, appearing as UTIME it had not
 * earned.  This is a routine rather than `*stamp = muzix_tick_now()` because
 * that spelling measured 19 bytes of _CODE a time in kernel/proc_table.c and
 * this is 10. */
void muzix_tick_stamp_store(uint16_t *stamp);

/* One count of the free-running cycle counter on CTC channel 3.
 *
 * 8 bits, wrapping every 256 counts = 65536 CPU cycles = 16.384 ms at 4 MHz. This
 * exists because the load accounting has to measure something an order of
 * magnitude smaller than a tick: an idle wakeup costs roughly 5500 cycles, and a
 * tick is 61440, so the tick cannot resolve it at all.
 *
 * The only thing it is read for is the gap between one halt returning and the
 * next halt being entered - a few thousand cycles, about 11 counts at 4 MHz. A gap long
 * enough to wrap would be 8.9 ms of the kernel running flat out, which is a
 * different fault entirely and shows up as a load figure of 1.00 rather than as
 * a wrong number here. */
uint8_t muzix_tick_cycles(void);

/* Raw CTC channel down-counters, for the boot probe ONLY.
 *
 * These exist to find out which link of the board's tick chain is alive without
 * depending on an interrupt being delivered - the chain is UART_CLK/2 -> ch0 ->
 * ZC/TO -> ch1 -> tick, and "the tick did not fire" does not say whether channel 0
 * counts, whether channel 1 counts, or whether both count and the vector is
 * wrong.
 *
 * Reading channel 0 acknowledges an outstanding interrupt on real hardware, so
 * this is a diagnostic and not something the kernel may call while running. */
uint8_t muzix_ctc_ch0(void);
uint8_t muzix_ctc_ch1(void);


/* Bytes the UART receive interrupt has collected, and the flag that says so.
 *
 * The handler in tick.s cannot wake a parked process: the map argument there
 * forbids a call out of an interrupt handler, so the handler's job stops at
 * putting the byte in _DATA and raising a flag.  These three are how the rest of
 * the kernel picks it up.
 *
 * muzix_uart_rx_pending() is the one the halt loop calls after a halt returned,
 * because returning from halt is the only thing that means "an interrupt was
 * taken while the processor was stopped" and that is exactly when there might be
 * a byte to hand over.  It READS AND CLEARS the flag, so it must be called only
 * when the caller is really going to do something about the wake - clearing it
 * and then not waking is how a keystroke gets lost.
 *
 * There is deliberately no buffer and no accessor that takes a byte.  The
 * consumer - fs/tty_device.c - already reads the 16550 itself, so a handler that
 * also read it would take bytes away from the only code that uses them; the
 * 16550's own 16-byte FIFO is the buffer.  This interrupt is a wake-up, nothing
 * more, and that is what removes the two-second park latency it was added for.
 *
 * muzix_uart_rx_count() is a 16-bit count of UART interrupts since boot. */
uint16_t muzix_uart_rx_count(void);
uint8_t muzix_uart_rx_pending(void);

#endif
