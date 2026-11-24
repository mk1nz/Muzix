#include "libc.h"
#include "syscall.h"

int
wait(int *status)
{
    return sys_wait(status);
}
