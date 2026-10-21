#ifndef MUZIX_Z80_IO_H
#define MUZIX_Z80_IO_H

#include <stdint.h>

/* Userspace I/O port access for Z80 diagnostics.
 *
 * These functions provide direct IN/OUT port access for userspace
 * programs.  They are distinct from the kernel-only
 * platform/zeta-v2/z80_io.s (which intentionally lacks z80_inb
 * because the bank select registers are write-only).
 *
 * SAFE TO READ: peripheral ports such as the 8255 PPI ($60-$63),
 * NS16550 UART ($68-$6F), and the DS1302 RTC ($70).
 *
 * NEVER READ: bank selector ports ($78-$7B) and MPGENA ($7C) —
 * these are write-only and reading them returns undefined values.
 */

void z80_outb(uint8_t port, uint8_t value);
uint8_t z80_inb(uint8_t port);

#endif /* MUZIX_Z80_IO_H */
