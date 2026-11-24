#include "syscall.h"

int32_t
sys_getpid(void)
{
    return SYSCALL(SYS_GETPID, 0, 0, 0);
}

int32_t
sys_getppid(void)
{
    return SYSCALL(SYS_GETPPID, 0, 0, 0);
}
