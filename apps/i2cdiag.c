/*
 * i2cdiag - I2C bus diagnostics with detailed timing sweep
 *
 * Tests for stuck SDA/SCL conditions and verifies LCD communication
 * across a range of I2C delays (1us steps) to identify optimal settings.
 * For each timing, fills the entire 16x2 LCD display as a real-world test.
 */

#define MUZIX_I2C_NO_PIN_CHECK

#include "../lib/libc.h"
#include "../lib/lcd.h"
#include "../lib/i2c.h"

static uint8_t test_pattern[32];

static void
build_test_pattern(void) {
    const char *msg = "0123456789ABCDEF";
    for (int i = 0; i < 32; i++) {
        test_pattern[i] = msg[i % 16];
    }
}

static int
test_lcd_with_delay(uint8_t low_us, uint8_t high_us, int char_count) {
    i2c_set_delays(low_us, high_us);

    /* Force bus check before every test - catch lockups even after
     * lcd_init() cached success from a prior iteration */
    int result = i2c_bus_check();
    if (result != MUZIX_I2C_OK) {
        return result;
    }

    /* Initialize LCD - sends full HD44780 init sequence */
    result = lcd_init();
    if (result != MUZIX_I2C_OK) {
        return result;
    }

    lcd_clear();

    /* Fill with test pattern up to char_count */
    for (int i = 0; i < char_count; i++) {
        if (i == 16) {
            /* Move to second line */
            lcd_set_cursor(0, 1);
        }
        result = lcd_print_char(test_pattern[i]);
        if (result != MUZIX_I2C_OK) {
            return result;
        }
    }

    return MUZIX_I2C_OK;
}

/* Check for bus lockup after a failure */
static void report_bus_state(int delay, int chars, int err)
{
    uint8_t bus_state = i2c_get_bus_state();
    uint8_t sda = bus_state & 0x01;
    uint8_t scl = (bus_state >> 1) & 0x01;
    
    printf("  LOCKUP: delay=%dus chars=%d err=%d", delay, chars, err);
    printf(" SDA=%d SCL=%d\n", sda, scl);
    
    if (sda == 0 && scl == 1) {
        printf("  -> SDA stuck LOW with SCL high = busy/no-ACK slave\n");
    } else if (sda == 0 && scl == 0) {
        printf("  -> SDA LOW while SCL low = premature START\n");
    } else if (scl == 0) {
        printf("  -> SCL low = clock stretch or master timeout\n");
    } else {
        printf("  -> Bus appears fine, retry may help\n");
        i2c_bus_check();
    }
}

int
main(int argc, char *argv[]) {
    int min_us = 1;
    int max_us = 50;
    int success_count = 0;
    int fail_count = 0;
    int first_ok = -1;
    int last_ok = -1;

    build_test_pattern();

    /* Parse command-line arguments for delay range */
    if (argc >= 2) {
        min_us = argv[1][0] - '0';
        if (argv[1][1]) {
            min_us = min_us * 10 + (argv[1][1] - '0');
        }
        if (min_us < 1) min_us = 1;
        if (min_us > 100) min_us = 100;
    }
    if (argc >= 3) {
        max_us = argv[2][0] - '0';
        if (argv[2][1]) {
            max_us = max_us * 10 + (argv[2][1] - '0');
        }
        if (max_us < min_us) max_us = min_us;
        if (max_us > 100) max_us = 100;
    }

    printf("I2C/LCD Diagnostics\n");
    printf("Delay sweep: %dus..%dus (1us steps)\n", min_us, max_us);
    printf("Tests: bus check + LCD init + increasing char fill (1-32)\n\n");

    /* Initial bus state check */
    if (i2c_bus_check() == MUZIX_I2C_OK) {
        printf("SDA/SCL state: OK\n");
        printf("No stuck lines detected\n");
    } else {
        printf("SDA/SCL state: STUCK - bus recovery attempted\n");
    }
    printf("\n");

    /* Print header */
    printf("delay chars result\n");
    printf("----- ----- ------\n");

    /* Sweep I2C delays in 1us steps, testing increasing fill levels */
    for (int delay = min_us; delay <= max_us; delay++) {
        for (int chars = 1; chars <= 32; chars++) {
            int result = test_lcd_with_delay((uint8_t)delay, (uint8_t)delay, chars);
            int ok = (result == MUZIX_I2C_OK);

            if (ok) {
                printf("%5d %5d OK\n", delay, chars);
                success_count++;

                if (first_ok < 0) {
                    first_ok = delay;
                }
                last_ok = delay;
            } else {
                printf("%5d %5d FAIL (err=%d)\n", delay, chars, result);
                fail_count++;
                report_bus_state(delay, chars, result);
            }

            /* Restore default delays */
            i2c_set_delays(5, 5);
        }
    }

    /* Summary */
    printf("\n=== Summary ===\n");
    printf("Working range: %dus..%dus\n", first_ok, last_ok);
    printf("Successful: %d/%d\n", success_count, success_count + fail_count);
    printf("Failed: %d/%d\n", fail_count, success_count + fail_count);

    if (success_count > 0) {
        printf("Recommended: %dus or higher\n", first_ok);
    }

    return 0;
}
