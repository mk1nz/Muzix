#include "libc.h"
#include "syscall.h"

char *
getcwd(char *buf, size_t size)
{
    return sys_getcwd(buf, size);
}
