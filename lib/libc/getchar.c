#include "libc.h"
#include "syscall.h"

int
getchar(void)
{
    unsigned char value;

    if (read(0, &value, 1) != 1) {
        return -1;
    }
    return value;
}
