/*
 * fs_clock.c - the FS server's own notion of time.
 *
 * STEP 2a (2026-10-12): the FS server is a userspace process, so the kernel's
 * tick does not exist here.  One thing still needs a counter:
 *
 *   - platform/zeta-v2/uart_io.s's _muzix_zeta_uart_send() spends at most
 *     MUZIX_UART_SEND_TICKS polling for a free transmitter, comparing the low
 *     byte of muzix_tick_count modulo 256.  The FS server link resolves that
 *     reference against this module (servers/fs/fs_server.lk lists fs_clock.rel
 * beside ../../zeta_uart_io.rel), so the UART's bound is measured on THIS
 *     counter, and a counter that never advances is a bound that never
 *     expires.  fs_server.c's fs_server_handle() therefore calls
 *     muzix_clock_tick() once per dispatched request.
 *
 * fs/tty_device.c's muzix_tty_wait_for_input() used to build a deadline from
 * muzix_tick_now() too; it no longer waits at all - it answers with what the
 * port says, now, and lib/libc.c's read() parks on the answer.
 *
 * None of this is an authoritative clock and this is not pretending to be one:
 * it advances once per dispatched request, so it measures IO progress rather
 * than wall time.  The waits that must be bounded in real time - reading a
 * line from the console - are bounded by libc's read(), which parks on the
 * kernel's 120 Hz deadline; this counter is the secondary self-check.
 *
 * A plain C global is correct here and is not the bug the kernel had with its
 * window 0: a userspace process's _DATA stays mapped in its own window 3 for
 * the whole of its life, so there is nothing to remap behind it.
 */

#include "fs_clock.h"

uint16_t muzix_tick_count;

void muzix_clock_tick(void)
{
    muzix_tick_count = (uint16_t)(muzix_tick_count + 1u);
}

uint16_t muzix_tick_now(void)
{
    return muzix_tick_count;
}