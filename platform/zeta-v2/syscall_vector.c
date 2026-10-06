#include "syscall_vector.h"

void muzix_z80_install_syscall_vector(volatile uint8_t *memory,
                                      uint16_t entry)
{
    if (!memory) {
        return;
    }

    memory[MUZIX_Z80_SYSCALL_VECTOR] = MUZIX_Z80_JP_OPCODE;
    memory[MUZIX_Z80_SYSCALL_VECTOR + 1] = (uint8_t)entry;
    memory[MUZIX_Z80_SYSCALL_VECTOR + 2] = (uint8_t)(entry >> 8);
}
