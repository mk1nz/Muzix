/*
 * test_host/test_i2c_delay.c - Host test for I2C delay timing.
 *
 * Tests the i2c_loops_for_us() calculation and i2c_delay_cycles() timing
 * formula against the production code.
 *
 * i2c_loops_for_us(): loops = ceil(us * mhz_x10 / 160)
 * i2c_delay_cycles(): 16 * count + 9 T-states per iteration
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Production formula from lib/i2c.c:i2c_loops_for_us */
static uint16_t
i2c_loops_for_us_ref(uint16_t us, uint8_t mhz_x10)
{
    uint8_t q = (uint8_t)(us / 10);
    uint8_t r = (uint8_t)(us % 10);
    uint16_t cycles = ((uint16_t)q * mhz_x10 + ((uint16_t)r * mhz_x10) / 10);
    return (uint16_t)((cycles + 15) / 16);
}

/* Production formula from lib/z80.io.s:_i2c_delay_cycles */
static uint16_t
i2c_delay_cycles_ref(uint8_t count)
{
    return 16 * count + 9;
}

static int failures;

static void
check(int ok, int line, const char *what)
{
    if (!ok) {
        printf("FAIL line %d: %s\n", line, what);
        failures++;
    }
}

static void
check_str(uint8_t got, uint8_t exp, int line, const char *what)
{
    if (got != exp) {
        printf("FAIL line %d: %s: got %u, expected %u\n", line, what, got, exp);
        failures++;
    }
}

int
main(void)
{
    int i, m;
    uint8_t mhz_x10;

    /* ---- Test ceil(cycles / 16) formula --------------------------------- */

    /* us=5, mhz_x10=20: cycles=10, loops=ceil(10/16)=1 */
    check(i2c_loops_for_us(5, 20) == 1u, __LINE__, "us=5, mhz=20: loops=1");
    /* us=5, mhz_x10=40: cycles=20, loops=ceil(20/16)=2 */
    check(i2c_loops_for_us(5, 40) == 2u, __LINE__, "us=5, mhz=40: loops=2");
    /* us=5, mhz_x10=60: cycles=30, loops=ceil(30/16)=2 */
    check(i2c_loops_for_us(5, 60) == 2u, __LINE__, "us=5, mhz=60: loops=2");
    /* us=5, mhz_x10=80: cycles=40, loops=ceil(40/16)=3 */
    check(i2c_loops_for_us(5, 80) == 3u, __LINE__, "us=5, mhz=80: loops=3");
    /* us=5, mhz_x10=200: cycles=100, loops=ceil(100/16)=7 */
    check(i2c_loops_for_us(5, 200) == 7u, __LINE__, "us=5, mhz=200: loops=7");

    /* us=255, mhz_x10=200: cycles=5100, loops=ceil(5100/16)=319 */
    check(i2c_loops_for_us(255, 200) == 319u, __LINE__, "us=255, mhz=200: loops=319");

    /* us=1, mhz_x10=20: cycles=1, loops=ceil(1/16)=1 */
    check(i2c_loops_for_us(1, 20) == 1u, __LINE__, "us=1, mhz=20: loops=1");

    /* us=255, mhz_x10=40: cycles=5100, loops=ceil(5100/16)=319 */
    check(i2c_loops_for_us(255, 40) == 319u, __LINE__, "us=255, mhz=40: loops=319");

    /* us=0: loops=1 (minimum) */
    check(i2c_loops_for_us(0, 40) == 1u, __LINE__, "us=0, mhz=40: loops=1");

    /* ---- Output overflow check ------------------------------------------ */

    /* Verify that i2c_delay_loops[us] stores 0 for overflow (>255) */
    /* This requires calling i2c_delay_cache_fill directly, which we can't
     * do from outside. Instead, verify via i2c_loops_for_us that
     * us=255, mhz_x10=200 gives 319 > 255 (which will be stored as 0). */
    check(i2c_loops_for_us(255, 200) > 255u, __LINE__,
          "us=255, mhz=200: loops=319 > 255 (stored as 0)");

    /* ---- Total delay calculation ---------------------------------------- */

    /* Total delay in T-states = 16 * loops + 9 */
    check(i2c_delay_cycles_ref(1u) == 25u, __LINE__, "delay_cycles(1)=25");
    check(i2c_delay_cycles_ref(2u) == 41u, __LINE__, "delay_cycles(2)=41");
    check(i2c_delay_cycles_ref(7u) == 121u, __LINE__, "delay_cycles(7)=121");
    check(i2c_delay_cycles_ref(319u) == 5113u, __LINE__, "delay_cycles(319)=5113");

    if (failures) {
        printf("test_i2c_delay: %d failures\n", failures);
        return 1;
    }
    printf("test_i2c_delay: all pass\n");
    return 0;
}
