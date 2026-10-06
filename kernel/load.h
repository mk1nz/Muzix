/*
 * CPU busy fraction in three windows, kept by the kernel.
 *
 * The figure is the fraction of wall time the PROCESSOR was executing - the
 * kernel's own work included, because the kernel is a consumer of the CPU like
 * anything else and on this machine it is a measurable share of it.  It is not the
 * sum of process times.
 *
 * It is measured as the interval's own length minus the time the processor spent
 * stopped, with the stopped time read from a free-running cycle counter on CTC
 * channel 3 and the subtraction made in the kernel.  That makes it a partition:
 * every cycle is either halted or executing, and nothing is unattributed.
 * load.c's muzix_load_note_halted() has the measurement, the arithmetic, and the
 * two things that were tried and were wrong - the tick cannot resolve it, and the
 * scheduler's own awake gap is a true fact about the wrong thing.
 *
 * Per-process UTIME and STIME are a different quantity and are unaffected: those
 * are one process's own running time.  Here it is the machine's.
 *
 * See load.c for why this is not in apps/top.c and why it is not the Unix load
 * average.  The three windows are Q14 fractions of wall time, with 1, 5 and 15
 * minute constants.  LOAD_Q14_SCALE (16384) is 1.0.
 *
 * The functions take the table and the tick as arguments rather than reaching for
 * globals, which is what makes them testable: a host test can drive them with a
 * table it fills itself and a tick count it chooses.  That is the whole reason
 * for this shape - platform/zeta-v2/test_tick.c tests a host uint16_t standing
 * in for the tick and so could not see a byte-order or pointer error in the real
 * routine, and passed through three of them.
 */

#ifndef MUZIX_LOAD_H
#define MUZIX_LOAD_H

#include <stdint.h>

#include "proc_table.h"

/* 1.0 in Q14. */
#define LOAD_Q14_SCALE 16384u

/* Fallback for a failed startup calibration on the known 4 MHz configuration. */
#define MUZIX_LOAD_CTC3_COUNTS_PER_TICK 130u

/* Record the boot instant and the calibrated CTC3 scale. */
void muzix_load_init(uint16_t now_ticks, uint16_t counts_per_tick);

/* Forget the last sample; the next is seeded from boot again.  For tests. */
void muzix_load_reset(void);

/* Fold the interval that has just ended into the three windows. */
void muzix_load_sample(muzix_kernel_proc_table_t *table, uint16_t now_ticks);

/* Record `counts` of time the processor has just spent stopped in HALT, since
 * the last sample.
 *
 * `counts` is CTC channel 3, so one count is 256 cycles.  Called by the
 * scheduler's idle loops around z80_halt(); see kernel/kernel_loop.c's
 * kernel_halt() for why they go through one place, and load.c's
 * muzix_load_note_halted() for why it is the STOPPED time that is handed over and
 * not the loop's own - handing over the loop instead produced a figure that read
 * zero on a machine running at full speed.
 * Accumulated here and consumed by the next muzix_load_sample(), which resets it
 * - so a caller reports only what has happened since the last read.
 *
 * The argument is ONE halt, not an interval's total, and it has to be: a halt is
 * at most a tick, 130 counts, while a minute of them is 468 000 and does not fit
 * sixteen bits.  A caller that tries to report a whole long interval in one call
 * gets it silently truncated. */
void muzix_load_note_halted(uint16_t counts);

/* One window: 0 for 1 minute, 1 for 5, 2 for 15.  Q14.  Out of range gives 0. */
uint32_t muzix_load_window(int index);

#endif /* MUZIX_LOAD_H */
