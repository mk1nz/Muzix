#include "libc.h"
#include "syscall.h"

int
mkdir(const char *pathname, int mode)
{
    return sys_mkdir(pathname, mode);
}
