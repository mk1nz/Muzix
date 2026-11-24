#include "libc.h"
#include "syscall.h"

/* Halt-wake CTC3 snapshot variables for loadavg accounting.
 * Defined in libc.lib so every userspace program linking it gets
 * the definition.  The kernel defines its own copy in
 * platform/zeta-v2/z80_io.s; the two are never linked together. */
uint8_t muzix_halt_wake_ctc3;
uint8_t muzix_halt_wake_ctc3_valid;

void
sync(void)
{
    sys_sync();
}
