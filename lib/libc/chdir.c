#include "libc.h"
#include "syscall.h"

int
chdir(const char *path)
{
    return sys_chdir(path);
}
