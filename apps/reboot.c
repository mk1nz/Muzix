/*
 * reboot - reboot the Zeta SBC V2
 *
 * Jumps to the ROM boot stub at 0x0000, which re-initializes the
 * machine and re-executes the boot sequence.  This is the userspace
 * equivalent of pressing the reset button: everything is re-initialized
 * from ROM, the kernel restarts, and the shell is re-execed into slot 1.
 */
#include "ztypes.h"

/*
 * Reboot by jumping to the ROM boot stub at 0x0000.  Declared naked so
 * SDCC emits just the JP instruction with no prologue/epilogue.  On the
 * host there is no machine to reboot, so the function is a no-op there.
 */
#if defined(__SDCC)
void
reboot(void) __naked {
    __asm
        jp 0x0000
    __endasm;
}
#else
void
reboot(void) {
    /* no-op on non-Z80 hosts */
}
#endif

int
main(int argc, char **argv) {
    (void)argc; (void)argv;
    reboot(); return 1;
}
