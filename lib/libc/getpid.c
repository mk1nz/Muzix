#include "libc.h"
#include "syscall.h"

int
getpid(void)
{
    return sys_getpid();
}
