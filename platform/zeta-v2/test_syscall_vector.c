#include "syscall_vector.h"

static int test_syscall_vector(void)
{
    uint8_t memory[0x100];
    uint16_t entry = 0x4567u;

    for (unsigned int i = 0; i < sizeof(memory); i++) {
        memory[i] = 0;
    }

    muzix_z80_install_syscall_vector(memory, entry);
    if (memory[MUZIX_Z80_SYSCALL_VECTOR] != MUZIX_Z80_JP_OPCODE ||
        memory[MUZIX_Z80_SYSCALL_VECTOR + 1] != 0x67 ||
        memory[MUZIX_Z80_SYSCALL_VECTOR + 2] != 0x45) {
        return 1;
    }

    muzix_z80_install_syscall_vector(0, entry);
    return 0;
}

int main(void)
{
    return test_syscall_vector();
}
