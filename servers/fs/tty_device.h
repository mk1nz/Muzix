#ifndef MUZIX_TTY_DEVICE_H
#define MUZIX_TTY_DEVICE_H

#include <stdint.h>
#include <stddef.h>

#include "../lib/tty_ioctl.h"

#define MUZIX_TTY_INPUT_SIZE 128u

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

typedef struct {
    muzix_tty_params_t params;
    muzix_tty_chars_t chars;
    uint8_t input[MUZIX_TTY_INPUT_SIZE];
    uint8_t input_length;
    uint8_t line_ready;
    uint8_t eof_pending;
    uint8_t output_stopped;
    int tty_fd;
} muzix_tty_device_t;

void muzix_tty_device_init(muzix_tty_device_t *tty);
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