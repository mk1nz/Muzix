/*
 * The one door from a host test to the emulator's DS1302 model.
 *
 * A test in kernel/ cannot include emulator/zeta_sbc_v2/emulator.h: that header
 * declares a Zeta SBC bank API (zeta_set_bank() among others) which collides
 * with the kernel's own platform headers, and including both is a hard error
 * rather than a shadow.  So the emulator header stays inside test_host/ and the
 * tests call these six functions instead.
 *
 * The model is not copied or reimplemented here - it is emulator/zeta_sbc_v2/
 * emu_ds1302.c, the same one the emulator puts on port $70, compiled into every
 * host test binary.  Two chip models would be a second thing to keep in step
 * with the silicon, and the disagreement between them would be the one worth
 * catching: the driver and the chip have to agree about bit order, about when
 * the master samples, and about what the hour register's flag bits mean, and a
 * model written to agree with the driver would agree with every mistake in it.
 *
 * Nothing in this file is in KERNEL_C_MODULES, so SDCC never sees it.
 */

#include <stdint.h>

#include "emulator.h"

/* ZETA_DEBUG() reads ->verbose_mode through this pointer and nothing else in
 * ds1302_read()/ds1302_write() touches it, so a zeroed context is a silent one,
 * which is what a test wants.  Static, because the struct carries the machine's
 * memory and has no business on the stack. */
static zeta_sbc_v2_t g_zeta;

/* The eight public model calls a test needs, and no more.  ds1302_init() is not
 * among them: it takes the host clock, and a test that wants a specific time
 * installs the seven register bytes itself. */
void muzix_host_ds1302_reset(void);
void muzix_host_ds1302_write(uint8_t port, uint8_t value);
uint8_t muzix_host_ds1302_read(uint8_t port);
void muzix_host_ds1302_set_registers(const uint8_t *registers);
void muzix_host_ds1302_get_registers(uint8_t *registers);
int muzix_host_ds1302_is_running(void);
void muzix_host_ds1302_set_write_protect(uint8_t reg);

void muzix_host_ds1302_reset(void)
{
    ds1302_reset();
}

void muzix_host_ds1302_write(uint8_t port, uint8_t value)
{
    ds1302_write(&g_zeta, port, value);
}

uint8_t muzix_host_ds1302_read(uint8_t port)
{
    return ds1302_read(&g_zeta, port);
}

/* buf is the chip's seven registers in chip order: seconds, minutes, hours, date,
 * month, day of week, year.  The hour goes in verbatim, mode bits and all -
 * ds1302_set_time_bcd_raw() is the only model entry point that does that, and it
 * is the only way to ask for a 12-hour chip, since ds1302_set_time_bcd() sets the
 * 24/12 bit itself. */
void muzix_host_ds1302_set_registers(const uint8_t *registers)
{
    uint8_t buf[7];

    for (uint8_t i = 0; i < 7u; i++) {
        buf[i] = registers[i];
    }
    ds1302_set_time_bcd_raw(buf);
}

void muzix_host_ds1302_get_registers(uint8_t *registers)
{
    ds1302_get_time_bcd(registers);
}

int muzix_host_ds1302_is_running(void)
{
    return ds1302_is_running() ? 1 : 0;
}

void muzix_host_ds1302_set_write_protect(uint8_t reg)
{
    ds1302_set_write_protect(reg);
}
