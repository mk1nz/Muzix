#include "libc.h"
#include "syscall.h"

extern void (*muzix_stdout_hook)(void);

void
exit(int status)
{
    /* Flush any buffered stdout before leaving.  The hook lives in crt0's
     * _DATA (zeroed at startup) and is set by putchar/puts/printf on first
     * output, so exit() itself does not reference stdout.rel: a program that
     * never prints does not carry the line buffer and flush code.  A direct
     * sys_exit() bypasses this, as does _exit; those paths own their output. */
    if (muzix_stdout_hook != (void (*)(void))0) {
        muzix_stdout_hook();
    }
    sys_exit(status);
}
