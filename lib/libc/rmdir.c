#include "libc.h"
#include "syscall.h"

int
rmdir(const char *pathname)
{
    return sys_rmdir(pathname);
}
