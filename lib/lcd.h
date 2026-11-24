#ifndef MUZIX_LCD_H
#define MUZIX_LCD_H

#include "ztypes.h"

#define MUZIX_LCD_OK    0
#define MUZIX_LCD_ERR  -1

/* HD44780 commands */
#define LCD_CMD_CLEAR           0x01
#define LCD_CMD_RETURN_HOME     0x02
#define LCD_CMD_ENTRY_MODE      0x04
#define LCD_CMD_DISPLAY_CTRL    0x08
#define LCD_CMD_CURSOR_SHIFT    0x10
#define LCD_CMD_FUNCTION_SET    0x20
#define LCD_CMD_SET_DDRAM_ADDR  0x80

/* Entry mode flags */
#define LCD_ENTRY_RIGHT         0x00
#define LCD_ENTRY_LEFT          0x02
#define LCD_ENTRY_SHIFT_DISPLAY 0x01

/* Display control flags */
#define LCD_DISPLAY_ON          0x04
#define LCD_DISPLAY_OFF         0x00
#define LCD_CURSOR_ON           0x02
#define LCD_CURSOR_OFF          0x00
#define LCD_BLINK_ON            0x01
#define LCD_BLINK_OFF           0x00

/* Function set flags */
#define LCD_FUNCTION_8BIT       0x10
#define LCD_FUNCTION_4BIT       0x00
#define LCD_FUNCTION_2LINES     0x08
#define LCD_FUNCTION_1LINE      0x00
#define LCD_FUNCTION_5x10       0x04
#define LCD_FUNCTION_5x8        0x00

/* LCD dimensions */
#define LCD_COLS  16
#define LCD_ROWS  2
#define LCD_CHARS (LCD_COLS * LCD_ROWS)

/* Public API */
int  lcd_init(void);
int  lcd_clear(void);
int  lcd_home(void);
int  lcd_set_cursor(uint8_t col, uint8_t row);
int  lcd_print(const char *str);
int  lcd_print_char(char c);
void lcd_print_int(int value);
void lcd_print_uint(unsigned int value);
void lcd_print_int_padded(int value, int width);
void lcd_print_uint_padded(unsigned int value, int width);
int  lcd_command(uint8_t cmd);
int  lcd_data(uint8_t data);

#endif /* MUZIX_LCD_H */
