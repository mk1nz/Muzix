#ifndef MUZIX_I2C_H
#define MUZIX_I2C_H

#include "ztypes.h"

#define MUZIX_I2C_OK        0
#define MUZIX_I2C_ERR_NACK  -1  /* No ACK received from slave */
#define MUZIX_I2C_ERR_BUS   -2  /* Bus error (arbitration lost) */

/* PPI port and bit definitions for I2C bit-bang using Zeta SBC V2 PPI (8255A).
 *
 * Hardware: Zeta SBC V2 with КР580ВВ55А (8255A clone) at ports 0x60-0x63
 *   PC0 = SDA (bidirectional, open-drain, pull-up to VCC)
 *   PC4 = SCL (output from master, pulled high by external resistor)
 *
 * SDA (PC0) is in Port C lower nibble, SCL (PC4) is in Port C upper nibble.
 * This separation allows independent direction control: PC0 switches between
 * OUTPUT (for driving) and INPUT (for ACK sampling), while PC4 remains OUTPUT
 * at all times so SCL stays controllable during ACK reading.
 *
 * КР580ВВ55А Mode 0 control word bit order (per standard 8255A datasheet):
 *   D7=1: I/O mode
 *   D6-D5: Group A mode (00 = Mode 0)
 *   D4: Port A direction (1=Input, 0=Output)
 *   D3: Port C upper (PC4-PC7) direction (1=Input, 0=Output)
 *   D2: Group B mode select (ignored in Mode 0)
 *   D1: Port B direction (1=Input, 0=Output)
 *   D0: Port C lower direction (1=Input, 0=Output)
 *
 * This layout follows the proven approach from Kris-Sekula's Z80+8255A I2C
 * implementation (https://github.com/Kris-Sekula/CA80), which uses PC0 for
 * SDA and PC4 for SCL with the same control word scheme.
 */
#define MUZIX_PPI_PORT_A    0x60
#define MUZIX_PPI_PORT_B    0x61
#define MUZIX_PPI_PORT_C    0x62
#define MUZIX_PPI_PORT_CTRL 0x63

/* Port C bit assignments for I2C
 * PC0 (bit 0) = SDA (Port C lower nibble)
 * PC4 (bit 4) = SCL (Port C upper nibble)
 * SCL on PC4 stays OUTPUT even when PC0 (SDA) switches to INPUT for ACK reading. */
#define MUZIX_I2C_SDA_BIT   0x01   /* PC0 */
#define MUZIX_I2C_SCL_BIT   0x10   /* PC4 */

#define MUZIX_PCF8574_ADDR_WRITE 0x4E
#define MUZIX_PCF8574_ADDR MUZIX_PCF8574_ADDR_WRITE

#define LCD_I2C_ADDR    MUZIX_PCF8574_ADDR

/* PCF8574T → LCD pin mapping (standard Chinese "blue" I2C adapter)
 * D4-D7 = P4-P7, RS = P0, RW = P1, E = P2, Backlight = P3
 */
#define PCF_RS       0x01   /* P0 */
#define PCF_RW       0x02   /* P1 (not used) */
#define PCF_E        0x04   /* P2 */
#define PCF_BACKLIGHT 0x08  /* P3 */
#define PCF_D4       0x10   /* P4 */
#define PCF_D5       0x20   /* P5 */
#define PCF_D6       0x40   /* P6 */
#define PCF_D7       0x80   /* P7 */

/* КР580ВВ55А Mode 0 control words for I2C bit-bang.
 *
 * Layout: PC0=SDA (Port C lower), PC4=SCL (Port C upper)
 *   D7=1: Mode-set (not BSR)
 *   D6-D5=00: Group A, Mode 0
 *   D4=0: Port A direction — OUTPUT (or 1=Input if needed)
 *   D3=0: Port C upper direction — OUTPUT (SCL on PC4 stays OUTPUT)
 *   D2=0: Group B, Mode 0
 *   D1=0: Port B direction — OUTPUT
 *   D0=0: Port C lower direction — OUTPUT (for SDA driving)
 *
 * For ACK reading: D0=1 switches PC0 (SDA) to INPUT while PC4 (SCL) remains
 * OUTPUT (D3=0). This is the key advantage of separating SDA and SCL across
 * different nibbles of Port C.
 *
 * 0x90 = 10010000b: PC lower OUTPUT (PC0=SDA out), PC upper OUTPUT (PC4=SCL out)
 * 0x91 = 10010001b: PC lower INPUT (PC0=SDA in for ACK), PC upper OUTPUT (PC4=SCL out)
 */
#define CTL_OUTPUT          0x90
#define CTL_C_LOWER_INPUT   0x91

int  i2c_init(void);
uint8_t i2c_get_cpu_mhz_x10(void);
int  i2c_bus_check(void);  /* Return MUZIX_I2C_ERR_BUS if SDA stuck low */
uint8_t i2c_get_bus_state(void);  /* Return bit0=SDA, bit1=SCL - for lockup detection */
int  i2c_start(void);
void i2c_stop(void);
int  i2c_write_byte(uint8_t byte);
void i2c_write_bit(uint8_t bit);
/* Address parameters are pre-shifted I2C address bytes including the R/W bit. */
int  i2c_send_addr(uint8_t address_byte);
int  i2c_send_byte(uint8_t address_byte, uint8_t data);
int  i2c_send_bytes(uint8_t address_byte, const uint8_t *data, uint8_t len);
void i2c_delay(void);
void i2c_delay_us(uint16_t us);
void i2c_set_delays(uint8_t low_us, uint8_t high_us);  /* For timing sweep */
uint16_t i2c_loops_for_us(uint16_t us);  /* For timing diagnostics */
void i2c_delay_low(void);   /* tLOW >= 4.7μs */
void i2c_delay_high(void);  /* tHIGH >= 4.0μs */
void i2c_delay_su_sta(void); /* tSU;STA >= 4.7μs */
void i2c_delay_hd_sta(void); /* tHD;STA >= 4.0us */
void i2c_delay_su_sto(void); /* tSU;STO >= 4.0μs */
void i2c_settle(void); /* 2 NOPs (~500ns) after port writes for КР580ВВ55А settling */

uint8_t i2c_read_bit(void);
uint8_t i2c_read_byte(uint8_t send_ack);

#endif /* MUZIX_I2C_H */