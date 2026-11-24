/* host-test-not-portable: uses Z80 inline assembly and I2C hardware */
#include "i2c.h"
#include "z80_io.h"

/* I2C bit-bang driver for Z80 using PPI 8255A (КР580ВВ55А).
 *
 * Hardware: Zeta SBC V2 with КР580ВВ55А at ports 0x60-0x63
 *   PC0 = SDA (bidirectional, open-drain, pull-up to VCC)
 *   PC4 = SCL (output from master, pulled high by external resistor)
 *
 * SDA (PC0) is in Port C lower nibble, SCL (PC4) is in Port C upper nibble.
 * This separation allows independent direction control: PC0 switches between
 * OUTPUT (for driving) and INPUT (for ACK sampling), while PC4 remains OUTPUT.
 *
 * Timing: I2C standard mode compliant (~100kHz SCL).
 * - tLOW >= 4.7us, tHIGH >= 4.0us
 * - КР580ВВ55А settling: 2 NOPs (~500ns) after port writes (for 300ns requirement)
 */

extern void z80_outb(uint8_t port, uint8_t value);
extern uint8_t z80_inb(uint8_t port);
extern void i2c_delay_cycles(uint8_t count);

static void i2c_delay_cache_invalidate(void);

/* Cached calibrated CPU frequency in MHz x10.
 * Obtained via SYS_CPU_MHZ_X10 syscall, which reads the kernel's calibrated
 * CTC3 counts-per-tick.  Falls back to 40 (4 MHz) only when the syscall
 * returns 0 (calibration not yet complete). */
static uint8_t g_cpu_mhz_x10 = 0;

uint8_t
i2c_get_cpu_mhz_x10(void) {
    if (g_cpu_mhz_x10 == 0) {
        int32_t mhz_x10 = sys_cpu_mhz_x10();
        if (mhz_x10 < 10) {
            mhz_x10 = 40;  /* fallback: calibration not yet complete */
        }
        g_cpu_mhz_x10 = (uint8_t)mhz_x10;
        i2c_delay_cache_invalidate();
    }
    return g_cpu_mhz_x10;
}

/* Precomputed delay-loop count cache, indexed by us (0..255).
 * Entry 0 = "not computed yet" sentinel (loops is always >= 1).
 * Filled once when g_cpu_mhz_x10 is first detected; invalidated on change.
 * Overflow (>255 loops, only us=255 at mhz_x10=200) stored as 0 → slow path. */
static uint8_t g_i2c_delay_loops[256];
static uint8_t g_i2c_delay_cache_valid = 0;

static void
i2c_delay_cache_fill(uint8_t mhz_x10) {
    uint16_t us;
    for (us = 0; us < 256; us++) {
        /* loops = ceil(us * mhz_x10 / 160) = (us * mhz_x10 + 159) / 160
         * Max: us=255, mhz_x10=200 → (51000 + 159) / 160 = 319 > 255
         * Stored as 0 → slow path in i2c_delay_us(). */
        uint16_t loops = ((uint16_t)us * mhz_x10 + 159) / 160;
        if (loops == 0) loops = 1;  /* us=0 → loops=1 minimum */
        g_i2c_delay_loops[(uint8_t)us] = (uint8_t)(loops > 255 ? 0 : loops);
    }
    g_i2c_delay_cache_valid = 1;
}

static void
i2c_delay_cache_invalidate(void) {
    g_i2c_delay_cache_valid = 0;
}

/* Cached control word state */
static uint8_t g_ctl_current = 0xFF;

/* Shadow copy of Port C output latch.
 * Bit 0 = PC0 (SDA), Bit 4 = PC4 (SCL)
 * Upper nibble controls PC4-SCL, lower nibble controls PC0-SDA */
static uint8_t g_port_c_shadow = 0x11;  /* SDA=1 (bit0), SCL=1 (bit4) */

/* Parameter save slot - z80_outb clobbers HL, DE, BC */
static uint8_t g_bit_save;

/* I2C timing delays (microseconds) - standard mode compliant */
static uint8_t g_i2c_delay_low_us = 5;   /* tLOW >= 4.7us */
static uint8_t g_i2c_delay_high_us = 5;  /* tHIGH >= 4.0us */

/* Calculate delay-loop count for target microseconds.
 * cycles = us * mhz_x10 / 10, in T-states on the Z80.
 * us is uint8_t (0..255) per the function contract; all callers pass
 * uint8_t delay globals or small literal arguments (3..50).
 * us * mhz_x10 <= 255 * 200 = 51000 fits in uint16_t; the q/r
 * decomposition keeps all intermediates within uint16_t:
 *   q <= 25, r <= 9, q * mhz_x10 <= 5000, r * mhz_x10 <= 1800.
 * Each _i2c_delay_cycles() iteration = 16 T-states (measured from the
 * assembly loop in lib/z80_io.s).
 * loops = ceil(cycles / 16) = (cycles + 15) / 16.
 * At mhz_x10 = 200 and us = 255: cycles = 5100, loops = 319.
 * Return type is uint16_t because loops can exceed 255 (max 319).
 * Minimum 1 loop to guarantee a nonzero delay.
 *
 * The previous value of 112 T-states per iteration came from an
 * unverified report; the SDCC-generated volatile C loop actually costs
 * 115 T-states per iteration.  The assembly loop below is both faster
 * (16 T/iter) and predictable. */
uint16_t
i2c_loops_for_us(uint16_t us) {
    uint8_t mhz_x10 = i2c_get_cpu_mhz_x10();
    /* cycles = us * mhz_x10 / 10, computed with q/r decomposition to
     * avoid SDCC's 16-bit multiply/divide codegen reordering operands
     * (verified: the compiler emits __mulint then __divuint correctly
     * when q and r are separate uint8_t values). */
    uint8_t q = (uint8_t)(us / 10);
    uint8_t r = (uint8_t)(us % 10);
    uint16_t cycles = (uint16_t)q * mhz_x10 + ((uint16_t)r * mhz_x10) / 10;
    /* loops = ceil(cycles / 16).  Max value at us=255, mhz_x10=200:
     * ceil(5100 / 16) = 319, which exceeds uint8_t.  Return uint16_t. */
    uint16_t loops = (uint16_t)((cycles + 15) / 16);
    return loops > 0 ? loops : 1;
}

/* Set I2C timing delays (for sweep diagnostics) */
void
i2c_set_delays(uint8_t low_us, uint8_t high_us) {
    g_i2c_delay_low_us = low_us;
    g_i2c_delay_high_us = high_us;
}

/* Settling delay for КР580ВВ55А output (2 NOPs ~500ns at 4MHz,
 * for 300ns requirement). Called AFTER every port write. */
void
i2c_settle(void) {
    __asm
    nop
    nop
    __endasm;
}

/* I2C bus timing delays.
 * Uses precomputed loop-count cache (filled once at mhz_x10 detection).
 * The helper costs exactly 16 T-states per iteration and 16*N + 9 total.
 * us is uint8_t (contract); loops is uint16_t internally because at
 * us=255 and mhz_x10=200, loops = ceil(5100/16) = 319 > 255.
 * The helper takes a uint8_t count in A, so when loops > 255 we call it
 * twice: first with 255, then with the remainder (loops - 255 <= 64).
 * Cache lookup replaces ~3320 T-states of __mulint/__divuint/__moduint
 * per call with a single table read (~4 T-states). */
void
i2c_delay_us(uint16_t us) {
    if (!g_i2c_delay_cache_valid) {
        i2c_delay_cache_fill(i2c_get_cpu_mhz_x10());
    }
    uint16_t loops = g_i2c_delay_loops[us];
    if (loops == 0) {
        /* Overflow (>255 loops) or cache not filled: use slow path */
        uint16_t real_loops = i2c_loops_for_us(us);
        if (real_loops > 255) {
            i2c_delay_cycles(255);
            i2c_delay_cycles((uint8_t)(real_loops - 255));
            return;
        }
        loops = (uint8_t)real_loops;
    }
    i2c_delay_cycles(loops);
}

void
i2c_delay_low(void) { i2c_delay_us(g_i2c_delay_low_us); }

void
i2c_delay_high(void) { i2c_delay_us(g_i2c_delay_high_us); }

void
i2c_delay_su_sta(void) { i2c_delay_us(5); }    /* tSU;STA >= 4.7us */

void
i2c_delay_hd_sta(void) { i2c_delay_us(5); }    /* tHD;STA >= 4.0us */

void
i2c_delay_su_sto(void) { i2c_delay_us(5); }    /* tSU;STO >= 4.0us */

void
i2c_delay(void) { i2c_delay_us(20); }    /* ~80us per call at 4MHz */

/* Write SCL to PC4 (Port C upper nibble).
 * SCL is PC4 — it stays OUTPUT at all times.
 * We can write directly to Port C upper bits regardless of Port C lower
 * direction (INPUT for ACK sampling). The 8255A latches each nibble independently.
 * i2c_settle() after write for КР580ВВ55А output settling. */
static void
i2c_write_scl(uint8_t scl) {
    if (scl) { g_port_c_shadow |= MUZIX_I2C_SCL_BIT; } else { g_port_c_shadow &= (uint8_t)~MUZIX_I2C_SCL_BIT; }
    z80_outb(MUZIX_PPI_PORT_C, g_port_c_shadow);
    i2c_settle();
}

/* Write SDA to PC0 (Port C lower nibble bit 0).
 * Only writes when in OUTPUT mode (g_ctl_current == CTL_OUTPUT).
 * During ACK sampling, Port C lower is INPUT and SDA is driven by the slave. */
static void
i2c_write_sda(uint8_t sda) {
    if (g_ctl_current == CTL_OUTPUT) {
        if (sda) { g_port_c_shadow |= MUZIX_I2C_SDA_BIT; } else { g_port_c_shadow &= (uint8_t)~MUZIX_I2C_SDA_BIT; }
        z80_outb(MUZIX_PPI_PORT_C, g_port_c_shadow);
        i2c_settle();
    }
}

/* Switch Port C lower nibble direction (PC0-PC3).
 * PC0 = SDA. Switching lower to INPUT releases SDA for ACK sampling.
 * PC4 (SCL, upper nibble) stays OUTPUT via D3=0 in both CW.
 * CRITICAL: On 8255A/КР580ВВ55А, writing a Mode 0 control word
 * resets ALL output latches of ALL ports to 0, including Port C upper (PC4=SCL).
 * We must immediately restore the full Port C shadow to prevent SCL glitch. */
static void
i2c_set_c_lower_output(uint8_t output) {
    uint8_t ctl = output ? CTL_OUTPUT : CTL_C_LOWER_INPUT;
    z80_outb(MUZIX_PPI_PORT_CTRL, ctl);
    g_ctl_current = ctl;
    i2c_settle();
    /* Restore Port C output latch — control word write cleared ALL latches.
     * This restores both PC0 (SDA) and PC4 (SCL) to their shadow values. */
    z80_outb(MUZIX_PPI_PORT_C, g_port_c_shadow);
    if (!output) {
        /* When switching to INPUT, external pull-up must charge SDA through
         * the resistor. Settle delay needed. */
        i2c_delay_us(5);
    }
    i2c_settle();
}

/* Read SDA from PC0 by switching Port C lower to INPUT.
 * PC0 (SDA) goes to INPUT; PC4 (SCL) remains OUTPUT.
 * External pull-up keeps SDA high when slave releases it.
 * IMPORTANT: Call i2c_write_sda(1) BEFORE this to ensure no glitch
 * when switching back to OUTPUT after read. */
static uint8_t
i2c_read_sda(void) {
    uint8_t val;
    i2c_set_c_lower_output(0);     /* Switch lower to INPUT - release SDA */
    val = z80_inb(MUZIX_PPI_PORT_C);
    i2c_set_c_lower_output(1);     /* Restore lower to OUTPUT */
    return (val & MUZIX_I2C_SDA_BIT) ? 1 : 0;
}

/* Read SCL from PC4.
 * PC4 (SCL) is read-only when in input mode; PC0 (SDA) remains OUTPUT.
 * External pull-up keeps SCL high when master releases it. */
static uint8_t
i2c_read_scl(void) {
    uint8_t val;
    val = z80_inb(MUZIX_PPI_PORT_C);
    return (val & MUZIX_I2C_SCL_BIT) ? 1 : 0;
}

int
i2c_init(void) {
    /* Invalidate delay cache on (re-)initialization so that loop counts
     * are recomputed for the current CPU frequency.  Also reset the cached
     * mhz_x10 so i2c_get_cpu_mhz_x10() re-detects via syscall if the
     * frequency changed since last boot. */
    g_cpu_mhz_x10 = 0;
    i2c_delay_cache_invalidate();

    /* Setup 8255A control word: Port C lower=OUTPUT (PC0=SDA out),
     * Port C upper=OUTPUT (PC4=SCL out). Port A, Port B remain INPUT. */
    i2c_set_c_lower_output(1);  /* Mode 0: C lower=OUTPUT */
    /* Initialize: SDA=1 (released, bit0), SCL=1 (released, bit4) */
    g_port_c_shadow = MUZIX_I2C_SDA_BIT | MUZIX_I2C_SCL_BIT;  /* SDA=1, SCL=1 */
    z80_outb(MUZIX_PPI_PORT_C, g_port_c_shadow);
    i2c_settle();

    i2c_delay();

    /* Verify bus is healthy */
    return i2c_bus_check();
}

/* Check I2C bus health: verify SDA line is high when released.
 *
 * Implements standard I2C bus recovery:
 * - Check SCL high (abort if stuck low)
 * - Check SDA high (bus free)
 * - If SDA stuck low: 9 SCL pulses, checking SDA after each
 * - Progressive retry with longer delays if still stuck
 * Returns MUZIX_I2C_ERR_BUS only if SDA remains LOW after all recovery attempts. */
int
i2c_bus_check(void) {
    /* Release both lines: SDA=1, SCL=1 in OUTPUT mode */
    i2c_set_c_lower_output(1);
    i2c_write_sda(1);
    i2c_write_scl(1);
    i2c_settle();
    i2c_delay_us(10);

    uint8_t scl, sda, i, retry, d;

    /* Check SCL high first - if stuck low, abort */
    scl = i2c_read_scl();
    if (scl == 0) {
        return MUZIX_I2C_ERR_BUS;
    }

    sda = i2c_read_sda();

    /* SDA stuck LOW: attempt I2C bus recovery by clocking SCL 9 times.
     * A slave stuck mid-byte will complete its transmission and release
     * SDA when clocked. Check SDA after each pulse. */
    if (sda == 0) {
        i2c_set_c_lower_output(1);  /* Ensure OUTPUT mode for driving SCL */
        i2c_write_scl(0);
        i2c_settle();

        for (i = 0; i < 9; i++) {
            i2c_delay_us(5);
            i2c_write_scl(1);  /* Clock HIGH */
            i2c_settle();
            i2c_delay_us(5);

            /* Check if slave released SDA */
            sda = i2c_read_sda();
            if (sda) {
                /* SDA released - recovery successful */
                break;
            }

            i2c_write_scl(0);  /* Clock LOW */
            i2c_settle();
        }

        /* Final check */
        i2c_write_sda(1);
        i2c_settle();
        i2c_delay_us(50);
        sda = i2c_read_sda();
    }

    /* If still stuck, retry with progressively longer delays */
    if (sda == 0) {
        for (retry = 0; retry < 3; retry++) {
            d = (uint8_t)(50u << retry);  /* 50, 100, 200 us */
            i2c_delay_us(d);
            i2c_set_c_lower_output(1);
            i2c_write_sda(1);
            i2c_write_scl(1);
            i2c_settle();
            i2c_delay_us(d);
            sda = i2c_read_sda();
            scl = i2c_read_scl();
            if (sda && scl) break;
        }
    }

    if (sda == 0) {
        return MUZIX_I2C_ERR_BUS;
    }
    return MUZIX_I2C_OK;
}

/* Get current I2C bus state - for lockup diagnostics.
 * Returns: bit 0 = SDA state, bit 1 = SCL state */
uint8_t
i2c_get_bus_state(void) {
    uint8_t scl, sda;
    scl = (g_port_c_shadow & MUZIX_I2C_SCL_BIT) ? 1 : 0;
    sda = i2c_read_sda();
    return (sda & 0x01) | ((scl & 0x01) << 1);
}

/* I2C Start condition:
 * 1. SDA=1, SCL=1 (bus idle)
 * 2. SDA=0, SCL=1 (start: SDA falls while SCL high)
 * 3. SDA=0, SCL=0
 *
 * Timing: tSU;STA >= 4.7us (SDA must be high before SCL falls),
 *         tHD;STA >= 4.0us (SDA must be low after START). */
int
i2c_start(void) {
    i2c_write_sda(1);  /* SDA=1 (released) */
    i2c_write_scl(1);  /* SCL=1 */
    i2c_settle();
    i2c_delay_su_sta();  /* tSU;STA >= 4.7us: keep SDA high before START */
    i2c_write_sda(0);  /* SDA=0 (start condition: SDA falls while SCL high) */
    i2c_delay_hd_sta();  /* tHD;STA >= 4.0us: SDA held low after START */
    i2c_write_scl(0);  /* SCL=0 */
    i2c_settle();
    return MUZIX_I2C_OK;
}

/* I2C Stop condition:
 * 1. SDA=0, SCL=0
 * 2. SDA=0, SCL=1
 * 3. SDA=1, SCL=1 (stop)
 *
 * Timing: tSU;STO >= 4.0us (SCL must be high before SDA rises),
 *         tHIGH >= 4.0us (SCL high time). */
void
i2c_stop(void) {
    i2c_write_sda(0);  /* SDA=0 */
    i2c_write_scl(0);  /* SCL=0 */
    i2c_settle();
    i2c_write_scl(1);  /* SCL=1 */
    i2c_settle();
    i2c_delay_su_sto();  /* tSU;STO >= 4.0us: SCL high before STOP */
    i2c_write_sda(1);  /* SDA=1 (stop condition: SDA rises while SCL high) */
    i2c_settle();
}

/* Write one I2C bit.
 * I2C standard mode: tLOW >= 4.7us, tHIGH >= 4.0us */
void
i2c_write_bit(uint8_t bit) {
    i2c_write_sda(bit);
    i2c_settle();
    i2c_write_scl(1);   /* slave samples */
    i2c_delay_high();   /* tHIGH >= 4.0us */
    i2c_write_scl(0);
    i2c_delay_low();    /* tLOW >= 4.7us */
}

/* Write one byte (8 bits + ACK read) */
int
i2c_write_byte(uint8_t byte) {
    uint8_t mask, bit, i;

    for (i = 0, mask = 0x80; i < 8; i++, mask >>= 1) {
        bit = (byte & mask) ? 1 : 0;
        i2c_write_bit(bit);
    }

    /* 9th clock pulse: ACK/NACK phase.
     * Master releases SDA, slave pulls low for ACK. */
    i2c_write_sda(1);  /* Pre-set SDA=1 in latch to prevent glitch on restore */
    i2c_set_c_lower_output(0);     /* Switch lower to INPUT - release SDA */
    i2c_write_scl(1);              /* Raise SCL for ACK sampling */
    i2c_settle();
    i2c_delay_us(3);               /* Give slave time to respond (tSU;ACK) */
    /* Read SDA directly — port is already in INPUT mode from above.
     * Do NOT call i2c_read_sda() which would re-toggle direction, causing
     * unnecessary CW writes and potential SCL glitches during ACK window. */
    uint8_t port_c_val, sda;
    port_c_val = z80_inb(MUZIX_PPI_PORT_C);
    sda = (port_c_val & MUZIX_I2C_SDA_BIT) ? 1 : 0;  /* Read ACK */
    i2c_write_scl(0);              /* Lower SCL */
    i2c_set_c_lower_output(1);     /* Restore lower to OUTPUT for driving */
    /* ACK is active-low: SDA=0 → ACK received, SDA=1 → NACK */
    if (sda) return MUZIX_I2C_ERR_NACK;
    return MUZIX_I2C_OK;
}

uint8_t
i2c_read_bit(void) {
    i2c_write_scl(0);
    i2c_settle();
    i2c_set_c_lower_output(0);     /* Switch lower to INPUT - release SDA */
    i2c_write_scl(1);   /* SCL high for slave to drive bit */
    i2c_settle();
    /* Read SDA directly while in INPUT mode — no re-toggle of direction */
    uint8_t port_c_val = z80_inb(MUZIX_PPI_PORT_C);
    g_bit_save = (port_c_val & MUZIX_I2C_SDA_BIT) ? 1 : 0;
    i2c_delay_high();   /* Give slave time to stabilize */
    i2c_write_scl(0);
    i2c_set_c_lower_output(1);     /* Restore lower to OUTPUT */
    i2c_delay_low();
    return g_bit_save;
}

int
i2c_send_bytes(uint8_t address_byte, const uint8_t *data, uint8_t len) {
    uint8_t i;
    int result;

    __asm
    di
    __endasm;
    result = i2c_start();
    if (result != MUZIX_I2C_OK) goto i2c_send_bytes_done;

    result = i2c_write_byte(address_byte);
    if (result != MUZIX_I2C_OK) goto i2c_send_bytes_done;

    for (i = 0; i < len; i++) {
        result = i2c_write_byte(data[i]);
        if (result != MUZIX_I2C_OK) goto i2c_send_bytes_done;
    }

i2c_send_bytes_done:
    i2c_stop();
    __asm
    ei
    __endasm;
    return result;
}

int
i2c_send_addr(uint8_t address_byte) {
    int result;

    __asm
    di
    __endasm;
    result = i2c_start();
    if (result != MUZIX_I2C_OK) {
        i2c_stop();
        __asm
        ei
        __endasm;
        return result;
    }
    result = i2c_write_byte(address_byte);
    i2c_stop();
    __asm
    ei
    __endasm;
    return result;
}

int
i2c_send_byte(uint8_t address_byte, uint8_t data) {
    int result;

    __asm
    di
    __endasm;
    result = i2c_start();
    if (result != MUZIX_I2C_OK) {
        i2c_stop();
        __asm
        ei
        __endasm;
        return result;
    }

    result = i2c_write_byte(address_byte);
    if (result != MUZIX_I2C_OK) {
        i2c_stop();
        __asm
        ei
        __endasm;
        return result;
    }

    result = i2c_write_byte(data);
    i2c_stop();
    __asm
    ei
    __endasm;
    return result;
}

uint8_t
i2c_read_byte(uint8_t send_ack) {
    uint8_t result = 0, i;

    for (i = 0; i < 8; i++) {
        result = (uint8_t)(result << 1);
        result |= i2c_read_bit();
    }

    /* Send ACK (pull SDA low) or NACK (release SDA) */
    if (send_ack) {
        i2c_write_sda(0);  /* ACK: pull SDA low */
    } else {
        i2c_write_sda(1);  /* NACK: release SDA */
    }
    i2c_settle();
    i2c_write_scl(1);
    i2c_delay_high();
    i2c_write_scl(0);
    i2c_delay_low();
    i2c_write_sda(1);  /* Release SDA */
    i2c_settle();

    return result;
}
