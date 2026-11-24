#include "libc.h"
#include "syscall.h"
#include "tty_ioctl.h"

#include "ztypes.h"

extern void (*muzix_stdout_hook)(void);

/*
 * Stdout is line-buffered, and this is the whole of that.
 *
 * Every character used to be its own sys_write: the banner of the shell is 232
 * characters and therefore 232 complete microkernel round trips - the shell
 * traps, the kernel routes, the FS is dispatched, the byte is copied through an
 * MM exchange, the reply comes back, the shell is dispatched again - measured
 * at about 27,000 instructions per character, which is most of the 6.9 million
 * the boot spends between the scheduler starting and the prompt.  At 4 MHz
 * that is half a minute to print a banner.
 *
 * Line buffering is the default for a console and is what everything here
 * prints anyway: the banner's five lines, uptime's two lines, the error messages
 * all end in a newline.  One write per line is then sixty writes where there
 * were two hundred and thirty, and the shell prompt - which deliberately has no
 * newline - is flushed explicitly where it is printed.
 *
 * The buffer is one per process because libc is linked into each one, so the
 * shell, the apps and the servers each have their own and none of them can see
 * another's.
 */
#define MUZIX_STDOUT_BUF 64
static unsigned char muzix_stdout_buf[MUZIX_STDOUT_BUF];
static unsigned char muzix_stdout_len;

/* Hand the buffer to the kernel and empty it.  Returns what sys_write
 * returned, which is the count or a negative status; the caller of putchar is
 * told success either way, as before, because a console that failed mid-line
 * is not something a print loop can act on. */
static int
muzix_stdout_flush(void)
{
    int rc;

    rc = 0;

    if (muzix_stdout_len != 0) {
        rc = sys_write(1, muzix_stdout_buf, muzix_stdout_len);
        muzix_stdout_len = 0;
    }
    return rc;
}

/* The explicit flush, for output that does not end in a newline: the prompt,
 * and anything a program prints and then blocks on. */
void
muzix_stdout_flush_now(void)
{
    (void)muzix_stdout_flush();
}

int
putchar(int c)
{
    unsigned char value;

    value = (unsigned char)c;

    /* Remember that output happened so exit() can flush the buffer.  Set on
     * every call (puts/printf route through putchar); the store is cheap and
     * the hook is only consulted by exit(). */
    muzix_stdout_hook = (void (*)(void))muzix_stdout_flush;

    if (muzix_stdout_len >= MUZIX_STDOUT_BUF) {
        (void)muzix_stdout_flush();
    }
    muzix_stdout_buf[muzix_stdout_len++] = value;
    if (value == '\n') {
        (void)muzix_stdout_flush();
    }
    return c;
}

void
puts(const char *str)
{
    while (*str) {
        putchar(*str++);
    }
    putchar('\n');
}
