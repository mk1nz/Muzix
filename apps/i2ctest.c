/*
 * i2ctest - I2C LCD scanner with comprehensive PPI diagnostics
 *
 * Comprehensive diagnostics for the КР580ВВ55А (8255A clone) PPI and
 * PCF8574T I2C LCD adapter. Tests control word acceptance, individual
 * pin drive capability, input pull-ups, Port C upper nibble, Port B,
 * and BSR mode bit manipulation.
 */

#define MUZIX_I2C_NO_PIN_CHECK

#include "../lib/libc.h"
#include "../lib/lcd.h"
#include "../lib/i2c.h"
#include "../lib/z80_io.h"

/* Control words for testing (standard 8255A: D0 = Port C lower direction) */
#define CW_PC_LOWER_OUT    0x90  /* D0=0: PC lower OUTPUT */
#define CW_PC_LOWER_IN     0x91  /* D0=1: PC lower INPUT */
#define CW_PC_LOWER_IN_PB_IN  0x93  /* D0=1: PC lower IN, D1=1: PB in */

/* BSR mode (D7=0): D5-D3 = pin select (PC0-PC3 only), D0 = set(1)/reset(0)
 * BSR only affects Port C LOWER nibble (PC0-PC3), not upper (PC4-PC7).
 * For PC4 (SCL), use Mode 0 control word bit D3. */
#define BSR_SET(bit)   ((uint8_t)(0x01 | ((bit) << 1)))  /* D7=0, D1=bit, D0=1 */
#define BSR_RESET(bit) ((uint8_t)(0x00 | ((bit) << 1)))  /* D7=0, D1=bit, D0=0 */

/* Test address by sending a simple byte */
static int test_addr(uint8_t addr)
{
    uint8_t data = 0x00;
    int result = i2c_send_bytes(addr, &data, 1);
    return result;
}

/* Timing sweep - test different microsecond delay values */
static void timing_sweep(uint8_t addr)
{
    uint8_t delays[] = {2, 3, 4, 5, 6, 8, 10, 15, 20, 30, 50};
    int i;

    printf("Timing sweep (addr 0x%02x):\n", addr);
    lcd_init();

    for (i = 0; i < 11; i++) {
        i2c_set_delays(delays[i], delays[i]);
        int result = test_addr(addr);
        printf("test %d: delay=%dus -> %s\n", i + 1, delays[i],
               result == MUZIX_I2C_OK ? "LCD RESPOND" : "no ack");

         char c = '0' + i;
        int lcdok = lcd_print_char(c);
        if (lcdok == 0) {
            printf("  -> char on LCD screen!\r\n");
            sleep(1);
        }
        sleep(2);
    }
    i2c_set_delays(6, 5);  /* Restore default */
}

/* Print loop counts for current timing */
static void print_timing(void)
{
    uint8_t loops5 = i2c_loops_for_us(5);
    uint8_t loops6 = i2c_loops_for_us(6);
    printf("loops[5us]=%d loops[6us]=%d\n", loops5, loops6);
    printf("expected: tLOW>=4.7 tHIGH>=4.0 (loops must be >= 2)\n");
}

/*
 * Write a control word and test if Port C lower (PC0-PC3) is in output mode.
 * Returns the readback value when writing 0x00.
 */
static uint8_t test_control_word(uint8_t cw)
{
    z80_outb(MUZIX_PPI_PORT_CTRL, cw);
    i2c_settle();
    z80_outb(MUZIX_PPI_PORT_C, 0x00);
    i2c_settle();
    return z80_inb(MUZIX_PPI_PORT_C);
}

/*
 * Comprehensive PPI diagnostics for КР580ВВ55А.
 * Tests: control word acceptance, individual bit drive, pull-ups,
 * Port C upper, Port B, and BSR mode.
 */
static void ppi_diagnostics(void)
{
    uint8_t val, i;

    printf("=== PPI (КР580ВВ55А) Comprehensive Diagnostics ===\n");

    /* --- Test 1: Control word variation --- */
    printf("\n--- Test 1: Control word variation ---\n");
    printf("Writing control word, then 0x00 to Port C, reading back:\n");
    printf("  (If readback=0x00: OUTPUT mode accepted; if 0x0F: control word failed)\n");

     /* Standard 8255A: D0 = Port C lower direction (1=Input, 0=Output).
      * D2 = Group B mode (ignored here, Mode 0). */
     uint8_t test_cw[] = {0x90, 0x92, 0x91, 0x93, 0x80, 0x91};
     const char *cw_names[] = {
         "0x90 (PCl out, D0=0)",
         "0x92 (PCl out, PB in, D0=0)",
         "0x91 (PCl in, D0=1) -- the INPUT word",
         "0x93 (PCl in, PB in, D0=1)",
         "0x80 (reset/reinit)",
         "0x91 (PCl in, D0=1) -- second check same word"
     };

    for (i = 0; i < 6; i++) {
        val = test_control_word(test_cw[i]);
        printf("  CW=%s -> write 0x00 read 0x%02x\n", cw_names[i], val);
    }

    /* --- Test 2: Individual bit drive in OUTPUT mode --- */
    printf("\n--- Test 2: Individual Port C lower bit drive (OUTPUT mode) ---\n");
    printf("Writing control word 0x90, then testing each bit pattern:\n");
    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_OUTPUT);
    i2c_settle();

    uint8_t patterns[] = {0x00, 0x01, 0x02, 0x03, 0x0C, 0x0F};
    const char *p_desc[] = {
        "0x00 (PC0=0,PC1=0)",
        "0x01 (PC0=1,PC1=0)",
        "0x02 (PC0=0,PC1=1)",
        "0x03 (PC0=1,PC1=1)",
        "0x0C (PC2=1,PC3=1)",
        "0x0F (all lower high)"
    };

    for (i = 0; i < 6; i++) {
        z80_outb(MUZIX_PPI_PORT_C, patterns[i]);
        i2c_settle();
        val = z80_inb(MUZIX_PPI_PORT_C);
        printf("  write %s -> read 0x%02x  (lower=%01x)\n",
               p_desc[i], val, val & 0x0F);
    }

    /* --- Test 3: Input pull-up verification --- */
    printf("\n--- Test 3: Input pull-up verification ---\n");
    printf("Switching Port C lower to INPUT (CW=0x91), reading pin states:\n");

    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_C_LOWER_INPUT);
    i2c_settle();
    val = z80_inb(MUZIX_PPI_PORT_C);
    printf("  Port C read (lower nibble pins): 0x%01x\n", val & 0x0F);
    printf("  PC0 (SDA): %s\n", (val & 0x01) ? "HIGH (pull-up active)" : "LOW (no pull-up or slave driving low)");
    printf("  PC4 (SCL): %s\n", (val & 0x10) ? "HIGH (output, active)" : "LOW (output, inactive)");
    /* Diagnostic signature of a stuck PCF8574T slave */
    if (!(val & 0x01)) {
        printf("  -> DIAGNOSTIC: SDA LOW — slave may be stuck holding SDA!\n");
        printf("     I2C bus recovery (clock SCL 9x) was attempted by i2c_bus_check().\n");
        printf("     If SDA is still LOW after recovery, the PCF8574T or pull-up wiring\n");
        printf("     has a hardware fault.\n");
    }

    /* --- Test 4: Port C upper nibble test --- */
    printf("\n--- Test 4: Port C upper nibble (PC4=SCL, PC5-PC7) ---\n");
    printf("Writing 0x90 (PC upper=OUTPUT, PC lower=OUTPUT), testing upper bits:\n");
    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_OUTPUT);
    i2c_settle();

    uint8_t upper_patterns[] = {0x00, 0x10, 0x20, 0x40, 0x80, 0xF0};
    for (i = 0; i < 6; i++) {
        z80_outb(MUZIX_PPI_PORT_C, upper_patterns[i]);
        i2c_settle();
        val = z80_inb(MUZIX_PPI_PORT_C);
        printf("  write upper=0x%01x -> read 0x%02x  (upper=%01x)\n",
               upper_patterns[i] >> 4, val, (val & 0xF0) >> 4);
    }

    /* --- Test 5: Port B test --- */
    printf("\n--- Test 5: Port B (PB0-PB7) ---\n");
    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_OUTPUT);
    i2c_settle();
    z80_outb(MUZIX_PPI_PORT_C, (uint8_t)(MUZIX_I2C_SDA_BIT | MUZIX_I2C_SCL_BIT));
    i2c_settle();

    z80_outb(MUZIX_PPI_PORT_B, 0x00);
    val = z80_inb(MUZIX_PPI_PORT_B);
    printf("  write Port B=0x00 -> read 0x%02x\n", val);
    z80_outb(MUZIX_PPI_PORT_B, 0xFF);
    val = z80_inb(MUZIX_PPI_PORT_B);
    printf("  write Port B=0xFF -> read 0x%02x\n", val);

    /* --- Test 6: BSR mode individual bit test --- */
    printf("\n--- Test 6: BSR mode (D7=0) individual bit control ---\n");
    printf("Using BSR mode to set/reset PC0 (SDA) without direction change:\n");

    /* Ensure Port C lower is OUTPUT first (for BSR to work — BSR affects output latch) */
    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_OUTPUT);
    i2c_settle();

    /* BSR: reset PC0 (SDA=0) — bit 0, reset */
    z80_outb(MUZIX_PPI_PORT_CTRL, BSR_RESET(0));
    i2c_settle();
    val = z80_inb(MUZIX_PPI_PORT_C);
    printf("  BSR reset PC0 (SDA): Port C=%02x (PC0=%d)\n", val, val & 0x01);

    /* BSR: set PC0 (SDA=1) */
    z80_outb(MUZIX_PPI_PORT_CTRL, BSR_SET(0));
    i2c_settle();
    val = z80_inb(MUZIX_PPI_PORT_C);
    printf("  BSR set PC0 (SDA): Port C=%02x (PC0=%d)\n", val, val & 0x01);

    /* --- Test 7: BSR + Input mode for pin state reading --- */
    printf("\n--- Test 7: BSR mode with INPUT check (PC0 pull-up test) ---\n");
    printf("Setting Port C lower to INPUT via Mode 0 CW, then reading PC0 pin state:\n");
    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_C_LOWER_INPUT);
    i2c_settle();
    /* In BSR mode, individual bits of Port C output latch can be set/reset,
     * but direction is still controlled by last Mode 0 word. */
    /* BSR set PC0 — but PC0 is INPUT, so this sets the latch, not the pin */
    z80_outb(MUZIX_PPI_PORT_CTRL, BSR_SET(0));
    i2c_settle();
    val = z80_inb(MUZIX_PPI_PORT_C);
    printf("  CW=INPUT, BSR set PC0: Port C lower pins = 0x%01x (PC0=%d)\n",
           val & 0x0F, val & 0x01);
    if (val & 0x01) {
        printf("  -> PC0 reads HIGH: PCF8574T pull-up is active (SDA line OK)\n");
    } else {
        printf("  -> PC0 reads LOW: no pull-up on SDA or slave is driving low\n");
    }

    /* --- Summary --- */
    printf("\n--- PPI Diagnostic Summary ---\n");
    /* Re-test the primary control word */
    val = test_control_word(CTL_OUTPUT);
    if (val == 0x00) {
        printf("  Control word 0x%02x: ACCEPTED (Port C lower = OUTPUT)\n", CTL_OUTPUT);
    } else {
        printf("  Control word 0x%02x: REJECTED (readback=0x%02x)\n", CTL_OUTPUT, val);
        printf("  Try: CW=0x92 (PB as input, PCl still out)\n");
        val = test_control_word(0x92);
        printf("  Control word 0x96: %s (readback=0x%02x)\n",
               val == 0x00 ? "ACCEPTED" : "STILL REJECTED", val);
    }

    /* Restore to default state */
    z80_outb(MUZIX_PPI_PORT_CTRL, CTL_OUTPUT);
    i2c_settle();
    z80_outb(MUZIX_PPI_PORT_C, (uint8_t)(MUZIX_I2C_SDA_BIT | MUZIX_I2C_SCL_BIT));  /* SDA=1, SCL=1 */
    i2c_settle();
    printf("  Restored: CW=0x90, Port C=0x11\n");
}

/* PCF8574 addresses: 0x4E = 0x27<<1 (standard PCF8574 at 0x27),
 * 0x40 = 0x20<<1, 0x42 = 0x21<<1, ... up to 0x4E (PCF8574 at 0x20-0x27)
 * Also scan PCF8574A range: 0x70 = 0x38<<1, ... up to 0x7E (0x3F<<1) */
#define NUM_ADDRS 16
static uint8_t addrs[NUM_ADDRS] = {
    0x4E, 0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C,  /* PCF8574: 0x20-0x27 */
    0x70, 0x72, 0x74, 0x76, 0x78, 0x7A, 0x7C, 0x7E   /* PCF8574A: 0x38-0x3F */
};

int
main(void) {
    int i;
    int found = -1;

    printf("I2C LCD Scanner\n");
    print_timing();

    int init_result = i2c_init();
    if (init_result != MUZIX_I2C_OK) {
        printf("BUS ERROR: code %d\n", init_result);
        printf("Check pull-up resistors (4.7k) on SDA and SCL, and wiring.\n");
        /* Run diagnostics even on bus error — they reveal the root cause */
        ppi_diagnostics();
        return 1;
    }

    /* Comprehensive PPI diagnostics */
    ppi_diagnostics();

    /* Also test with explicit i2c_init() re-entry */
    printf("\n=== I2C Bus Scan ===\n");
    printf("Scanning...\n");
    for (i = 0; i < NUM_ADDRS; i++) {
        int result = test_addr(addrs[i]);

        printf("0x%x: ", addrs[i]);
        if (result == MUZIX_I2C_OK) {
            printf("RESPOND\n");
            if (found < 0) found = i;
        } else {
            printf("n/a\n");
        }
        sleep(2);
    }

    if (found >= 0) {
        printf("found at address 0x%x\n", addrs[found]);
        printf("LCD detected at this address\n");

        /* Run timing sweep on the found address */
        timing_sweep(addrs[found]);
        return 0;
    } else {
        printf("No LCD detected\n");
        printf("Running timing sweep on 0x4E (standard address)...\n");
        i2c_set_delays(2, 2);
        int r = test_addr(0x4E);
        i2c_set_delays(5, 5);
        if (r == MUZIX_I2C_OK) {
            printf("  With tLOW=2,tHIGH=2: ACK! Address 0x4E responds at fast timings.\n");
            printf("Run 'echo -d TEST' to verify LCD functionality.\n");
        } else {
            printf("  With tLOW=2,tHIGH=2: no ack (expected if timing too fast for hw)\n");
            timing_sweep(0x4E);
        }
        printf("Try: check SDA/SCL wiring (PC0=SDA, PC4=SCL) and pull-up resistors\n");
        printf("Key: SCL (PC4) MUST have a 4.7k pull-up to VCC (or be driven OUTPUT)\n");
        printf("    when Port C upper is configured as OUTPUT.\n");
        return 1;
    }
}
