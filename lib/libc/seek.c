#include "libc.h"
#include "syscall.h"

int
seek(int fd, int offset)
{
    return sys_seek(fd, offset, 0);
}
