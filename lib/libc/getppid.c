#include "libc.h"
#include "syscall.h"

int
getppid(void)
{
    return sys_getppid();
}
