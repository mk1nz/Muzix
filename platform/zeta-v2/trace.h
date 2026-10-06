#ifndef MUZIX_ZETA_TRACE_H
#define MUZIX_ZETA_TRACE_H

#include <stdint.h>

/*
 * Kernel trace ring.
 *
 * The kernel's code and data must both fit in the single 64 KiB Z80 address
 * space, so tracing cannot be paid for with inline prints: 186 scattered
 * dbg_puts() call sites and their literals cost about 4.4 KiB of code, which
 * is more than the entire overflow this build had to shed.
 *
 * Instead every trace byte lands in a fixed ring here, and the ring is dumped
 * on demand.  That costs a few hundred bytes once, keeps every existing call
 * site working unchanged, and gives better post-mortem data than prints that
 * only fire on the happy path.
 *
 * The ring overwrites its oldest bytes rather than stopping when full, so a
 * failure that happens late still leaves the most recent events intact.  The
 * total byte count is kept so the dump can report how much was lost.  It is 32
 * bits wide and does not saturate: a run long enough to be worth inspecting is
 * exactly the run whose byte count must not stop moving.
 */

#define MUZIX_TRACE_RING_SIZE 128u

void muzix_trace_putc(uint8_t c);
void muzix_trace_write(const char *s);
void muzix_trace_hex8(uint8_t v);
void muzix_trace_clear(void);

/* Writes the ring to the UART oldest-first, preceded by a short header. */
void muzix_trace_dump(void);

/* Number of bytes ever written since the last muzix_trace_clear(). */
uint32_t muzix_trace_total(void);

/* Number of bytes dropped because the ring overwrote them. */
uint32_t muzix_trace_dropped(void);

/*
 * Stack high-water mark.
 *
 * The kernel's C stack is the span between the top of _DATA and 0xFFFE, and
 * _CODE already fills 0x0098..0xE9FA, so that span is only about 1.2 KB.  There
 * is no room to reason about the deepest call chain from the source: the frames
 * that matter (muzix_handle_userspace_syscall at 424 bytes, the fs_service
 * directory operations at 264-271) are individually plausible and only their
 * sum decides whether the kernel writes through its own globals.  Measuring is
 * cheaper than guessing.
 *
 * Call muzix_trace_stack_mark() at the entry of every function that can be the
 * bottom of a chain; muzix_trace_stack_report() prints the lowest SP seen.
 */
void muzix_trace_stack_mark(void);
void muzix_trace_stack_report(void);

#endif
