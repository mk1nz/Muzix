#include "libc.h"
#include "syscall.h"

int
read(int fd, void *buf, size_t count)
{
    int n;

    for (;;) {
        n = sys_read(fd, buf, count);
        if (n == MUZIX_READ_AGAIN) {
            (void)sys_park(240);
            continue;
        }
        return n;
    }
}
