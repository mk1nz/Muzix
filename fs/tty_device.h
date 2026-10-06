#ifndef MUZIX_TTY_DEVICE_H
#define MUZIX_TTY_DEVICE_H

#include <stdint.h>
#include <stddef.h>

/* The request numbers, the line-discipline flags and the line buffer size are
 * an interface with programs as well as with this device, so they are written
 * down once in lib/tty_ioctl.h, which lib/libc.h includes too.  They used to be
 * here and only here, which meant a program had to have its own copy of
 * MUZIX_TTY_IOCTL_PENDING and no way to be wrong loudly about it. */
#include "../lib/tty_ioctl.h"

/*
 * Whether the TTY dispatches through the send/recv/available function pointers
 * at all.
 *
 * It must be 0 for the kernel.  SDCC 4.5 with this project's peephole file
 * cannot make an indirect call: for `g_tty_send(buffer[i], ud)` it emits
 *
 *     pop hl ; add hl,bc ; ld a,(hl) ; push bc
 *     ld de,(_g_tty_userdata)
 *     call _muzix_sdz80_call_hl          ; which is only `jp (hl)`
 *
 * with no `ld hl,(g_tty_send)` anywhere - so `jp (hl)` transfers to whatever
 * HL happens to hold, which here is the buffer address on the kernel stack.  The
 * first userspace SYS_WRITE therefore jumped into the stack at 0xFE22 instead
 * of reaching the UART, and the machine rebooted into the boot stub.  The cause
 * is that --peep-file replaces SDCC's *entire* default peephole set with the
 * two rules in platform/zeta-v2/muzix_sdccall.peep.
 *
 * With this 0 the TTY calls the Zeta UART primitives directly.  That is not a
 * restriction in practice: the kernel's only callbacks are exactly those three
 * routines, installed by muzix_zeta_syscall_runtime_init.  The struct fields and
 * muzix_tty_device_set_callbacks() still exist, and still record what was
 * installed, so a test binary can build with -DMUZIX_TTY_CALLBACKS=1 and drive
 * the device with its own callbacks.
 */
#ifndef MUZIX_TTY_CALLBACKS
#define MUZIX_TTY_CALLBACKS 0
#endif

typedef struct {
    uint8_t erase;
    uint8_t kill;
    uint16_t flags;
} muzix_tty_params_t;

typedef struct {
    uint8_t intr;
    uint8_t quit;
    uint8_t start;
    uint8_t stop;
    uint8_t eof;
    uint8_t brk;
} muzix_tty_chars_t;

typedef void (*muzix_tty_send_t)(uint8_t byte, void *userdata);
typedef uint8_t (*muzix_tty_recv_t)(void *userdata);
typedef uint8_t (*muzix_tty_available_t)(void *userdata);

typedef struct {
    muzix_tty_params_t params;
    muzix_tty_chars_t chars;
    muzix_tty_send_t send;
    muzix_tty_recv_t recv;
    muzix_tty_available_t available;
    void *userdata;
    uint8_t input[MUZIX_TTY_INPUT_SIZE];
    uint8_t input_length;
    uint8_t line_ready;
    uint8_t eof_pending;
    uint8_t output_stopped;
} muzix_tty_device_t;

/* Wait for one input byte, giving up after MUZIX_TTY_READ_TICKS.  Returns 1 if
 * a byte is available, 0 if the wait timed out.  0 is a report, not an error
 * code: the caller decides whether running out of patience is a failure.
 *
 * This is in the header, and not `static` in tty_device.c, for a measured
 * reason that is easy to undo by accident: SDCC 4.5 inlines small static
 * functions, and when it inlined this one it took the deadline local and its
 * stack spill with it into muzix_tty_read() and grew that function by 222 bytes
 * for no extra work.  A non-static function is not a candidate.  The reason it
 * is a function at all is on the definition. */
int muzix_tty_wait_for_input(void);

/* Collect one canonical line into tty->input.  Returns 1 when a line is ready or
 * end of file has been seen, 0 when the wait timed out with an empty buffer.
 * Declared here, and not static in tty_device.c, because SDCC 4.5 inlines small
 * statics - and inlining this one put its stack spill back inside
 * muzix_tty_read() and grew that by 222 bytes.  The measurement is on the
 * definition. */
int muzix_tty_collect_line(muzix_tty_device_t *tty);

void muzix_tty_device_init(muzix_tty_device_t *tty);
void muzix_tty_device_set_callbacks(muzix_tty_device_t *tty,
                                     muzix_tty_send_t send,
                                     muzix_tty_recv_t recv,
                                     muzix_tty_available_t available,
                                     void *userdata);
int muzix_tty_read(muzix_tty_device_t *tty,
                   uint8_t *buffer,
                   size_t length,
                   size_t *result);
int muzix_tty_write(muzix_tty_device_t *tty,
                    const uint8_t *buffer,
                    size_t length,
                    size_t *result);
int muzix_tty_ioctl(muzix_tty_device_t *tty, uint16_t request, void *argument);

#endif
