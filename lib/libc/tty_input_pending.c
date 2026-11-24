#include "libc.h"
#include "syscall.h"

int
tty_input_pending(void)
{
    uint16_t pending;

    pending = 0;

    if (sys_ioctl(0, MUZIX_TTY_IOCTL_PENDING, &pending) != 0) {
        return -1;
    }
    return pending != 0;
}
