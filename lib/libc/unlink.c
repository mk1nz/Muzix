#include "libc.h"
#include "syscall.h"

int
unlink(const char *pathname)
{
    return sys_unlink(pathname);
}
