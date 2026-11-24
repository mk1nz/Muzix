/* PCF8574T LCD 1602 driver (4-bit mode) over I2C. */

#include "lcd.h"
#include "i2c.h"

/* PCF8574T is an 8-bit I2C I/O expander driving an HD44780-compatible 1602 LCD
 * in 4-bit mode. The standard "blue" I2C adapter maps:
 *   P0 -> D4, P1 -> D5, P2 -> D6, P3 -> D7
 *   P4 -> RS, P5 -> RW (tied low), P6 -> E, P7 -> Backlight
 * Each byte sent over I2C controls all 8 pins; writing a byte to the LCD
 * takes two I2C bytes (high nibble + low nibble, each with an E pulse). */

#define LCD_I2C_ADDR MUZIX_PCF8574_ADDR

/* HD44780 command codes */
#define LCD_CMD_CLEAR 0x01
#define LCD_CMD_RETURN_HOME 0x02
#define LCD_CMD_ENTRY_MODE 0x04
#define LCD_CMD_DISPLAY_CTRL 0x08
#define LCD_CMD_CURSOR_SHIFT 0x10
#define LCD_CMD_FUNCTION_SET 0x20
#define LCD_CMD_CGRAM_ADDR 0x40
#define LCD_CMD_DDRAM_ADDR 0x80

/* Entry mode flags */
#define LCD_ENTRY_RIGHT 0x00
#define LCD_ENTRY_LEFT 0x02
#define LCD_ENTRY_SHIFT_INC 0x01
#define LCD_ENTRY_SHIFT_DEC 0x00

/* Display control flags */
#define LCD_DISPLAY_ON 0x04
#define LCD_DISPLAY_OFF 0x00
#define LCD_CURSOR_ON 0x02
#define LCD_CURSOR_OFF 0x00
#define LCD_BLINK_ON 0x01
#define LCD_BLINK_OFF 0x00

/* Function set flags */
#define LCD_FUNCTION_8BIT 0x10
#define LCD_FUNCTION_4BIT 0x00
#define LCD_FUNCTION_2LINES 0x08
#define LCD_FUNCTION_1LINE 0x00
#define LCD_FUNCTION_5x10 0x04
#define LCD_FUNCTION_5x8 0x00

/* Cursor/shift flags */
#define LCD_DISPLAY_MOVE 0x08
#define LCD_CURSOR_MOVE 0x00
#define LCD_MOVE_RIGHT 0x04
#define LCD_MOVE_LEFT 0x00

static uint8_t g_lcd_data[3];
static int g_lcd_initialized = 0;

/* Write one nibble to LCD with E pulse. */
static int
lcd_write_nibble(uint8_t nibble, uint8_t rs, uint8_t backlight) {
    uint8_t base = (uint8_t)((nibble & 0x0Fu) << 4 |
                             (rs != 0u ? PCF_RS : 0u) |
                             (backlight != 0u ? PCF_BACKLIGHT : 0u));
    int result;

    g_lcd_data[0] = base;                    /* E low, data set */
    g_lcd_data[1] = (uint8_t)(base | PCF_E); /* E high (latch) */
    g_lcd_data[2] = base;                    /* E low (latch) */

    result = i2c_send_bytes(LCD_I2C_ADDR, g_lcd_data, 3u);
    if (result != MUZIX_I2C_OK) return result;

    i2c_delay();
    i2c_delay();
    return MUZIX_I2C_OK;
}

/* Send a full byte to the LCD in 4-bit mode. */
static int
lcd_write_byte(uint8_t byte, uint8_t rs, uint8_t backlight) {
    uint8_t hi = (uint8_t)((byte >> 4) & 0x0Fu);
    uint8_t lo = (uint8_t)(byte & 0x0Fu);
    int result;

    result = lcd_write_nibble(hi, rs, backlight);
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_write_nibble(lo, rs, backlight);
    if (result != MUZIX_I2C_OK) return result;

    return MUZIX_I2C_OK;
}

/* Send a command to the LCD. */
int
lcd_command(uint8_t cmd) {
    return lcd_write_byte(cmd, 0u, 1u);
}

/* Send data byte to the LCD. */
int
lcd_data(uint8_t data) {
    return lcd_write_byte(data, 1u, 1u);
}

/* Initialize the LCD. */
int
lcd_init(void) {
    int result;
    int bus_result;

    /* Skip re-initialization if LCD is already initialized AND bus is healthy. */
    if (g_lcd_initialized != 0) {
        bus_result = i2c_bus_check();
        if (bus_result == MUZIX_I2C_ERR_BUS) {
            /* Bus is stuck - force re-initialization */
            g_lcd_initialized = 0;
        } else {
            return MUZIX_I2C_OK;
        }
    }

    /* Initialize I2C */
    result = i2c_init();
    if (result != MUZIX_I2C_OK) return result;

    /* Wait for LCD power-up (>15ms). i2c_delay() ~= 40us each at 4 MHz.
     * 60*15 = 900 calls = ~36ms. */
    {
        uint8_t j;
        uint8_t k;
        for (j = 0u; j < 60u; ++j) {
            for (k = 0u; k < 15u; ++k) {
                i2c_delay(); /* 40us each = 600us per outer loop */
            }
        }
    }

    /* LCD 4-bit init sequence per HD44780 datasheet */
    result = lcd_write_nibble(0x03u, 0u, 1u);
    if (result != MUZIX_I2C_OK) return result;

    {
        uint8_t j;
        uint8_t k;
        for (j = 0u; j < 20u; ++j) {
            for (k = 0u; k < 15u; ++k) {
                i2c_delay(); /* ~40us each = 600us per outer loop */
            }
        }
    }

    result = lcd_write_nibble(0x03u, 0u, 1u);
    if (result != MUZIX_I2C_OK) return result;

    {
        uint8_t k;
        for (k = 0u; k < 8u; ++k) {
            i2c_delay(); /* ~40us each = ~320us */
        }
    }

    result = lcd_write_nibble(0x03u, 0u, 1u);
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_write_nibble(0x02u, 0u, 1u);
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_command((uint8_t)(LCD_CMD_FUNCTION_SET | LCD_FUNCTION_4BIT | LCD_FUNCTION_2LINES | LCD_FUNCTION_5x8));
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_command((uint8_t)(LCD_CMD_DISPLAY_CTRL | LCD_DISPLAY_OFF));
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_command(LCD_CMD_CLEAR);
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_command((uint8_t)(LCD_CMD_ENTRY_MODE | LCD_ENTRY_LEFT));
    if (result != MUZIX_I2C_OK) return result;

    result = lcd_command((uint8_t)(LCD_CMD_DISPLAY_CTRL | LCD_DISPLAY_ON | LCD_CURSOR_OFF | LCD_BLINK_OFF));
    if (result != MUZIX_I2C_OK) return result;

    /* Clear display and home cursor */
    lcd_clear();

    g_lcd_initialized = 1;
    return MUZIX_I2C_OK;
}

/* Clear the LCD display. */
int
lcd_clear(void) {
    uint8_t k;

    lcd_command(LCD_CMD_CLEAR);
    /* HD44780 Clear Display: execution time >1.52ms.
     * i2c_delay() ~= 40us each. 100 calls = ~4ms. */
    for (k = 0u; k < 100u; ++k) {
        i2c_delay(); /* ~40us each = ~4ms */
    }
    return MUZIX_I2C_OK;
}

/* Return cursor to home position. */
int
lcd_home(void) {
    uint8_t k;

    lcd_command(LCD_CMD_RETURN_HOME);
    /* HD44780 Return Home: execution time >1.52ms.
     * i2c_delay() ~= 40us each. 100 calls = ~4ms. */
    for (k = 0u; k < 100u; ++k) {
        i2c_delay(); /* ~40us each = ~4ms */
    }
    return MUZIX_I2C_OK;
}

/* Set LCD cursor position. */
int
lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t row_offsets[4] = { 0x00u, 0x40u, 0x14u, 0x54u };
    if (row > 1u) row = 1u;
    lcd_command((uint8_t)(LCD_CMD_DDRAM_ADDR | (col + row_offsets[row])));
    return MUZIX_I2C_OK;
}

/* Print a character to the LCD. */
int
lcd_print_char(char c) {
    return lcd_data((uint8_t)c);
}

/* Print a null-terminated string to the LCD. */
int
lcd_print(const char *str) {
    int col = 0;
    int row = 0;
    const uint8_t row_offsets[2] = { 0x00u, 0x40u };
    int total = 0;

    while (*str != '\0' && total < LCD_CHARS) {
        char c = (char)*str++;

        if (col >= LCD_COLS) {
            if (row == 1) {
                break; /* Second line full - stop */
            }
            /* Wrap to second line */
            col = 0;
            row = 1;
            lcd_command((uint8_t)(LCD_CMD_DDRAM_ADDR | row_offsets[row]));
            {
                int d;
                for (d = 0; d < 10; ++d) {
                    i2c_delay();
                }
            }
        }

        if (c == '\n') {
            /* Handle explicit newlines */
            col = 0;
            row = 1;
            lcd_command((uint8_t)(LCD_CMD_DDRAM_ADDR | row_offsets[row]));
            {
                int d;
                for (d = 0; d < 10; ++d) {
                    i2c_delay();
                }
            }
            continue;
        }

        lcd_data((uint8_t)c);
        ++col;
        ++total;
        i2c_delay(); /* HD44780 processing time per character */
    }
    return MUZIX_I2C_OK;
}

/* Print an unsigned integer to the LCD. */
void
lcd_print_uint(unsigned int value) {
    lcd_print_uint_padded(value, 0);
}

/* Print a signed integer to the LCD. */
void
lcd_print_int(int value) {
    lcd_print_int_padded(value, 0);
}

/* Print a signed integer with padding. */
void
lcd_print_int_padded(int value, int width) {
    unsigned int uval;
    int neg = 0;
    char buf[12];
    int i = 0;

    if (value < 0) {
        neg = 1;
        uval = (unsigned int)(-value);
    } else {
        uval = (unsigned int)value;
    }

    if (uval == 0u) {
        buf[i++] = '0';
    } else {
        while (uval > 0u) {
            unsigned int q = 0u;
            unsigned int v = uval;
            while (v >= 10u) {
                v -= 10u;
                ++q;
            }
            buf[i++] = (char)('0' + (int)v);
            uval = q;
        }
    }

    if (neg != 0) {
        while (i + (neg != 0 ? 1 : 0) < width) {
            buf[i++] = ' ';
        }
        if (width > 0 && i + 1 >= width) buf[i++] = '-'; else lcd_print_char('-');
    } else {
        while (i < width) {
            buf[i++] = '0';
        }
    }

    while (i > 0) {
        lcd_print_char(buf[--i]);
    }
}

/* Print an unsigned integer with padding. */
void
lcd_print_uint_padded(unsigned int value, int width) {
    char buf[12];
    int i = 0;

    if (value == 0u) {
        buf[i++] = '0';
    } else {
        while (value > 0u) {
            unsigned int q = 0u;
            unsigned int v = value;
            while (v >= 10u) {
                v -= 10u;
                ++q;
            }
            buf[i++] = (char)('0' + (int)v);
            value = q;
        }
    }

    while (i < width) {
        buf[i++] = '0';
    }

    while (i > 0) {
        lcd_print_char(buf[--i]);
    }
}
