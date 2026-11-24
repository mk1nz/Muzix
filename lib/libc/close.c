#include "libc.h"
#include "syscall.h"

int
close(int fd)
{
    return sys_close(fd);
}
