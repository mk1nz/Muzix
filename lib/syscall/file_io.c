#include "syscall.h"

int32_t
sys_open(const char *pathname, int32_t flags)
{
    return SYSCALL(SYS_OPEN, pathname, flags, 0);
}

int32_t
sys_close(int32_t fd)
{
    return SYSCALL(SYS_CLOSE, fd, 0, 0);
}

int32_t
sys_read(int32_t fd, void *buf, int32_t count)
{
    return SYSCALL(SYS_READ, fd, buf, count);
}

int32_t
sys_write(int32_t fd, const void *buf, int32_t count)
{
    return SYSCALL(SYS_WRITE, fd, buf, count);
}

int32_t
sys_seek(int32_t fd, int32_t offset, int32_t whence)
{
    return SYSCALL(SYS_SEEK, fd, offset, whence);
}

int32_t
sys_park(int32_t ticks)
{
    return SYSCALL(SYS_PARK, ticks, 0, 0);
}
