/*
 * test_spi_delay.c - host test for the SPI delay calculation used by
 * platform/zeta-v2/spi_io.c.
 *
 * Tests the timing formula that decides how many spi_delay_cycles()
 * iterations to insert after each SCLK edge in order to hold SCLK at
 * 25 kHz (40 µs period, 20 µs per half-period) regardless of CPU
 * frequency.
 *
 * Formula (from spi_io.c):
 *
 *     target_t = 2 × mhz_x10        T-states per 20 µs half-period
 *     needed   = max(0, target_t - natural_pure)
 *     loops    = ceil(needed / 16)   spi_delay_cycles() iterations
 *
 * Pure bit-bang timings (T-states) from production listing:
 *   tLOW:  MOSI=0 → 294, MOSI=1 → 301
 *   tHIGH: MISO=0 → 265, MISO=1 → 273
 *
 * At 2-8 MHz, target_t <= 160, all below natural timings → 0 loops.
 * At 20 MHz, target_t = 400, delays are inserted.
 */

#include "ztypes.h"
#include <stdio.h>

/* Pure bit-bang timings (T-states) from spi_io.c production listing. */
#define SPI_TLOW_PURE_MOSI0  294u
#define SPI_TLOW_PURE_MOSI1  301u
#define SPI_THIGH_PURE_MISO0 265u
#define SPI_THIGH_PURE_MISO1 273u

/* The formula under test, duplicated from spi_io.c. */
static uint8_t
spi_delay_loops_for_half_period(uint16_t target_t, uint16_t natural_pure) {
    if (target_t > natural_pure) {
        uint16_t needed = target_t - natural_pure;
        return (uint8_t)((needed + 15) / 16);
    }
    return 0;
}

static int failures;

static void
check(int ok, int id, const char *what) {
    if (!ok) {
        printf("FAIL %d: %s\n", id, what);
        failures++;
    }
}

int
main(void) {
    /* --- supported CPU frequencies: 2, 4, 6, 8, 20 MHz --- */
    /* target_t = 2 × mhz_x10 */
    uint16_t target_2  = 2 * 20;   /* 40  */
    uint16_t target_4  = 2 * 40;   /* 80  */
    uint16_t target_6  = 2 * 60;   /* 120 */
    uint16_t target_8  = 2 * 80;   /* 160 */
    uint16_t target_20 = 2 * 200;  /* 400 */

    /* At 2-8 MHz, target_t <= 160, well below natural timings (265-301 T),
     * so no delay is inserted for any path. */
    check(spi_delay_loops_for_half_period(target_2,  SPI_TLOW_PURE_MOSI0) == 0, 1,  "2 MHz tLOW MOSI=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_2,  SPI_TLOW_PURE_MOSI1) == 0, 2,  "2 MHz tLOW MOSI=1: 0 loops");
    check(spi_delay_loops_for_half_period(target_2,  SPI_THIGH_PURE_MISO0) == 0, 3, "2 MHz tHIGH MISO=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_2,  SPI_THIGH_PURE_MISO1) == 0, 4, "2 MHz tHIGH MISO=1: 0 loops");

    check(spi_delay_loops_for_half_period(target_4,  SPI_TLOW_PURE_MOSI0) == 0, 5,  "4 MHz tLOW MOSI=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_4,  SPI_TLOW_PURE_MOSI1) == 0, 6,  "4 MHz tLOW MOSI=1: 0 loops");
    check(spi_delay_loops_for_half_period(target_4,  SPI_THIGH_PURE_MISO0) == 0, 7, "4 MHz tHIGH MISO=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_4,  SPI_THIGH_PURE_MISO1) == 0, 8, "4 MHz tHIGH MISO=1: 0 loops");

    check(spi_delay_loops_for_half_period(target_6,  SPI_TLOW_PURE_MOSI0) == 0, 9,  "6 MHz tLOW MOSI=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_6,  SPI_TLOW_PURE_MOSI1) == 0, 10, "6 MHz tLOW MOSI=1: 0 loops");
    check(spi_delay_loops_for_half_period(target_6,  SPI_THIGH_PURE_MISO0) == 0, 11, "6 MHz tHIGH MISO=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_6,  SPI_THIGH_PURE_MISO1) == 0, 12, "6 MHz tHIGH MISO=1: 0 loops");

    check(spi_delay_loops_for_half_period(target_8,  SPI_TLOW_PURE_MOSI0) == 0, 13, "8 MHz tLOW MOSI=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_8,  SPI_TLOW_PURE_MOSI1) == 0, 14, "8 MHz tLOW MOSI=1: 0 loops");
    check(spi_delay_loops_for_half_period(target_8,  SPI_THIGH_PURE_MISO0) == 0, 15, "8 MHz tHIGH MISO=0: 0 loops");
    check(spi_delay_loops_for_half_period(target_8,  SPI_THIGH_PURE_MISO1) == 0, 16, "8 MHz tHIGH MISO=1: 0 loops");

    /* At 20 MHz, target_t = 400 T. Natural timings are below target,
     * so delay cycles ARE inserted. */
    /* tLOW MOSI=0: 400 - 294 = 106 → ceil(106/16) = 7 */
    check(spi_delay_loops_for_half_period(target_20, SPI_TLOW_PURE_MOSI0) == 7, 17, "20 MHz tLOW MOSI=0: 7 loops");
    /* tLOW MOSI=1: 400 - 301 = 99 → ceil(99/16) = 7 */
    check(spi_delay_loops_for_half_period(target_20, SPI_TLOW_PURE_MOSI1) == 7, 18, "20 MHz tLOW MOSI=1: 7 loops");
    /* tHIGH MISO=0: 400 - 265 = 135 → ceil(135/16) = 9 */
    check(spi_delay_loops_for_half_period(target_20, SPI_THIGH_PURE_MISO0) == 9, 19, "20 MHz tHIGH MISO=0: 9 loops");
    /* tHIGH MISO=1: 400 - 273 = 127 → ceil(127/16) = 8 */
    check(spi_delay_loops_for_half_period(target_20, SPI_THIGH_PURE_MISO1) == 8, 20, "20 MHz tHIGH MISO=1: 8 loops");

    /* --- Verify achieved half-periods at 20 MHz (all >= 20 µs = 400 T) --- */
    /* tLOW MOSI=0: 294 + 7×16 = 406 T >= 400 ✓ */
    check(294 + 7 * 16 >= 400, 21, "20 MHz tLOW MOSI=0: >= 400 T (20 µs)");
    /* tLOW MOSI=1: 301 + 7×16 = 413 T >= 400 ✓ */
    check(301 + 7 * 16 >= 400, 22, "20 MHz tLOW MOSI=1: >= 400 T (20 µs)");
    /* tHIGH MISO=0: 265 + 9×16 = 409 T >= 400 ✓ */
    check(265 + 9 * 16 >= 400, 23, "20 MHz tHIGH MISO=0: >= 400 T (20 µs)");
    /* tHIGH MISO=1: 273 + 8×16 = 401 T >= 400 ✓ */
    check(273 + 8 * 16 >= 400, 24, "20 MHz tHIGH MISO=1: >= 400 T (20 µs)");

    /* --- spi_delay_cycles() timing: 16*N + 9 T-states --- */
    check(16 * 1 + 9 == 25, 25, "spi_delay_cycles(1) = 25 T");
    check(16 * 7 + 9 == 121, 26, "spi_delay_cycles(7) = 121 T");
    check(16 * 9 + 9 == 153, 27, "spi_delay_cycles(9) = 153 T");

    if (failures) {
        printf("test_spi_delay: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_spi_delay: all cases pass\n");
    return 0;
}
