/*
 * Host stand-ins for the Z80 primitives that live in assembly.
 *
 * Every other module in the tree is plain C and builds with the host compiler
 * unchanged; these two are not, because they are the point at which the tree
 * touches the board.
 *
 *   z80_outb()          platform/zeta-v2/z80_io.s    `out (port),a`
 *   muzix_zeta_uart_*() platform/zeta-v2/uart_io.s   `in`/`out` against $68-$6D
 *
 * A test cannot exercise an `out` instruction, and must not pretend to: what is
 * under test in every case that reaches these is the logic that decides *which*
 * port gets *which* value, and the module keeps its own shadow of the register
 * state.  So the port write here records what the code asked for and goes no
 * further.  The record is read by nothing today; it is kept because a test that
 * needs to assert "this module programmed bank register 2 with page 0x25" has
 * to be able to, and because discarding the value silently would make the seam
 * invisible.
 *
 * This file is in test_host/ and is not in KERNEL_C_MODULES, so it is never
 * compiled by SDCC and never reaches the ROM.  tools/host_tests.rb links it
 * into every host test binary.
 */

#include <stdint.h>

#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/uart_io.h"

#define HOST_PORT_LOG_SIZE 256

typedef struct {
    uint8_t port;
    uint8_t value;
} host_port_write_t;

static host_port_write_t g_port_log[HOST_PORT_LOG_SIZE];
static uint16_t g_port_log_count;

/* Optional per-port observers, installed by a test that wants the wire to go
 * somewhere - a chip model, an instrumented device.  NULL means the write is
 * recorded and dropped and the read answers 0. */
static void (*g_port_write)(uint8_t port, uint8_t value);
static uint8_t (*g_port_read)(uint8_t port);

void muzix_host_reset_port_log(void);
uint16_t muzix_host_port_log_count(void);
uint8_t muzix_host_port_log_port(uint16_t index);
uint8_t muzix_host_port_log_value(uint16_t index);

void muzix_host_reset_port_log(void)
{
    uint16_t index;

    for (index = 0; index < g_port_log_count; index++) {
        g_port_log[index].port = 0;
        g_port_log[index].value = 0;
    }
    g_port_log_count = 0;
}

uint16_t muzix_host_port_log_count(void)
{
    return g_port_log_count;
}

uint8_t muzix_host_port_log_port(uint16_t index)
{
    return index < g_port_log_count ? g_port_log[index].port : 0;
}

uint8_t muzix_host_port_log_value(uint16_t index)
{
    return index < g_port_log_count ? g_port_log[index].value : 0;
}

/* z80_halt()          platform/zeta-v2/z80_io.s    `halt`
 *
 * The stand-in returns immediately.  A host has nothing to halt until, and a real
 * one would never come back - which is the point of the instruction and the reason
 * it lives in assembly where a host test cannot reach it. */
void z80_halt(void)
{
}

void z80_idle_halt(void)
{
}

/* z80_capture_sp()          platform/zeta-v2/z80_io.s    reads SP through the stack
 *
 * A host has no Z80 stack to report, so this returns zero.  Nothing in the host
 * gate reads the value back - the parking test drives kernel/proc_table.c with a
 * table and a slot it fills itself, and never executes this. */
uint16_t z80_capture_sp(void)
{
    return 0;
}

/* z80_syscall_user_sp()     platform/zeta-v2/z80_io.s    reads 0xFFFC
 *
 * Zero, deliberately.  On a host 0xFFFC is an ordinary unmapped page, and reading
 * the real address faults, so a stand-in that told the truth would take the test
 * down rather than test anything.  Zero is what a process outside a system call
 * would have - no resume point - and get_resume() reports that by returning 0, so
 * a host test of parking sees the same "not recorded" the real code sees there. */
uint16_t z80_syscall_user_sp(void)
{
    return 0;
}

/* z80_ei / z80_di            platform/zeta-v2/z80_io.s    `ei` / `di`
 *
 * No-ops on a host.  Nothing here can deliver an interrupt anyway, and the callers
 * are the same ones the real build uses, so the shape of the code under test is the
 * same even though the effect cannot be. */
void z80_enable_interrupts(void)
{
}

void z80_disable_interrupts(void)
{
}

void z80_outb(uint8_t port, uint8_t value)
{
    if (g_port_log_count < HOST_PORT_LOG_SIZE) {
        g_port_log[g_port_log_count].port = port;
        g_port_log[g_port_log_count].value = value;
        g_port_log_count++;
    }
    if (g_port_write != 0) {
        g_port_write(port, value);
    }
}

/* z80_inb() is the one primitive with no Z80 counterpart here, and that is
 * deliberate rather than an omission: platform/zeta-v2/z80_io.h carries no
 * z80_inb() because the bank select registers at $78-$7B and MPGENA at $7C are
 * write-only and must never be read back.  The DS1302's data line at $70 is not
 * in that set - it is the one port whose bit 0 the master has to sample - so the
 * RTC driver reads it, and a host build of that driver needs something to read.
 *
 * A test that wants a value on the wire attaches its own reader with
 * muzix_host_port_attach(); with nothing attached, an unmapped port reads 0,
 * which is the same answer a machine with no peripheral on it gives. */
uint8_t z80_inb(uint8_t port);

uint8_t z80_inb(uint8_t port)
{
    return g_port_read != 0 ? g_port_read(port) : 0u;
}

void muzix_host_port_attach(void (*write)(uint8_t port, uint8_t value),
                            uint8_t (*read)(uint8_t port))
{
    g_port_write = write;
    g_port_read = read;
}

void muzix_host_port_detach(void)
{
    g_port_write = 0;
    g_port_read = 0;
}

/* The UART is a NS16550 at $68-$6D.  Off the board there is no receive holding
 * register, so nothing is ever available and recv() returns 0; a module that
 * calls these directly is waiting for hardware, not computing anything, and the
 * host test that does that is reported rather than given a fake answer.  The
 * TTY device is not in that position - fs/tty_device.h directs host builds at
 * -DMUZIX_TTY_CALLBACKS=1, where these three are not called at all. */
void muzix_zeta_uart_init(void)
{
}

void muzix_zeta_uart_send(uint8_t byte, void *userdata)
{
    (void)byte;
    (void)userdata;
}

uint8_t muzix_zeta_uart_recv(void *userdata)
{
    (void)userdata;
    return 0;
}

uint8_t muzix_zeta_uart_available(void *userdata)
{
    (void)userdata;
    return 0;
}

/* kernel_entry.s, not uart_io.s: it is the window-0 entry that restores a
 * process map and jumps into it.  A host test that called it would be trying to
 * schedule a process, which needs a banked address space this does not have. */
void muzix_zeta_enter_userspace(void)
{
}

/* The tick, off the board.  See platform/zeta-v2/test_tick.c for what this is
 * for and platform/zeta-v2/tick.s for the rules it restates. */
uint16_t muzix_host_tick;

void muzix_host_tick_set(uint16_t ticks)
{
    muzix_host_tick = ticks;
}

void muzix_host_tick_advance(uint16_t ticks)
{
    muzix_host_tick = (uint16_t)(muzix_host_tick + ticks);
}

void muzix_tick_init(void)
{
}

uint16_t muzix_tick_now(void)
{
    return muzix_host_tick;
}

/* Expired is now - deadline in the LOW half of the range, which is the whole
 * content of this function and the easiest thing in the tick to get backwards.
 * tick.s gets it from the carry: `sbc hl,de` carries while now is behind the
 * deadline and the routine returns 1 on the other branch.  So
 *
 *     expired  <=>  now >= deadline  <=>  (now - deadline) < 0x8000
 *
 * and the first version of this stub said `>= 0x8000`, which is the negation.
 * platform/zeta-v2/test_tick.c caught it, and that is the argument for keeping
 * the stub as C that is tested against the same rules the assembly implements:
 * the two had drifted, and a host test of a bounded wait would have been testing
 * the wrong rule while passing - or failing for a reason that had nothing to do
 * with the code under test. */
uint8_t muzix_tick_expired(uint16_t deadline)
{
    return (uint8_t)((uint16_t)(muzix_host_tick - deadline) < 0x8000u);
}

void muzix_tick_add_process_time(uint32_t *counter, uint16_t *stamp)
{
    *counter += (uint16_t)(muzix_host_tick - *stamp);
    *stamp = muzix_host_tick;
}

void muzix_tick_stamp_store(uint16_t *stamp)
{
    *stamp = muzix_host_tick;
}

/* The free-running cycle counter on CTC channel 3.
 *
 * A host model rather than a constant, because the two things a test needs from
 * it are different: the scheduler's idle loops READ it to measure the awake gap
 * between two halts, and a test that wants to check that arithmetic has to be
 * able to move it.  Returning a fixed number would make every gap zero and the
 * measurement vacuously correct.
 *
 * It models the real part's shape and nothing else: 8 bits, wrapping at 256.  The
 * wrap is why kernel/load.c measures the AWAKE gap and not the halt - an awake
 * gap is a few thousand cycles, about 21 counts, while a halt ends at the next
 * tick boundary and can be half a tick long, which in counts is up to 120 and
 * depends on the phase it started in.  See MUZIX_CTC_CTRL_CYCLE in
 * platform/zeta-v2/tick.s. */
static uint8_t g_host_cycles;

uint8_t muzix_tick_cycles(void)
{
    return g_host_cycles;
}

/* Move the counter, as the hardware would over some elapsed time.  `counts` is
 * CTC channel 3 counts; muzix_host_tick_advance takes ticks, and one tick is 240
 * of these. */
void host_cycle_advance(uint8_t counts)
{
    g_host_cycles = (uint8_t)(g_host_cycles + counts);
}

void host_cycle_set(uint8_t counts)
{
    g_host_cycles = counts;
}

/* The boot-time halt probe's "emit this byte per tick" switch.
 *
 * The assembly default is 0, meaning quiet, and that is the right default: the
 * ISR must not write to the UART on every tick of a working machine.  A host test
 * that reaches the probe through kernel_main.c still has to link, so the symbol
 * exists here with the same meaning and the same zero default.  A test can turn
 * it on to check that the ISR honours it, which is the same thing the probe does
 * on the board. */
uint8_t muzix_tick_probe;
