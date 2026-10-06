/*
 * Host model of the two UART receive counters that live in assembly.
 *
 *   muzix_uart_rx_count()   platform/zeta-v2/tick.s
 *   muzix_uart_rx_pending() platform/zeta-v2/tick.s
 *
 * WHY THIS IS NEEDED
 * ------------------
 * kernel/kernel_loop.c calls muzix_uart_rx_pending() on every return from HALT,
 * and fs/tty_device.c calls muzix_uart_rx_count() when it checks for work.  Both
 * are assembly, so a host test that compiles either cannot link without them - and
 * fifteen tests dropped silently out of the host gate when they were first added,
 * because the runner reports a missing module as NOT BUILT per test while the
 * total still reads like a pass.
 *
 * WHY IT MODELS A FLAG AND NOT A BUFFER
 * ------------------------------------
 * Because that is all the target has.  There is no ring here and no byte-taking
 * accessor, deliberately: the consumer in fs/tty_device.c already reads the 16550
 * directly, and the 16550's own 16-byte FIFO is the buffer.  A host model that
 * queued bytes would have let a test pass against behaviour the target does not
 * have - which is exactly how the earlier ring version shipped and then took the
 * byte away from the code that uses it, so the shell read a zero-length read,
 * took it for end of file, and exited at its prompt.
 *
 * So this models the two counters and the flag's read-and-clear, and the tests
 * that need to pretend a byte arrived use host_uart_rx_pending_set().
 */

#include <stdint.h>

#include "../platform/zeta-v2/tick.h"

static uint16_t g_rx_count;
static uint8_t g_rx_pending;

uint16_t muzix_uart_rx_count(void)
{
    return g_rx_count;
}

uint8_t muzix_uart_rx_pending(void)
{
    uint8_t value = g_rx_pending;

    g_rx_pending = 0;
    return value;
}

/* What the interrupt handler does: count it and raise the flag. */
void host_uart_rx_pending_set(void)
{
    g_rx_count++;
    g_rx_pending = 1;
}

void host_uart_rx_reset(void)
{
    g_rx_count = 0;
    g_rx_pending = 0;
}