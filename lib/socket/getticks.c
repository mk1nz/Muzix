#include "syscall.h"

uint16_t
sys_getticks(void)
{
    return SYSCALL(SYS_GETTICKS, 0, 0, 0);
}
