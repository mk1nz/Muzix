#include "libc.h"
#include "syscall.h"

int
open(const char *pathname, int flags)
{
    return sys_open(pathname, flags);
}
