#include "libc.h"
#include "syscall.h"

int
fork(void)
{
    return sys_fork();
}
