/*
 * echo - print arguments, optionally to LCD with -d flag
 */

#define MUZIX_I2C_NO_PIN_CHECK

#include "../lib/libc.h"
#include "../lib/lcd.h"
#include "../lib/i2c.h"

static int
lcd_display(int argc, char *argv[]) {
    int i;
    int result;

    /* Initialize LCD - lcd_init() skips re-init if already initialized */
    result = lcd_init();
    if (result != MUZIX_I2C_OK) {
        printf("LCD init failed (I2C error %d)\n", result);
        printf("Check pull-up resistors and wiring.\n");
        return 1;
    }
    lcd_clear();

    /* Build single string in buffer for proper line wrapping */
    char buf[64];
    int len = 0;
    for (i = 1; i < argc && len < 63; i++) {
        const char *arg = argv[i];
        while (*arg && len < 63) {
            buf[len++] = *arg++;
        }
        if (i < argc - 1 && len < 63) {
            buf[len++] = ' ';
        }
    }
    buf[len] = '\0';
    lcd_print(buf);
    return 0;
}

int
main(int argc, char *argv[]) {
    int i;
    int lcd_mode = 0;

    /* Check for -d flag */
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        lcd_mode = 1;
        argv++;
        argc--;
    }

    if (lcd_mode) {
        return lcd_display(argc, argv);
    }

    for (i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            putchar(' ');
        }
    }
    putchar('\n');
    return 0;
}
