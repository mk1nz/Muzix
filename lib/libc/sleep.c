#include "libc.h"
#include "syscall.h"

void
sleep(uint16_t seconds)
{
    uint16_t ticks;

    ticks = (uint16_t)(seconds * 120);
    sys_park(ticks);
}
