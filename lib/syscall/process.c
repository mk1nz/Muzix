#include "syscall.h"

int32_t
sys_fork(void)
{
    return SYSCALL(SYS_FORK, 0, 0, 0);
}

int32_t
sys_exec(const char *path, const char * const *argv)
{
    return SYSCALL(SYS_EXEC, path, argv, 0);
}

void
sys_exit(int32_t status)
{
    SYSCALL(SYS_EXIT, status, 0, 0);
    /* Never returns */
    while (1) {}
}

void
_exit(int32_t status)
{
    sys_exit(status);
}

int32_t
sys_wait(int32_t *status)
{
    return SYSCALL(SYS_WAIT, status, 0, 0);
}
