#ifndef MUZIX_Z80_IO_H
#define MUZIX_Z80_IO_H

#include <stdint.h>

void z80_outb(uint8_t port, uint8_t value);

/* Stop the processor until the next enabled interrupt, then carry on.
 *
 * Not inline asm in C: an SDCC `__asm` block does not compile on a host, and
 * putting one in kernel_loop.c removed that module from the host build.  Returns
 * rather than jumping, because the caller has work to do afterwards. */
void z80_halt(void);

/* Enable interrupts and halt atomically with respect to the EI/HALT sequence.
 *
 * Use this for idle waits: keeping EI and HALT in one assembly routine prevents
 * an interrupt from being handled in the gap between separate C calls. */
void z80_idle_halt(void);

/* The stack pointer AS THIS CALL'S CALLER SAW IT - the +2 that undoes the call.
 *
 * That is the value a park has to store, and the word sitting at it is the return
 * address into the caller - which is the PC a resume jumps to, since a Z80 keeps a
 * suspended process's PC in its own stack frame.  FUZIX relies on this and nothing
 * else; see the comment on z80_capture_sp() in z80_io.s. */
uint16_t z80_capture_sp(void);

/* The USER's stack pointer while a system call is in flight, and - the word AT it
 * - the address in the process that returning to the trap hands back.
 *
 * Both are the resume point a parked process needs, and the trap computes them on
 * every call: it parks the user's pre-call SP at 0xFFFC on the way in and uses
 * that cell to return on the way out.  Reading that cell is a platform primitive,
 * not arithmetic, which is why it is assembly with a host stand-in - on a host
 * 0xFFFC is unmapped and reading it faults. */
uint16_t z80_syscall_user_sp(void);

/* Enable and disable maskable interrupts (IFF1).
 *
 * Put z80_enable_interrupts() immediately before z80_halt() rather than trusting
 * the caller's interrupt state.  FUZIX does exactly this at all three of its
 * plt_idle() call sites in Kernel/process.c, with the comment "yes please,
 * interrupts on (WRS: they probably are already on?)"; its scheduler runs with them
 * off, so relying on IF would be a hang. */
void z80_enable_interrupts(void);
void z80_disable_interrupts(void);

/* No z80_inb(): the Zeta V2 bank select registers ($78-$7B) and MPGENA ($7C)
 * are write-only and must never be read back.  Peripheral reads live in the
 * port-explicit drivers (uart_io.s, and the FDC code in kernel/). */

#endif
