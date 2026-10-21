#include "tty_device.h"
#include "fs_syscalls.h"
#include <string.h>

#define MUZIX_TTY_READ_SECONDS 5
#define MUZIX_TICKS_PER_SECOND 120
#define MUZIX_TTY_READ_TICKS (MUZIX_TICKS_PER_SECOND * MUZIX_TTY_READ_SECONDS)

static void tty_put_byte(int fd, uint8_t byte)
{
    size_t result;
    if (byte == '\n') {
        uint8_t cr = '\r';
        sys_write(fd, &cr, 1, &result);
    }
    sys_write(fd, &byte, 1, &result);
}

static void muzix_tty_echo(muzix_tty_device_t *tty, uint8_t byte)
{
    if ((tty->params.flags & MUZIX_TTY_FLAG_ECHO) != 0) {
        tty_put_byte(tty->tty_fd, byte);
    }
}

static void tty_echo_erased_char(muzix_tty_device_t *tty)
{
    muzix_tty_echo(tty, '\b');
    muzix_tty_echo(tty, ' ');
    muzix_tty_echo(tty, '\b');
}

#define tty_is_erase(tty, byte)                                        \
    ((byte) == 0x08u || (byte) == 0x7Fu || (byte) == (tty)->params.erase)

void muzix_tty_device_init(muzix_tty_device_t *tty)
{
    if (!tty) {
        return;
    }
    memset(tty, 0, sizeof(*tty));
    tty->params.flags = MUZIX_TTY_FLAG_CANONICAL | MUZIX_TTY_FLAG_ECHO;
    tty->params.erase = 0x7F;
    tty->params.kill = 21;
    tty->chars.intr = 3;
    tty->chars.quit = 28;
    tty->chars.start = 17;
    tty->chars.stop = 19;
    tty->chars.eof = 4;
    tty->chars.brk = 0;
    tty->tty_fd = 0;
}

int muzix_tty_read(muzix_tty_device_t *tty,
                   uint8_t *buffer,
                   size_t length,
                   size_t *result)
{
    size_t count = 0;
    uint8_t byte;
    int32_t ret;

    if (!tty || !buffer || !result) {
        return -1;
    }

    if ((tty->params.flags & MUZIX_TTY_FLAG_CANONICAL) != 0) {
        if (tty->line_ready == 0 && tty->eof_pending != 0) {
            tty->eof_pending = 0;
            *result = 0;
            return 0;
        }

        while (tty->line_ready == 0 && tty->eof_pending == 0) {
            ret = sys_read(tty->tty_fd, &byte, 1);
            if (ret == MUZIX_READ_AGAIN || ret == 0) {
                sys_park(MUZIX_TTY_READ_TICKS);
                ret = sys_read(tty->tty_fd, &byte, 1);
                if (ret <= 0) {
                    *result = 0;
                    return MUZIX_READ_AGAIN;
                }
            }
            if (ret < 0) {
                *result = 0;
                return MUZIX_READ_AGAIN;
            }

            if (byte == tty->chars.stop) {
                tty->output_stopped = 1;
            } else if (byte == tty->chars.start) {
                tty->output_stopped = 0;
            } else if (tty_is_erase(tty, byte)) {
                if (tty->input_length != 0) {
                    tty->input_length--;
                    tty_echo_erased_char(tty);
                }
            } else if (byte == tty->params.kill) {
                while (tty->input_length != 0) {
                    tty->input_length--;
                    tty_echo_erased_char(tty);
                }
            } else if (byte == tty->chars.eof) {
                tty->eof_pending = 1;
                if (tty->input_length != 0) {
                    tty->line_ready = 1;
                }
            } else {
                if (byte == '\r') {
                    byte = '\n';
                }
                if (tty->input_length < MUZIX_TTY_INPUT_SIZE) {
                    tty->input[tty->input_length++] = byte;
                    muzix_tty_echo(tty, byte);
                }
                if (byte == '\n') {
                    tty->line_ready = 1;
                }
            }
        }

        while (tty->line_ready != 0 && count < length &&
               tty->input_length != 0) {
            buffer[count++] = tty->input[0];
            memmove(tty->input, tty->input + 1, tty->input_length - 1);
            tty->input_length--;
        }
        if (tty->line_ready != 0 && tty->input_length == 0) {
            tty->line_ready = 0;
        }
        *result = count;
        return 0;
    }

    ret = sys_read(tty->tty_fd, &byte, 1);
    if (ret <= 0) {
        *result = 0;
        return MUZIX_READ_AGAIN;
    }
    buffer[count++] = byte;
    muzix_tty_echo(tty, byte);

    while (count < length) {
        ret = sys_read(tty->tty_fd, &byte, 1);
        if (ret <= 0) {
            break;
        }
        if (byte == tty->chars.stop) {
            tty->output_stopped = 1;
        } else if (byte == tty->chars.start) {
            tty->output_stopped = 0;
        } else {
            buffer[count++] = byte;
            muzix_tty_echo(tty, byte);
        }
    }
    *result = count;
    return 0;
}

int muzix_tty_write(muzix_tty_device_t *tty,
                    const uint8_t *buffer,
                    size_t length,
                    size_t *result)
{
    size_t index;
    int32_t ret;

    if (!tty || !result || !buffer) {
        return -1;
    }
    *result = 0;
    if (length == 0) {
        return 0;
    }
    if (tty->output_stopped) {
        return -1;
    }
    for (index = 0; index < length; index++) {
        ret = sys_write(tty->tty_fd, &buffer[index], 1, result);
        if (ret < 0) {
            return -1;
        }
    }
    *result = length;
    return 0;
}

static uint16_t muzix_tty_input_pending(muzix_tty_device_t *tty)
{
    uint16_t pending = tty->input_length != 0 ? 1u : 0u;
    uint8_t byte;
    int32_t ret = sys_read(tty->tty_fd, &byte, 1);
    if (ret > 0) {
        pending++;
    }
    return pending;
}

int muzix_tty_ioctl(muzix_tty_device_t *tty, uint16_t request, void *argument)
{
    if (!tty || !argument) {
        return -1;
    }
    switch (request) {
    case MUZIX_TTY_IOCTL_GETP:
        *(muzix_tty_params_t *)argument = tty->params;
        return 0;
    case MUZIX_TTY_IOCTL_SETP:
        tty->params = *(const muzix_tty_params_t *)argument;
        return 0;
    case MUZIX_TTY_IOCTL_GETC:
        *(muzix_tty_chars_t *)argument = tty->chars;
        return 0;
    case MUZIX_TTY_IOCTL_SETC:
        tty->chars = *(const muzix_tty_chars_t *)argument;
        return 0;
    case MUZIX_TTY_IOCTL_PENDING:
        *(uint16_t *)argument = muzix_tty_input_pending(tty);
        return 0;
    default:
        return -1;
    }
}