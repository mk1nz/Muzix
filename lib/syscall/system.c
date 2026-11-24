#include "syscall.h"

int32_t
sys_sync(void)
{
    return SYSCALL(SYS_SYNC, 0, 0, 0);
}

int32_t
sys_readdir(int32_t fd, int32_t index, void *entry)
{
    return SYSCALL(SYS_READDIR, fd, index, entry);
}
