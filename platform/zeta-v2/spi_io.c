/* SPI bit-bang driver for W5100 Ethernet controller on Zeta SBC V2.
 *
 * Follows the same pattern as lib/i2c.c: uses z80_outb()/z80_inb() for port I/O
 * with shadow state in _DATA. Uses PPI (8255) Port C and Port A.
 *
 * I2C uses PC0=SDA (Port C lower nibble, PC0-PC3) and PC4=SCL (Port C upper
 * nibble, PC4-PC7). SPI uses PC5 (upper nibble) for CLK, PC6 for MOSI,
 * PC7 for CS. No conflict — PC4 (SCL) and PC5/PC6/PC7 are all upper-nibble bits.
 *
 * Pin assignment:
 *   PC5 = SPI_CLK    (0x20) — output (upper nibble, PC4-PC7)
 *   PC6 = SPI_MOSI   (0x40) — output (upper nibble, PC4-PC7)
 *   PC7 = SPI_CS_N   (0x80) — output, active low (upper nibble)
 *   PA0 = SPI_MISO   (0x01) — input (Port A)
 *
 * Control word 0x90 (Mode 0): D7=1(mode set), D4=1(Port A input for MISO),
 *   D3=0(PC upper output — CLK/MOSI/CS), D2=0, D1=0(Port B output),
 *   D0=0(PC lower output).
 * The 8255A control register is write-only; we save the known default 0x90
 * for restore rather than reading it back.
 */
/* host-test-not-portable: talks to Z80 PPI hardware via z80_outb()/z80_inb()
 * and reads the kernel's CTC-calibrated CPU frequency via
 * muzix_tick_get_cpu_mhz_x10(); neither exists on a host. */
#include "spi_io.h"
#include "z80_io.h"
#include "tick.h"
#include "../../lib/i2c.h"

#define SPI_PPI_CTRL    0x90  /* PA input (MISO), PC upper out, PB out, PC lower out */

#define SPI_CLK_BIT     0x20  /* PC5 */
#define SPI_MOSI_BIT    0x40  /* PC6 */
#define SPI_CS_BIT      0x80  /* PC7 (active low) */
#define SPI_MISO_BIT    0x01  /* PA0 (Port A) */

static uint8_t g_spi_saved_ctl;
static uint8_t g_spi_port_c;

/* SPI timing guarantee: SCLK <= 25 kHz maximum, with tHIGH >= 20 µs and
 * tLOW >= 20 µs minimum half-periods.  This is a MAXIMUM frequency bound,
 * not a fixed clock: actual SCLK varies with CPU frequency because the
 * natural bit-bang execution time is intentionally retained whenever it
 * already exceeds the 20 µs minimum half-period.
 *
 * The CPU frequency is detected at boot via SYS_CPU_MHZ_X10 (syscall 63) and
 * cached by muzix_tick_get_cpu_mhz_x10().  The target T-states per 20 µs
 * half-period is target_t = 2 * mhz_x10.  At supported CPU frequencies
 * (2, 4, 6, 8, 20 MHz), the measured natural execution between SCLK edges
 * determines whether any delay is inserted:
 *
 *   Pure bit-bang timings (T-states) from production listing of
 *   _muzix_zeta_spi_tx, excluding any delay helper insertion:
 *
 *     tLOW  (CLK LOW -> CLK HIGH):
 *       MOSI=0: 294 T   MOSI=1: 301 T   (worst case: 301 T)
 *     tHIGH (CLK HIGH -> CLK LOW):
 *       MISO=0: 265 T   MISO=1: 273 T   (worst case: 273 T)
 *
 *   At 2-8 MHz: natural half-periods already exceed 20 µs
 *     (>= 100 T-states per 20 µs at 8 MHz, scaling down at lower CPU),
 *     so ZERO delay cycles are inserted and actual SCLK is well below
 *     the 25 kHz maximum.
 *
 *   At 20 MHz: target_t = 400 T per half-period. Natural timing is
 *     below target (265-301 T), so delay cycles ARE inserted via
 *     _spi_delay_cycles() (16 T per iteration + 9 T overhead), up to
 *     9 iterations for tHIGH (MISO=0) and 7 iterations for tLOW (MOSI=0).
 *     Achieved SCLK: ~24.3-24.8 kHz, never exceeding 25 kHz.
 *
 * The delay helper is a frequency-compensating throttle only: it adds the
 * minimum cycles required to reach 20 µs per half-period, never more, and
 * never slows a naturally-slower half-period.
 */
static uint8_t g_spi_mhz_x10 = 0;
static uint16_t g_spi_target_t = 0;   /* target T-states per 20 µs half-period */

static uint8_t
spi_get_cpu_mhz_x10(void) {
    if (g_spi_mhz_x10 == 0) {
        uint8_t mhz_x10 = muzix_tick_get_cpu_mhz_x10();
        if (mhz_x10 < 10) {
            mhz_x10 = 40;  /* fallback: calibration not yet complete */
        }
        g_spi_mhz_x10 = mhz_x10;
        /* target_t = 2 × mhz_x10 T-states per 20 µs half-period */
        g_spi_target_t = (uint16_t)2 * mhz_x10;
    }
    return g_spi_mhz_x10;
}

extern void spi_delay_cycles(uint8_t count);

/* Natural pure bit-bang timings (T-states) from production listing. */
#define SPI_TLOW_PURE_MOSI0  294u
#define SPI_TLOW_PURE_MOSI1  301u
#define SPI_THIGH_PURE_MISO0 265u
#define SPI_THIGH_PURE_MISO1 273u

/* Insert delay after MOSI setup to reach the tLOW target (20 µs).
 * Called between CLK LOW (OUT #1) and CLK HIGH (OUT #3). */
static void
spi_delay_tlow(uint8_t mosi) {
    uint16_t natural = mosi ? SPI_TLOW_PURE_MOSI1 : SPI_TLOW_PURE_MOSI0;
    if (g_spi_target_t > natural) {
        uint16_t needed = g_spi_target_t - natural;
        spi_delay_cycles((uint8_t)((needed + 15) / 16));
    }
}

/* Insert delay after MISO read to reach the tHIGH target (20 µs).
 * Called between CLK HIGH (OUT #3) and next CLK LOW (OUT #4). */
static void
spi_delay_thigh(uint8_t miso) {
    uint16_t natural = miso ? SPI_THIGH_PURE_MISO1 : SPI_THIGH_PURE_MISO0;
    if (g_spi_target_t > natural) {
        uint16_t needed = g_spi_target_t - natural;
        spi_delay_cycles((uint8_t)((needed + 15) / 16));
    }
}

void
muzix_zeta_spi_init(void) {
    /* 8255A control register is write-only — cannot read it back on real hardware.
     * Save the known I2C default (0x90 = CTL_OUTPUT from lib/i2c.h) for restore.
     * The I2C driver re-sets its own control word before each operation, so the
     * restored value only matters between SPI and I2C ops. */
    g_spi_saved_ctl = 0x90;
    z80_outb(MUZIX_PPI_PORT_CTRL, SPI_PPI_CTRL);
    g_spi_port_c = SPI_CS_BIT;
    z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);
    spi_get_cpu_mhz_x10();  /* prime the cache and target_t */
}

void
muzix_zeta_spi_deinit(void) {
    z80_outb(MUZIX_PPI_PORT_CTRL, g_spi_saved_ctl);
    g_spi_port_c = MUZIX_I2C_SDA_BIT | MUZIX_I2C_SCL_BIT;
    z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);
}

void
muzix_zeta_spi_cs_low(void) {
    g_spi_port_c &= ~SPI_CS_BIT;
    z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);
}

void
muzix_zeta_spi_cs_high(void) {
    g_spi_port_c |= SPI_CS_BIT;
    g_spi_port_c &= ~SPI_CLK_BIT;
    z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);
}

uint8_t
muzix_zeta_spi_tx(uint8_t byte) {
    uint8_t received = 0;
    uint8_t i;
    uint8_t mhz_x10 = spi_get_cpu_mhz_x10();
    (void)mhz_x10;  /* target_t already computed by spi_get_cpu_mhz_x10 */

    for (i = 0; i < 8; i++) {
        /* CLK low */
        g_spi_port_c &= ~SPI_CLK_BIT;
        z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);

        /* Set MOSI from MSB of byte */
        uint8_t mosi = byte & 0x80;
        if (mosi) {
            g_spi_port_c |= SPI_MOSI_BIT;
        } else {
            g_spi_port_c &= ~SPI_MOSI_BIT;
        }
        z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);

        /* tLOW delay: from CLK LOW to CLK HIGH */
        spi_delay_tlow(mosi);

        byte <<= 1;

        /* CLK high — slave shifts MISO */
        g_spi_port_c |= SPI_CLK_BIT;
        z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);

        /* Read MISO from Port A, PA0 */
        received <<= 1;
        uint8_t miso = z80_inb(MUZIX_PPI_PORT_A) & SPI_MISO_BIT;
        if (miso) {
            received |= 0x01;
        }

        /* tHIGH delay: from CLK HIGH to next CLK LOW */
        spi_delay_thigh(miso);

        /* CLK low for next bit */
        g_spi_port_c &= ~SPI_CLK_BIT;
        z80_outb(MUZIX_PPI_PORT_C, g_spi_port_c);
    }

    return received;
}
