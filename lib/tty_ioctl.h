#ifndef MUZIX_TTY_IOCTL_H
#define MUZIX_TTY_IOCTL_H

#include <stdint.h>

/*
 * TTY ioctl requests, the one place they are written down.
 *
 * These are an interface between the kernel and a program, so they live here
 * rather than in fs/tty_device.h, which is a kernel header: the FS side
 * includes this, and so does lib/libc.h, and there is therefore no second
 * copy of a number for the two to disagree about.  That is a deliberate
 * departure from the syscall numbers, which are written out in both
 * kernel/syscalls.h and lib/syscall.h with a comment asking the reader to keep
 * them in step.  A request number that drifts does not fail: the syscall
 * arrives, the switch finds no case, and the program gets -1 for a request
 * that is supposed to work.
 *
 * The numbering follows the existing four rather than a scheme of its own.
 */
#define MUZIX_TTY_IOCTL_GETP 0x7408u
#define MUZIX_TTY_IOCTL_SETP 0x7409u
#define MUZIX_TTY_IOCTL_SETC 0x7411u
#define MUZIX_TTY_IOCTL_GETC 0x7412u

/*
 * Is there unconsumed input waiting on the console?
 *
 * The argument is a uint16_t and the answer is a count, not a flag: 0 when
 * nothing is waiting, non-zero when something is.  A count rather than a
 * boolean because the device has two places a byte can be and a program
 * generally only wants to know about the pair, and because a count is the
 * shape a program can print or test without a second convention.  The value is
 * not a byte count a program can rely on - it reports one for the receive
 * register rather than how many bytes the FIFO holds - so the contract is
 * "zero or not zero", and nothing more.
 *
 * Two properties this has to have, and both are the reason it is a request of
 * its own rather than a mode of read():
 *
 *   - It must not block.  read() on this TTY is canonical and waits for a
 *     whole line (muzix_tty_read, fs/tty_device.c), so a program that wants to
 *     ask "has anyone typed anything" once between two displays has no call it
 *     can make: read() would not answer until the user pressed Enter, and a
 *     program that treated that as an answer would hang for as long as the
 *     user felt like typing nothing.
 *
 *   - It must not consume.  A keypress checked and thrown away is a keypress
 *     the user cannot see they made, and the shell that gets the terminal back
 *     would not see it either.  So this reads the UART's line status register
 *     and looks at the device's own line buffer, and touches neither the
 *     receive register nor the buffer.  The byte is still there afterwards,
 *     which is what makes the check safe to repeat every two seconds.
 *
 * This is the predicate the TTY has always had - `available` in the callback
 * set, the same one muzix_tty_read's wait loop uses - exposed rather than
 * newly computed.  What was missing was a way for a program to ask.
 */
#define MUZIX_TTY_IOCTL_PENDING 0x7413u

/* "Nothing to give you right now", as a read() return.
 *
 * This has to be a value of its own and not -1, and the reason is who is
 * supposed to act on it.
 *
 * A read on a character device has three possible answers - here are some
 * bytes, here are none but there may be later, and this is an error - and all
 * three have to be told apart, because the caller does something different with
 * each.  The middle one is the one that is waited on.  Collapsing it into -1
 * leaves the caller unable to tell "wait for me" from "never going to happen",
 * and since only one of those two is worth retrying, a caller has to guess.
 *
 * The guess was the bug, and it was not a small one.  -1 is what
 * fs/tty_device.c returned for an idle read, so lib/libc.c's read() saw a
 * negative result and returned it straight to the caller instead of parking,
 * and every caller of read() on the console was left with a `while (result < 0)
 * retry` loop around a read that would never wait.  The machine spent 100% of
 * its time in those loops, and the load figure was right to say so: no tick
 * was ever given back, because nothing was ever parked.
 *
 * So -1 stays an error and means what it means everywhere else, and this one
 * value is what "not yet" is.  It is declared here because this header is the
 * one piece of the read interface that the kernel, libc and programs all
 * include: fs/tty_device.h includes it, lib/libc.h includes it, and a program
 * that wants to make its own read loop - rather than using read(), which
 * parks for it - needs the same number the kernel writes.
 *
 * EAGAIN is the POSIX spelling of this and -11 is the value Linux uses for it.
 * The kernel has no other error codes to collide with, since every other
 * failure it reports is the bare -1 this exists to be distinct from.
 */
#define MUZIX_READ_AGAIN (-11)

/* Line discipline, as reported and set by GETP/SETP. */
#define MUZIX_TTY_FLAG_CANONICAL 0x0001u
#define MUZIX_TTY_FLAG_ECHO      0x0002u

/* Bytes of line buffer this TTY keeps, which is the most that can ever be
 * pending inside the device however much is in the receive FIFO. */
#define MUZIX_TTY_INPUT_SIZE 128u

#endif /* MUZIX_TTY_IOCTL_H */
