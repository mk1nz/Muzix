#include "libc.h"
#include "syscall.h"

int
write(int fd, const void *buf, size_t count)
{
    return sys_write(fd, buf, count);
}
