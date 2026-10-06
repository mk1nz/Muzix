#ifndef MUZIX_ZETA_SYSCALL_VECTOR_H
#define MUZIX_ZETA_SYSCALL_VECTOR_H

#include <stdint.h>

#define MUZIX_Z80_SYSCALL_VECTOR 0x0030u
#define MUZIX_Z80_JP_OPCODE 0xC3u

void muzix_z80_install_syscall_vector(volatile uint8_t *memory,
                                      uint16_t entry);

#endif
