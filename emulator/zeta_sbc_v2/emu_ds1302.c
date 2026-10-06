/**
 * Zeta SBC V2 Emulator - DS1302 RTC Support
 * 
 * This file implements the Maxim DS1302 Real-Time Clock emulation
 * with 3-wire serial interface.
 * 
 * Hardware Reference:
 * - Port: $70 (RTC latch register)
 * - Signals: CE, CLK, I/O (bit-banged)
 * 
 * DS1302 Register Map (BCD format):
 * | Addr (R/W) | Function | Range |
 * |------------|----------|-------|
 * | 80h/81h | Seconds (CH bit) | 00-59 |
 * | 82h/83h | Minutes | 00-59 |
 * | 84h/85h | Hours (TF, PM) | 1-12/0-23 |
 * | 86h/87h | Date | 1-31 |
 * | 88h/89h | Month | 1-12 |
 * | 8Ah/8Bh | Day of Week | 1-7 |
 * | 8Ch/8Dh | Year | 00-99 |
 * | 8Eh/8Fh | Write Protect (WP) | - |
 * | 90h/91h | Trickle Charge | - |
 * 
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "emulator.h"

/* ============================================================================
 * DS1302 Constants
 * ============================================================================ */

/* Port address */
#define DS1302_PORT            0x70

/* Latch register bits (DSRTC_STD mode) */
#define DSRTC_DATA             0x80    /* Bit 7 - Data output */
#define DSRTC_CLK              0x40    /* Bit 6 - Clock */
#define DSRTC_RD               0x20    /* Bit 5 - Read direction (~WE) */
#define DSRTC_CE               0x10    /* Bit 4 - Chip enable */
#define DSRTC_IN               0x01    /* Bit 0 - Data input */

/* RTC latch shadow */
#define DSRTC_MASK             0xF0    /* Mask for bits we own */
#define DSRTC_IDLE             0x20    /* Quiescent state (~WE high) */

/* Command byte bits */
#define CMD_RAM                0x40    /* RAM vs clock */
#define CMD_WRITE              0x00    /* Write command bit */
#define CMD_READ               0x01    /* Read command bit */

/* Register addresses */
#define REG_SEC                0x80    /* Seconds (read) */
#define REG_SEC_W              0x81    /* Seconds (write) */
#define REG_MIN                0x82    /* Minutes */
#define REG_MIN_W              0x83    /* Minutes (write) */
#define REG_HOUR               0x84    /* Hours */
#define REG_HOUR_W             0x85    /* Hours (write) */
#define REG_DATE               0x86    /* Date */
#define REG_DATE_W             0x87    /* Date (write) */
#define REG_MONTH              0x88    /* Month */
#define REG_MONTH_W            0x89    /* Month (write) */
#define REG_DOW                0x8A    /* Day of week */
#define REG_DOW_W              0x8B    /* Day of week (write) */
#define REG_YEAR               0x8C    /* Year */
#define REG_YEAR_W             0x8D    /* Year (write) */
#define REG_WP                 0x8E    /* Write protect */
#define REG_WP_W               0x8F    /* Write protect (write) */
#define REG_TCS                0x90    /* Trickle charge */
#define REG_CLOCK_BURST        0xBE    /* Clock burst read */
#define REG_CLOCK_BURST_W      0xBE    /* Clock burst write */
#define REG_RAM_BURST          0xFE    /* RAM burst read */
#define REG_RAM_BURST_W        0xFF    /* RAM burst write */

/* Clock halt bit */
#define CH_MASK                0x80    /* Clock halt in seconds register */

/* Hour register layout, as on the silicon:
 *
 *   bit 7    TF      time-fail
 *   bit 6    24/12   1 = 24-hour mode
 *   bit 5    PM      afternoon, 12-hour mode only
 *   bits 4:0 hour,   BCD
 *
 * In 24-hour mode bit 5 is NOT a flag: it is the tens-of-2 of the BCD hour, so
 * the hour is BCD in bits 5:0 and runs 0x00-0x23.  In 12-hour mode bit 5 is
 * PM and the hour is BCD in bits 4:0, running 0x01-0x12.
 *
 * That overlap is the whole defect.  The model used to hold the hour as a plain
 * 8-bit BCD with the mode bits clear, so bit 5 was always the tens-of-2 for
 * hours 20-23 and never a mode flag - yet the rollover test read it as one
 * (see bcd_inc_hour).  The chip has always had a real bit 6, so the registers
 * are now held in the chip's format and ds1302_make_hour() builds them.
 */
/* The hours register, per the datasheet's own table for 85h/84h:
 *
 *     bit 7   12/24   1 selects 12-hour mode - so 24-hour mode is a ZERO here
 *     bit 6   0       reserved
 *     bit 5   10      AM/PM in 12-hour mode, second tens of the hour (20-23)
 *     bit 4-3 10      hour tens
 *     bit 2-0 1       hour units
 *
 * This model had bit 6 as the mode and called bit 7 TF.  There is no TF on a
 * DS1302 - bit 7 is the mode select - and bit 6 is reserved and reads 0, so the
 * driver could never set it to anything the chip would agree with.  Worse, the
 * driver here and platform/zeta-v2/rtc_ds1302.c were wrong in the SAME way and
 * wrong the same way round, so the emulator agreed with the driver and a real
 * board had nowhere to disagree with it.  A model written to match the code
 * under test instead of the part cannot catch this class of fault at all. */
#define HOUR_12_24             0x80u   /* 1 = 12-hour mode, 0 = 24-hour */
#define HOUR_RESERVED          0x40u   /* reads 0; nothing sets it */
#define HOUR_PM                0x20u
#define HOUR_VALUE_24          0x3Fu   /* BCD 00-23, bit 5 included */
#define HOUR_VALUE_12          0x1Fu   /* BCD 01-12, PM above it */

/* Build a 24-hour hour register: the 12/24 bit CLEAR, no PM, BCD in bits 5:0. */
static uint8_t ds1302_make_hour(uint8_t bcd24)
{
    return (uint8_t)(bcd24 & HOUR_VALUE_24);
}

/* Write protect bit */
#define WP_MASK                0x80    /* Write protect */

/* How many registers a CLOCK burst transfer covers.
 *
 * Eight, not seven.  The clock registers in address order are seconds, minutes,
 * hours, date, month, day of week, year and write protect - eight of them -
 * and the DS1302 datasheet is explicit that a clock burst write must carry all
 * of them:
 *
 *   "Reads or writes in burst mode start with bit 0 of address 0.  When
 *    writing to the clock registers in the burst mode, the first eight
 *    registers must be written."
 *
 * Seven is what a person counts - the seven fields of a date and time - and it
 * is what platform/zeta-v2/rtc_ds1302.c used to send after command 0xBE.  The
 * eighth is write protect, which nothing in a clock wants to touch, which is
 * precisely why it is easy to leave out.
 *
 * The consequence of leaving it out is the whole point of modelling it: the
 * chip has been told to write eight registers and has been given seven, so the
 * transfer is not the one the chip was told to perform.  This model therefore
 * commits a clock burst write only when all eight bytes have arrived, and
 * discards the whole thing if chip enable falls first.  The registers keep
 * whatever they held, which is the only report a short burst can make.
 *
 * Nothing here is a judgement about the part on the board - a chip that quietly
 * accepted seven would be modelled by removing the check.  It is the datasheet's
 * own statement about the command this model implements.
 */
#define DS1302_CLOCK_BURST_REGS 8

/* ============================================================================
 * DS1302 State
 * ============================================================================ */

typedef struct {
    /* Latch register shadow */
    uint8_t latch;
    
    /* Interface state */
    bool ce_asserted;
    bool clock_high;
    bool data_output;    /* For writes */
    bool data_driven;    /* Whether we're outputting data */
    
    /* Command state */
    bool cmd_started;
    uint8_t cmd_bits;
    uint8_t current_cmd;
    
    /* Transaction state */
    bool is_write;
    bool is_ram;
    bool burst;
    uint8_t reg_addr;   /* internal id: REG_* for clock, CMD_RAM|n for RAM */
    uint8_t bit_count;
    /* True once the 8-bit command has been received, so the clock edges
     * that follow shift data rather than command bits. */
    bool data_phase;

    /* A clock burst write in progress: the bytes collected so far, and how many.
     * Held until the eighth arrives, because a burst is one transfer and only a
     * complete one is committed - see DS1302_CLOCK_BURST_REGS. */
    uint8_t burst_buf[DS1302_CLOCK_BURST_REGS];
    uint8_t burst_count;
    
    /* Clock registers (BCD format) */
    uint8_t seconds;     /* 00-59, bit 7 = clock halt */
    uint8_t minutes;     /* 00-59 */
    uint8_t hours;       /* 00-23 or 1-12 */
    uint8_t date;        /* 01-31 */
    uint8_t month;       /* 01-12 */
    uint8_t dow;         /* 01-07 */
    uint8_t year;        /* 00-99 */
    uint8_t wp;          /* Bit 7 = write protect */
    
    /* Trickle charge */
    uint8_t trickle;
    
    /* 31 bytes RAM */
    uint8_t ram[31];
    
    /* Read buffer for data input */
    uint8_t input_bit;
    bool input_valid;
    
} ds1302_state_t;

static ds1302_state_t ds1302;

/* ============================================================================
 * DS1302 Initialization
 * ============================================================================ */

void ds1302_init(void)
{
    memset(&ds1302, 0, sizeof(ds1302));
    
    /* Initialize latch to idle state */
    ds1302.latch = DSRTC_IDLE;
    ds1302.ce_asserted = false;
    ds1302.clock_high = false;
    ds1302.data_driven = false;
    ds1302.cmd_started = false;
    
    /* Initialize clock to a reasonable default time */
    /* Use current system time if available */
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    
    ds1302.seconds = ((tm->tm_sec / 10) << 4) | (tm->tm_sec % 10);
    ds1302.minutes = ((tm->tm_min / 10) << 4) | (tm->tm_min % 10);
    ds1302.hours = ds1302_make_hour((uint8_t)(((tm->tm_hour / 10) << 4) |
                                            (tm->tm_hour % 10)));
    ds1302.date = ((tm->tm_mday / 10) << 4) | (tm->tm_mday % 10);
    ds1302.month = (((tm->tm_mon + 1) / 10) << 4) | ((tm->tm_mon + 1) % 10);
    ds1302.dow = tm->tm_wday + 1;  /* Convert 0-6 to 1-7 */
    ds1302.year = (((tm->tm_year % 100) / 10) << 4) | (tm->tm_year % 10);
    
    /* Clear write protect */
    ds1302.wp = 0x00;
    
    /* Default trickle charge (1 diode, 2K resistor) */
    ds1302.trickle = 0xA5;
}

void ds1302_reset(void)
{
    memset(&ds1302, 0, sizeof(ds1302));
    
    /* Initialize latch to idle state */
    ds1302.latch = DSRTC_IDLE;
    ds1302.ce_asserted = false;
    ds1302.clock_high = false;
    ds1302.data_driven = false;
    ds1302.cmd_started = false;
    
    /* Clear write protect */
    ds1302.wp = 0x00;
    
    /* Default trickle charge (1 diode, 2K resistor) */
    ds1302.trickle = 0xA5;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * Convert binary to BCD
 */
static uint8_t bin_to_bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

/**
 * Convert BCD to binary
 */
static uint8_t bcd_to_bin(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

/* The wire command byte, as the chip defines it:
 *
 *   bit 7    0
 *   bit 6    1 = RAM, 0 = clock registers
 *   bit 5    1 = burst
 *   bit 4    0
 *   bits 3-0 register number
 *   bit 0    1 = read, 0 = write
 *
 * The REG_* constants below are the model's own internal register ids - 0x80
 * upwards, with the low bit selecting read or write - not wire commands.  The
 * two were being conflated: ds1302_write() did `reg_addr = cmd & 0x3F` and
 * passed that straight to read_clock_reg(), whose cases are all 0x80 and
 * above, so the mask could never produce a value any case matched.  Every read
 * returned the 0xFF default and every write fell through the switch and did
 * nothing.  ds1302_decode_cmd() is the translation between the two.
 */
typedef struct {
    bool is_write;
    bool is_ram;
    bool burst;
    uint8_t reg;          /* internal id: REG_* for clock, 0x40|n for RAM */
} ds1302_command_t;

/* Clock register number to internal id.
 *
 * The internal ids step by two - REG_SEC, REG_MIN, ... are 0x80, 0x82, 0x84 and
 * the _W forms are the odd id above each - so the index maps straight in, with
 * bit 0 of the command choosing the write form. */
static uint8_t ds1302_clock_reg_id(uint8_t index, bool is_write)
{
    if (index >= 8u) {
        return REG_TCS;
    }
    return (uint8_t)(REG_SEC + (uint8_t)(index * 2u) + (is_write ? 1u : 0u));
}

static void ds1302_decode_cmd(uint8_t cmd, ds1302_command_t *out)
{
    out->is_write = (cmd & CMD_READ) == 0;
    out->is_ram = (cmd & CMD_RAM) != 0;
    out->burst = (cmd & 0x20u) != 0;
    if (out->is_ram) {
        out->reg = (uint8_t)(CMD_RAM | (uint8_t)(cmd & 0x1Fu));
    } else if (out->burst) {
        /* A burst always starts at the first clock register; the address field
         * of a burst command is not meaningful.  It starts at the read form or
         * the write form to suit the direction, because the two accessors
         * switch on different forms of the same ids. */
        out->reg = out->is_write ? REG_SEC_W : REG_SEC;
    } else {
        out->reg = ds1302_clock_reg_id((uint8_t)((cmd >> 1) & 0x07u),
                                       out->is_write);
    }
}

/**
 * Read a clock register
 */
static uint8_t read_clock_reg(uint8_t addr)
{
    switch (addr) {
        case REG_SEC:     return ds1302.seconds;
        case REG_MIN:     return ds1302.minutes;
        case REG_HOUR:    return ds1302.hours;
        case REG_DATE:    return ds1302.date;
        case REG_MONTH:   return ds1302.month;
        case REG_DOW:     return ds1302.dow;
        case REG_YEAR:    return ds1302.year;
        case REG_WP:      return ds1302.wp;
        case REG_TCS:     return ds1302.trickle;
        default:          return 0xFF;
    }
}

/**
 * Write a clock register
 *
 * WRITE PROTECT IS ENFORCED HERE, AND IT WAS NOT
 * ---------------------------------------------
 * Bit 7 of the write-protect register stops every write to the clock registers
 * and to RAM, and the one register that stays writable is write protect itself
 * - a chip that could be made unwritable with no way back would be a chip that
 * stays that way.  The datasheet is explicit that this is a protection rather
 * than a hint, and FUZIX treats it as one on this same board: it reads 0x8F,
 * clears the bit if it is set, does the write, and puts it back, and its one
 * clock write opens with
 * `ds1302_write_register(0x8E, 0x00)` for exactly this reason.
 *
 * The model stored the bit and never looked at it, so it accepted a write that
 * the silicon would drop.  That is the worst direction for a model to be wrong
 * in, because the failure it hides is the one a host test cannot see and a board
 * reports as a single unhelpful line: a chip that is read correctly forever and
 * written not at all.  `date` on hardware said "the clock did not take the new
 * time" and printed the time the chip already had, and every host test passed,
 * because every host test set a chip whose protection was clear.
 */
static void write_clock_reg(uint8_t addr, uint8_t val)
{
    if ((ds1302.wp & WP_MASK) != 0 && addr != REG_WP_W) {
        return;
    }

    switch (addr) {
        case REG_SEC_W:   ds1302.seconds = val & ~CH_MASK; break;
        case REG_MIN_W:   ds1302.minutes = val; break;
        case REG_HOUR_W:  ds1302.hours = (uint8_t)(val & ~HOUR_RESERVED); break;
        case REG_DATE_W:  ds1302.date = val; break;
        case REG_MONTH_W: ds1302.month = val; break;
        case REG_DOW_W:   ds1302.dow = val; break;
        case REG_YEAR_W:  ds1302.year = val; break;
        case REG_WP_W:    ds1302.wp = val & WP_MASK; break;
        case REG_TCS:     ds1302.trickle = val; break;
    }
}

/**
 * Read RAM byte
 */
static uint8_t read_ram(uint8_t addr)
{
    if (addr < 31) {
        return ds1302.ram[addr];
    }
    return 0xFF;
}

/**
 * Write RAM byte
 *
 * Write protect covers the 31 bytes of RAM as well as the clock, which is the
 * other half of what the bit is for - it is a blanket on the chip's writable
 * storage, not a clock-only flag.
 */
static void write_ram(uint8_t addr, uint8_t val)
{
    if ((ds1302.wp & WP_MASK) != 0) {
        return;
    }

    if (addr < 31) {
        ds1302.ram[addr] = val;
    }
}

/* ============================================================================
 * 3-Wire Interface Logic
 * ============================================================================ */

/**
 * Load the register the current command selected into the shift register, for a
 * read.
 */
static uint8_t ds1302_load_read_reg(void)
{
    if (ds1302.is_ram) {
        return read_ram((uint8_t)(ds1302.reg_addr & 0x1F));
    }
    return read_clock_reg(ds1302.reg_addr);
}

/**
 * Advance to the next clock register in a burst, in whichever direction the
 * burst is going.
 *
 * The order is the order the registers appear on the chip: seconds, minutes,
 * hours, date, month, day of week, year, write protect.  It has to include
 * write protect, because that is the eighth register of a clock burst and the
 * datasheet requires all eight to be written - the walk used to step from year
 * straight to trickLe charge and skipped it, so the eighth byte of a burst
 * landed on the wrong register even in the one case that was allowed to write
 * eight.
 *
 * The internal ids step by two with the write form odd - REG_SEC is 0x80 and
 * REG_SEC_W is 0x81 - and read_clock_reg() and write_clock_reg() each switch on
 * one of the two forms.  A burst therefore has to carry the direction from one
 * register to the next, or the second byte of a burst write is handed to a
 * function with no case for the address it was given and silently dropped.
 */
static void ds1302_next_burst_reg(void)
{
    uint8_t is_write = (uint8_t)(ds1302.reg_addr & 0x01u);
    uint8_t base = (uint8_t)(ds1302.reg_addr & 0xFEu);

    switch (base) {
        case REG_SEC:    base = REG_MIN;   break;
        case REG_MIN:    base = REG_HOUR;  break;
        case REG_HOUR:   base = REG_DATE;  break;
        case REG_DATE:   base = REG_MONTH; break;
        case REG_MONTH:  base = REG_DOW;   break;
        case REG_DOW:    base = REG_YEAR;  break;
        case REG_YEAR:   base = REG_WP;    break;
        default:         base = REG_TCS;   break;
    }
    ds1302.reg_addr = (uint8_t)(base | is_write);
}

/**
 * Commit a complete clock burst write.
 *
 * The bytes were collected rather than written as they arrived, because the chip
 * only performs the transfer if all eight arrive; this is where they land.  The
 * write forms step by two from REG_SEC_W, so the buffer index gives the
 * register directly.
 */
static void ds1302_commit_burst(void)
{
    uint8_t i;

    for (i = 0; i < DS1302_CLOCK_BURST_REGS; i++) {
        write_clock_reg((uint8_t)(REG_SEC_W + (uint8_t)(i * 2u)),
                        ds1302.burst_buf[i]);
    }
    ds1302.burst_count = 0;
}

/**
 * Process a rising clock edge
 * Note: This function doesn't use ZETA_DEBUG to avoid needing zeta parameter
 *
 * The transfer is two distinct phases - an 8-bit command, then 0 or more 8-bit
 * data bytes - and they have to be told apart.  A single `bit_count` could not:
 * it was set back to 0 the moment the command byte completed, so the test that
 * decides command-versus-data (`bit_count < 8`) sent every following edge into
 * the command branch again and the data branch was unreachable.  A read
 * therefore returned whatever happened to be in current_cmd, and a write never
 * reached write_clock_reg() at all - measured on the host, a write of 0x33 to
 * the seconds register left the register at 0x45.
 *
 * `data_phase` is the flag that separates them.
 */
static void clock_rising_edge(void)
{
    if (!ds1302.cmd_started) {
        /* Not in a command yet */
        return;
    }

    if (!ds1302.data_phase) {
        /* Command byte.  Each bit arrives into bit 7 and is shifted down, so
         * the master sends it least-significant bit first. */
        ds1302.current_cmd = (uint8_t)((ds1302.current_cmd >> 1) |
                                       (ds1302.input_bit << 7));
        ds1302.bit_count++;

        if (ds1302.bit_count < 8) {
            return;
        }

        /* Command byte complete. */
        {
            ds1302_command_t c;
            ds1302_decode_cmd(ds1302.current_cmd, &c);
            ds1302.bit_count = 0;
            ds1302.is_write = c.is_write;
            ds1302.is_ram = c.is_ram;
            ds1302.burst = c.burst;
            ds1302.reg_addr = c.reg;
        }
        ds1302.data_phase = true;

        if (!ds1302.is_write) {
            /* A read.  Present the first data bit now, the way the silicon
             * does after the eighth command clock, so the master can sample it
             * before pulsing the clock again. */
            ds1302.current_cmd = ds1302_load_read_reg();
            ds1302.data_output = (uint8_t)(ds1302.current_cmd & 0x01u);
            ds1302.data_driven = true;
        }
        return;
    }

    /* Data byte. */
    if (ds1302.is_write) {
        ds1302.current_cmd = (uint8_t)((ds1302.current_cmd >> 1) |
                                       (ds1302.input_bit << 7));
        ds1302.bit_count++;

        if (ds1302.bit_count < 8) {
            return;
        }

        ds1302.bit_count = 0;
        if (ds1302.is_ram) {
            write_ram((uint8_t)(ds1302.reg_addr & 0x1F), ds1302.current_cmd);
            if (ds1302.burst && (ds1302.reg_addr & 0x1Fu) < 30u) {
                ds1302.reg_addr = (uint8_t)(ds1302.reg_addr + 1u);
            }
        } else if (ds1302.burst) {
            /* A clock burst is ONE transfer, and the chip performs it only if
             * all eight registers are written.  So the bytes are held here and
             * committed together at the end, rather than each landing as it
             * arrives: a burst cut short by chip enable leaves every register
             * as it was.  See DS1302_CLOCK_BURST_REGS.
             *
             * This is what a short burst write looks like from outside, and it
             * is the difference the host could not see before: with the byte
             * written on arrival, seven bytes after 0xBE looked exactly like
             * seven registers stored. */
            if (ds1302.burst_count < DS1302_CLOCK_BURST_REGS) {
                ds1302.burst_buf[ds1302.burst_count] = ds1302.current_cmd;
                ds1302.burst_count++;
                if (ds1302.burst_count == DS1302_CLOCK_BURST_REGS) {
                    ds1302_commit_burst();
                }
            }
            ds1302_next_burst_reg();
        } else {
            write_clock_reg(ds1302.reg_addr, ds1302.current_cmd);
        }
        return;
    }

    /* A read: shift the next bit out.
     *
     * The bit is sampled by the master BEFORE the clock edge that shifts the
     * next one in, so the advance to the next burst register happens on the
     * edge that completes the byte: the master has already taken the eighth
     * bit by then, and the first bit of the next byte is on the line for the
     * master to sample next.  Doing the advance on a following edge instead
     * made every byte after the first in a burst one bit late. */
    if (ds1302.bit_count < 8) {
        ds1302.current_cmd = (uint8_t)(ds1302.current_cmd >> 1);
        ds1302.data_output = (uint8_t)(ds1302.current_cmd & 0x01u);
        ds1302.bit_count++;

        if (ds1302.bit_count == 8 && ds1302.burst) {
            ds1302.bit_count = 0;
            if (ds1302.is_ram) {
                if ((ds1302.reg_addr & 0x1Fu) < 30u) {
                    ds1302.reg_addr = (uint8_t)(ds1302.reg_addr + 1u);
                }
            } else {
                ds1302_next_burst_reg();
            }
            ds1302.current_cmd = ds1302_load_read_reg();
            ds1302.data_output = (uint8_t)(ds1302.current_cmd & 0x01u);
        }
        return;
    }

    /* A non-burst read is finished with this byte. */
    ds1302.bit_count = 0;
}

/**
 * Process a falling clock edge
 * Note: This function doesn't use ZETA_DEBUG to avoid needing zeta parameter
 */
static void clock_falling_edge(void)
{
    /* Nothing to do.  A real DS1302 latches the master's bit on the rising edge
     * and presents the next bit for the master to sample on the falling one, so
     * there is no work for the chip here: the master samples the bit from
     * ds1302_read() after the falling edge and before the next rising one. */
}

/* ============================================================================
 * Port I/O Operations
 * ============================================================================ */

/**
 * Read from RTC port ($70)
 */
uint8_t ds1302_read(zeta_sbc_v2_t* zeta, uint8_t port)
{
    (void)port;
    uint8_t val = ds1302.latch & DSRTC_MASK;
    
    /* Add input bit if we're outputting data */
    if (ds1302.data_driven) {
        if (ds1302.data_output) {
            val |= DSRTC_DATA;
        }
    }

    /* Add data input bit (bit 0)
     *
     * This is the bit the chip drives back, and it is the bit a master reads
     * for a data transfer.  It used to be gated on `input_valid`, which is
     * cleared by the first ds1302_read() of the transfer - so only the very
     * first sampled bit of every byte came from the chip and the other seven
     * were whatever `input_bit` happened to hold.  The transfer state machine
     * now keeps `data_driven` set for the whole data phase, so that is what
     * gates the bit, and the byte comes out intact.
     *
     * `data_driven` is set when the master releases the data line (RD high) and
     * stays set until CE goes low, which is exactly the window in which the
     * chip owns the line. */
    if (ds1302.data_driven && ds1302.data_output) {
        val |= DSRTC_IN;
    }

    ZETA_DEBUG(zeta, "DS1302: Port read = 0x%02X\n", val);
    return val;
}

/**
 * Write to RTC port ($70)
 */
void ds1302_write(zeta_sbc_v2_t* zeta, uint8_t port, uint8_t val)
{
    (void)port;
    uint8_t old_latch = ds1302.latch;
    ds1302.latch = (old_latch & ~DSRTC_MASK) | (val & DSRTC_MASK);
    
    /* Check CE change
     *
     * The edge is detected from the CE bit of the value being written against
     * the CE bit last latched, not from `!old_latch`.  `old_latch` is the
     * whole previous port byte, so requiring it to be zero meant a command only
     * started when the master happened to leave every one of DATA, CLK, RD and
     * CE low on the previous write: a master that idles with the read line
     * released (0x20, the documented quiescent state) never started a command
     * at all. */
    bool ce_now = (val & DSRTC_CE) != 0;
    bool ce_before = (old_latch & DSRTC_CE) != 0;
    if (!ce_before && ce_now) {
        /* CE just went high - start new command */
        ds1302.cmd_started = true;
        ds1302.bit_count = 0;
        ds1302.current_cmd = 0;
        ds1302.data_phase = false;
        ds1302.is_write = false;
        ds1302.is_ram = false;
        ds1302.burst = false;
        ds1302.data_driven = false;
        ds1302.burst_count = 0;
        ZETA_DEBUG(zeta, "DS1302: CE high - starting command\n");
    } else if (ce_before && !ce_now) {
        /* CE just went low - end command
         *
         * This is where a transfer ends, and where one that was not completed
         * is thrown away.  A clock burst write that has not collected all eight
         * registers is discarded here, together with whatever it had gathered:
         * the chip was told to write eight and was given fewer, so no register
         * changes.  Without this a burst cut short would leave a partial
         * staging buffer that the next transfer's bytes could finish off. */
        ds1302.cmd_started = false;
        ds1302.data_phase = false;
        ds1302.data_driven = false;
        ds1302.burst_count = 0;
        ZETA_DEBUG(zeta, "DS1302: CE low - command ended\n");
    }
    
    /* Check clock edge */
    bool clk_now = (val & DSRTC_CLK) != 0;
    if (!ds1302.clock_high && clk_now) {
        /* Rising edge */
        clock_rising_edge();
    } else if (ds1302.clock_high && !clk_now) {
        /* Falling edge */
        clock_falling_edge();
    }
    ds1302.clock_high = clk_now;
    
    /* Check data direction (RD bit) */
    bool data_dir_output = (val & DSRTC_RD) == 0;
    
    /* Set input bit for next clock edge */
    if (data_dir_output) {
        ds1302.input_bit = (val & DSRTC_DATA) ? 1 : 0;
    } else {
        ds1302.input_bit = (val & DSRTC_IN) ? 1 : 0;
    }
    
    /* Track data driven state */
    if (ds1302.cmd_started && data_dir_output == 0) {
        ds1302.data_driven = true;
    }
    
    ZETA_DEBUG(zeta, "DS1302: Port write = 0x%02X (ce=%d, clk=%d, dir=%s, data=%d)\n",
            val, ce_now, clk_now, data_dir_output ? "out" : "in",
            (val & DSRTC_DATA) ? 1 : 0);
}

/* ============================================================================
 * Status Functions
 * ============================================================================ */

/**
 * Check if clock is running (not halted)
 */
bool ds1302_is_running(void)
{
    return (ds1302.seconds & CH_MASK) == 0;
}

/**
 * Get current time as BCD buffer
 */
/* ============================================================================
 * Time advance
 * ============================================================================
 *
 * The model had no way to move: ds1302_init() took the host time once and the
 * registers then stood still, so a driver reading the chip would have seen a
 * frozen clock however correct the driver was.  This advances it by one second
 * and the caller decides when a second has passed.
 *
 * Bit 7 of the seconds register is the chip's own clock-halt bit, and a real
 * DS1302 stops counting while it is set, so it is honoured here rather than
 * advancing through it.
 *
 * All fields are BCD, as they are in the chip, so the carries are decimal. */

/* Increment a BCD field, carrying at `max` (a BCD literal).
 *
 * The first version tried to carry by masking and OR-ing the tens nibble
 * - `(v & 0xF0) | 0x10` - which is wrong: 0x39 masked is already 0x30, so OR-ing
 * 0x10 left the tens at 3 instead of taking it to 4, and 39 came out as 30.
 * Nibble arithmetic is the only way to do this that cannot do that.
 *
 * The limit is checked on the value *after* the increment, not before it.
 * Testing first put the 59 -> 00 rollover out of reach: 0x59 has hi == 5,
 * which is below 9, so the code took the increment branch and never
 * compared, and 59 went to 60 instead of carrying.
 *
 * Returns 1 when the field wrapped, so the caller knows to carry upwards. */
static int bcd_inc(uint8_t* field, uint8_t max)
{
    uint8_t lo = (uint8_t)(*field & 0x0F);
    uint8_t hi = (uint8_t)((*field >> 4) & 0x0F);

    if (lo < 9) {
        lo++;
    } else {
        lo = 0;
        if (hi < 9) {
            hi++;
        } else {
            hi = 0;
        }
    }

    if ((uint8_t)((hi << 4) | lo) > max) {
        *field = (uint8_t)(*field & 0x80);       /* keep the halt bit */
        return 1;
    }
    *field = (uint8_t)((*field & 0x80) | (hi << 4) | lo);
    return 0;
}

/* Increment the hour register.
 *
 * This is bcd_inc() with the mode bits split off first, because the hour is the
 * one register whose flag bits sit inside the byte bcd_inc() would read the tens
 * nibble from.  bcd_inc() takes the high nibble as `(v >> 4) & 0x0F`, and for
 * a register holding 0x60 - 24-hour mode, hour 00, which is exactly what
 * ds1302_init() writes for midnight - that reads as 6 rather than 0.
 *
 * The limit has to come from the chip's own 24/12 bit, bit 6.  It used to come
 * from bit 5, which is not a mode flag: in 24-hour mode bit 5 is the tens-of-2
 * of the BCD hour, so every time from 20:00 to 23:59 set it and the model read
 * those hours as 12-hour.  The limit was then 0x12, so 0x20 incremented to
 * 0x21, was compared against 0x12 and wrapped - the hour carried into the date
 * at 20:00 instead of at midnight, hours 20 through 23 were unreachable, and an
 * eight-hour run from 18:11 landed at 06:11 the next day with the date already
 * advanced.
 *
 * The limit is 0x23 because that is the largest 24-hour BCD hour, and the
 * value mask is 0x3F because bit 5 is part of it in 24-hour mode.  In 12-hour
 * mode bit 5 is PM, so the mask narrows to 0x1F and the limit to 0x12, and the
 * wrap is to 01 rather than 00: twelve is the zero hour there.  The old code
 * always wrote 01, which is the 12-hour answer and wrong for 24-hour.
 *
 * The wrap preserves the PM bit in 12-hour mode, so 11:59:59 PM rolls to 01 PM
 * and passes 12 AM.  A real DS1302 does not document whether it toggles PM on
 * the 12 -> 1 transition, and the model never enters 12-hour mode by itself -
 * ds1302_init() and ds1302_set_time_bcd() both put the chip in 24-hour mode - so the
 * register image is kept stable rather than guessed at.  A driver that wants
 * 12-hour mode has to write the hour register itself. */
static int bcd_inc_hour(uint8_t* field)
{
    int is24 = ((*field & HOUR_12_24) == 0);
    /* Keep the bits that are flags in this mode, taken from the field itself:
     * a mask of what to preserve, not what to set.  Using a constant here
     * forced the TF bit on, because the constant includes it. */
    uint8_t keep = (uint8_t)(*field &
                             (is24 ? HOUR_12_24 : (HOUR_12_24 | HOUR_PM)));
    uint8_t lo = (uint8_t)(*field & 0x0F);
    uint8_t hi = (uint8_t)((*field >> 4) & (is24 ? 0x03u : 0x01u));

    if (lo < 9) {
        lo++;
    } else {
        lo = 0;
        if (hi < 9) {
            hi++;
        } else {
            hi = 0;
        }
    }

    if ((uint8_t)((hi << 4) | lo) > (is24 ? 0x23u : 0x12u)) {
        *field = (uint8_t)(keep | (is24 ? 0x00u : 0x01u));
        return 1;
    }
    *field = (uint8_t)(keep | (hi << 4) | lo);
    return 0;
}

void ds1302_advance_one_second(void)
{
    /* A real DS1302 does not count while bit 7 of the seconds register is
     * set, so neither does this. */
    if ((ds1302.seconds & CH_MASK) != 0) {
        return;
    }

    if (bcd_inc(&ds1302.seconds, 0x59) != 0) {
        if (bcd_inc(&ds1302.minutes, 0x59) != 0) {
            if (bcd_inc_hour(&ds1302.hours) != 0) {
                if (bcd_inc(&ds1302.dow, 0x07) != 0) {
                    ds1302.dow = 0x01;
                }
                if (bcd_inc(&ds1302.date, 0x31) != 0) {
                    ds1302.date = 0x01;
                    if (bcd_inc(&ds1302.month, 0x12) != 0) {
                        ds1302.month = 0x01;
                        /* bcd_inc leaves 0x99 at 0x00 without reporting a wrap,
                         * so the year is written unconditionally.  The old
                         * `!= 0 ? 0x00 : ds1302.year` tested the return value
                         * of a call whose result it then discarded, which
                         * worked only because 0x99 happened to be the one
                         * value both branches produced. */
                        bcd_inc(&ds1302.year, 0x99);
                    }
                }
            }
        }
    }
}

void ds1302_get_time_bcd(uint8_t* buf)
{
    buf[0] = ds1302.seconds;  /* Seconds */
    buf[1] = ds1302.minutes;  /* Minutes */
    buf[2] = ds1302.hours;    /* Hours */
    buf[3] = ds1302.date;     /* Date */
    buf[4] = ds1302.month;    /* Month */
    buf[5] = ds1302.dow;      /* DOW */
    buf[6] = ds1302.year;     /* Year */
}

/**
 * Set time from BCD buffer
 *
 * buf[2] is a plain BCD hour, as the function's own comment says.  The 24/12
 * mode bit is set here rather than taken from the caller so that a caller
 * passing 0x18 for 18:00 gets 24-hour mode, which is what it means.  A caller
 * that genuinely wants 12-hour mode has to use ds1302_set_time_bcd_raw(), which
 * exists for exactly that.
 */
void ds1302_set_time_bcd(uint8_t* buf)
{
    ds1302.seconds = buf[0] & ~CH_MASK;
    ds1302.minutes = buf[1];
    ds1302.hours = ds1302_make_hour(buf[2]);
    ds1302.date = buf[3];
    ds1302.month = buf[4];
    ds1302.dow = buf[5];
    ds1302.year = buf[6];
}

/**
 * Set time from a BCD buffer holding the hour register verbatim, mode bits and
 * all.  This is what the driver's own register writes amount to.
 */
void ds1302_set_time_bcd_raw(uint8_t* buf)
{
    ds1302_set_time_bcd(buf);
    ds1302.hours = buf[2];
}

void ds1302_set_write_protect(uint8_t reg)
{
    ds1302.wp = reg & WP_MASK;
}

/**
 * Get time as binary
 */
void ds1302_get_time_bin(uint8_t* sec, uint8_t* min, uint8_t* hour,
                         uint8_t* day, uint8_t* mon, uint8_t* year)
{
    *sec = bcd_to_bin(ds1302.seconds);
    *min = bcd_to_bin(ds1302.minutes);
    /* Mask the mode bit off before decoding.  The hour is the one register with
     * any: bit 7 says which mode, and bit 5 means PM in 12-hour mode and the
     * second tens of the hour in 24-hour mode, so the value mask depends on the
     * mode bit.  Bit 6 is reserved and is dropped either way. */
    *hour = bcd_to_bin((uint8_t)(ds1302.hours &
                                 ((ds1302.hours & HOUR_12_24) ? HOUR_VALUE_12
                                                               : HOUR_VALUE_24)));
    *day = bcd_to_bin(ds1302.date);
    *mon = bcd_to_bin(ds1302.month);
    *year = bcd_to_bin(ds1302.year);
}

/**
 * Set time from binary
 */
void ds1302_set_time_bin(uint8_t sec, uint8_t min, uint8_t hour,
                         uint8_t day, uint8_t mon, uint8_t year)
{
    ds1302.seconds = bin_to_bcd(sec) & ~CH_MASK;
    ds1302.minutes = bin_to_bcd(min);
    ds1302.hours = ds1302_make_hour(bin_to_bcd(hour));
    ds1302.date = bin_to_bcd(day);
    ds1302.month = bin_to_bcd(mon);
    ds1302.dow = 1;  /* Default to Sunday */
    ds1302.year = bin_to_bcd(year);
}

/**
 * Check if trickle charge is enabled
 */
bool ds1302_trickle_enabled(void)
{
    return (ds1302.trickle & 0xA0) == 0xA0;
}
