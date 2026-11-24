#include "libc.h"
#include "syscall.h"

int
stat(const char *pathname, void *statbuf)
{
    return sys_stat(pathname, statbuf);
}
