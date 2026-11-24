/* * * TTY ioctl request codes and the MUZIX_READ_AGAIN sentinel,
 * shared by the kernel, libc, and userspace programs.
 */

#ifndef MUZIX_TTY_IOCTL_H
#define MUZIX_TTY_IOCTL_H

#include "ztypes.h"

/* * TTY ioctl request codes.
 * * These live here (a single shared header) rather than in fs/tty_device.h, so
 * the kernel side and the userspace side can never disagree about a number.
 * A mismatched request number silently returns -1 instead of working.
 * * The numbering follows the four pre-existing codes rather than inventing a
 * new scheme.
 */
#define MUZIX_TTY_IOCTL_GETP 0x7408u /*< Get line discipline parameters */
#define MUZIX_TTY_IOCTL_SETP 0x7409u /*< Set line discipline parameters */
#define MUZIX_TTY_IOCTL_SETC 0x7411u /*< Set control characters */
#define MUZIX_TTY_IOCTL_GETC 0x7412u /*< Get control characters */

/* * Query whether unconsumed input is waiting on the console.
 * * The argument is a uint16_t; the result is a count (non-zero if input is
 * waiting, 0 otherwise). This does not block and does not consume: the byte
 * that made the answer non-zero remains available for a later read().
 * * read() on this TTY is canonical and waits for a whole line, so a program
 * that wants to ask "has anyone typed anything" between two displays has no
 * other call it can make.
 */
#define MUZIX_TTY_IOCTL_PENDING 0x7413u

/* * "Nothing to give you right now", as a read() return value.
 * * This must be a distinct value, not -1. A character-device read has three
 * answers: some bytes, none-yet (may arrive later), and error. Collapsing
 * "none yet" into -1 leaves the caller unable to tell "wait for me" from
 * "never going to happen", so every caller spun on a read that would never
 * wait. -1 stays an error; this value is what "not yet" means.
 * * The kernel, libc, and programs all include this header, so they share the
 * same number the kernel writes.
 */
#define MUZIX_READ_AGAIN (-11)

/* Canonical-mode line discipline flag. */
#define MUZIX_TTY_FLAG_CANONICAL 0x0001u

/* Echo flag for the line discipline. */
#define MUZIX_TTY_FLAG_ECHO 0x0002u

/* Bytes of line buffer the TTY keeps - the most ever pending in the device. */
#define MUZIX_TTY_INPUT_SIZE 128u

#endif /* MUZIX_TTY_IOCTL_H */
