#include "libc.h"
#include "syscall.h"

int
readdir(int fd, int index, struct dirent *entry)
{
    return sys_readdir(fd, index, entry);
}
