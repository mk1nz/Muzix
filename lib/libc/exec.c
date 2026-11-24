#include "libc.h"
#include "syscall.h"

int
exec(const char *path, const char *const *argv)
{
    return sys_exec(path, argv);
}
