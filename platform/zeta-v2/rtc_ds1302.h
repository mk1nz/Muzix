#ifndef MUZIX_ZETA_RTC_DS1302_H
#define MUZIX_ZETA_RTC_DS1302_H

#include <stdint.h>

/*
 * DS1302 real-time clock, on the Zeta SBC V2.
 *
 * The chip is a three-wire serial RTC on a single 8-bit port: bit 7 is the
 * bidirectional data line, bit 6 the clock, bit 5 the read direction and bit 4
 * chip enable, and the chip drives the data bit back in bit 0.  There is no
 * data bus and no register address latched by hardware, so every register access
 * is eight command bits followed by eight data bits, all bit-banged.
 *
 * Two things about bit 5 are worth stating here, because both have been the
 * wrong way round.  First, on this board it is the control for the data pin's
 * driver and 1 RELEASES the pin - it is not a direction bit a datasheet
 * describes and the driver computes.  FUZIX calls it PIN_DATA_HIZ and `or`s it
 * onto a shadow of the port register rather than storing a direction
 * (as does FUZIX's board driver); the value 0x20 is the same bit in both
 * implementations, so the constant was never the
 * problem.  Second, and as a consequence: the port register cannot be read back,
 * so a driver that recomputes the whole byte on every pin change has no way to
 * express "leave data as it was" - and during a read that is exactly what is
 * wanted, because the data line is then the chip's.  rtc_ds1302.c keeps a shadow
 * and read-modify-writes it for that reason.
 *
 * All time and date registers are BCD, as they are on the chip.  Nothing here
 * converts: muzix_rtc_time_t is binary because that is what a caller wants, and
 * the conversion happens once, on the way in and out.
 */

/* The port and its bits. */
#define MUZIX_DS1302_PORT   0x70u
#define MUZIX_DS1302_DATA   0x80u
#define MUZIX_DS1302_CLK    0x40u
#define MUZIX_DS1302_RD     0x20u
#define MUZIX_DS1302_CE     0x10u
#define MUZIX_DS1302_IN     0x01u

/*
 * Command byte: bit 6 selects RAM over the clock registers, bit 5 selects a
 * burst, bits 3:0 are the register and bit 0 is 1 for a read, 0 for a write.
 * So seconds is 0x00 written / 0x01 read, minutes 0x02 / 0x03, and so on up in
 * pairs, and the whole clock register block comes out in one command with 0xBF.
 *
 * Bit 7 is 1 on every clock-register command and 1 on every RAM command, and
 * together with bit 6 it is what tells the two blocks apart: 0x80 is "write
 * seconds" and 0xC0 is "write RAM 0".  FUZIX spells the same commands directly
 * - 0x8F to read write protect, 0xC0 | 2n for RAM (ds1302.c:137-143).
 */
#define MUZIX_DS1302_CMD_CLOCK 0x80u
#define MUZIX_DS1302_CMD_RAM   0x40u
#define MUZIX_DS1302_CMD_BURST 0x20u
#define MUZIX_DS1302_CMD_READ  0x01u

/* The wire command to write one clock register, which is all this driver needs
 * of the command byte: 0x80, 0x82, 0x84 ... for seconds, minutes, hours and so
 * on up to write protect at 0x8E. */
#define MUZIX_DS1302_CMD_CLOCK_WRITE(reg) \
    ((uint8_t)(MUZIX_DS1302_CMD_CLOCK | ((uint8_t)(reg) << 1)))

/* Clock register numbers (the command's bits 3:0, before the shift). */
#define MUZIX_DS1302_REG_SEC    0x00u
#define MUZIX_DS1302_REG_MIN    0x01u
#define MUZIX_DS1302_REG_HOUR   0x02u
#define MUZIX_DS1302_REG_DATE   0x03u
#define MUZIX_DS1302_REG_MONTH  0x04u
#define MUZIX_DS1302_REG_DOW    0x05u
#define MUZIX_DS1302_REG_YEAR   0x06u
#define MUZIX_DS1302_REG_WP     0x07u

/* The clock-burst commands.  0xBF reads all seven time registers out in chip
 * order in one transfer, and the driver uses it for every read.
 *
 * 0xBE - the burst WRITE - is defined and deliberately not used.  The DS1302
 * datasheet is explicit about what that transfer owes: "When writing to the
 * clock registers in the burst mode, the first eight registers must be written."
 * The clock registers are eight, not seven - seconds, minutes, hours, date,
 * month, day of week, year and write protect - and a driver that sends the
 * seven fields of a date and time has told the chip to write eight registers
 * and given it seven.  muzix_zeta_rtc_set() therefore writes one register per
 * transfer, which is also what FUZIX does; it has a burst READ and no burst
 * write at all.  See muzix_zeta_rtc_set() in rtc_ds1302.c. */
#define MUZIX_DS1302_BURST_READ  0xBFu
#define MUZIX_DS1302_BURST_WRITE 0xBEu

/* Hour register flags, above the BCD hour.
 *
 * Bit 7 is the 12/24-hour mode select, and it is INVERTED: the datasheet is
 * explicit that "bit 7 of the hours register is defined as the 12- or 24-hour
 * mode-select bit.  When high, the 12-hour mode is selected."  So 24-hour mode
 * is bit 7 LOW, and the way to ask for it is to write a zero there, not a one.
 *
 * This used to be a constant named MUZIX_DS1302_HOUR_24 whose value was 0x40,
 * with a trailing comment saying "1 = 24-hour mode".
 *
 * which is wrong twice over.  Bit 6 is not a mode bit at all - the register table
 * marks it 0 - and a 1 there means nothing to the chip.  Worse, the name said the
 * bit was SET for 24-hour mode, so the write was asking for the opposite of what
 * it meant.  It worked, and worked for a long time, for one reason only: bit 7
 * was left at 0, and bit 7 low happens to be 24-hour mode.  The 0x40 was decoration.
 *
 * The consequence was that the mode bit was dead in the read path too.  The
 * decoder asked `b[2] & 0x40`, a bit the chip never sets, so every real board
 * took the 12-hour branch - and it stayed harmless only because
 * MUZIX_RTC_HOUR_24_HOURS below is 1, which skips the 12-hour conversion and
 * lands on the same 24-hour fallback the other branch would have taken.  Both
 * branches ended at `& 0x3F`, so a mode bit that was never consulted produced the
 * right hour anyway, on the emulator and on the board.
 *
 * The BCD hour itself is bits 5-0 and the existing `& 0x3F` was always right. */
#define MUZIX_DS1302_HOUR_12_24 0x80u /* 1 = 12-hour mode, 0 = 24-hour */
/* Bit 6 of the hours register is reserved and reads as 0.  Nothing sets it. */
#define MUZIX_DS1302_HOUR_RESERVED 0x40u
/* Whether this board's clock is in 24-hour mode, for the case where the mode
 * bit is not set.  The bit is the only real discriminator and this board does
 * not set it: it returns 0x20 for 20:49 and 0x10 for 10:34, both 24-hour BCD
 * with the mode bit clear.
 *
 * And it cannot be inferred from the byte either.  0x21 is 21:00 in 24-hour and
 * 1 PM in 12-hour, 0x20 is 20:00 or 12 PM; for every value from 20 to 23 the
 * two readings are both valid, and nothing in the register says which is meant.
 * So the mode is declared here.  Flipping this macro is the whole of adapting
 * to a 12-hour board; the mode bit still wins when the chip sets it. */
#define MUZIX_RTC_HOUR_24_HOURS 1

#define MUZIX_DS1302_HOUR_PM  0x20u  /* afternoon, 12-hour mode only */

/* Seconds register bit 7: the chip's own clock halt. */
#define MUZIX_DS1302_CH_HALT  0x80u

/* Day of week, as the chip numbers them: 1 = Sunday. */
#define MUZIX_DS1302_DOW_SUN 1u
#define MUZIX_DS1302_DOW_SAT 7u

typedef struct {
    uint8_t second;   /* 0-59   */
    uint8_t minute;   /* 0-59   */
    uint8_t hour;     /* 0-23   */
    uint8_t day;      /* 1-31   */
    uint8_t month;    /* 1-12   */
    uint8_t year;     /* 0-99   */
    uint8_t weekday;  /* 1-7, Sunday = 1, as the chip counts */
} muzix_rtc_time_t;

/*
 * Read the whole time block from the chip in one burst transfer.
 *
 * Returns 0 on success, -1 if the chip's seconds register is not a BCD second,
 * or -1 if a 12-hour chip's hour register is not in the range 1-12.
 *
 * ON A REFUSED READ, `*time` HOLDS THE CHIP'S OWN BYTES, NOT A TIME.
 *
 * The seven bytes land in the struct and are converted only if the answer is
 * accepted, so a -1 leaves them as they came off the wire: BCD, unmasked, and
 * in the struct's field order.  That is deliberate and it is the reason the
 * caller should hand the struct on even when the read failed - the bytes are
 * what distinguish "no chip" from "a board in 12-hour mode" from "a wire that
 * did not do what was asked", and none of that is visible in the return value.
 * A caller that reports only the return value reports nothing usable.
 *
 * BOTH HOUR FORMATS ARE READ.  The hour register carries its own format in bit
 * 6: set, and bits 5:0 are a BCD hour 00-23; clear, and bit 5 is the PM flag and
 * bits 4:0 are a BCD hour 01-12, in which 12 means midnight or noon rather than
 * 12 o'clock.  The two have to be told apart, and the bit that separates them is
 * also the tens-of-2 of the hour in 24-hour mode, so reading it wrongly does not
 * produce a number outside 0-23 - it produces a plausible hour that is wrong by
 * twelve, which is the one failure mode a caller cannot check for.
 *
 * A 12-hour chip used to be refused outright rather than decoded.  That was right
 * in the abstract and wrong in practice: it cost date, uptime and top the clock
 * on hardware whose clock was answering correctly all along.  The decode is
 * covered over the whole table - both formats, all twenty-four readings in each,
 * plus the refused ones - by platform/zeta-v2/test_rtc_ds1302.c, which drives
 * this function over a wire rather than restating its arithmetic; a test with its
 * own copy of the conversion is what hid two of the three bugs that were in this
 * branch when it was first written.
 *
 * It fits only because the decode is a loop over the seven registers rather
 * than seven unrolled calls; see the comment in rtc_ds1302.c for the byte
 * counts.
 */
int muzix_zeta_rtc_get(muzix_rtc_time_t *time);

/*
 * Write the time registers, one register per transfer, then read the chip back
 * over `*time`, so that on return the struct holds what the chip actually says
 * rather than what it was asked to store.
 *
 * NOT one burst write.  The driver used to send command 0xBE and the seven
 * register bytes, which is the chip's "write every clock register" command; the
 * datasheet requires eight bytes for it and the eighth is write protect, so
 * that transfer is one byte short of the one the chip was told to perform and
 * nothing is stored.  On the board `date MMDDhhmmYYYY` reported that the clock
 * had not taken the new time.  See the note on MUZIX_DS1302_BURST_WRITE above.
 *
 * `time` is therefore not const.  Reading back rather than comparing is both
 * cheaper and better: comparing field by field is code the kernel has not got,
 * and the comparison belongs in the caller, which is the one that knows what it
 * was trying to do.  A caller that wants to know whether the set took keeps
 * what it asked for, calls this, and compares - which is what `date` does, and
 * is the reason the two are not the same operation.
 *
 * A caller that does not name a weekday (zero) gets Sunday, because every write
 * path here stores the day-of-week register and there is no way to leave it
 * alone.  That is the one field a set can come back different on.
 *
 * WRITE PROTECT IS CLEARED FIRST, AND LEFT CLEAR.  Bit 7 of the write-protect
 * register rejects every write to the clock registers and to RAM, and on this
 * board it is set: FUZIX treats it as a real protection on the same wiring and
 * the same chip, clearing it around its own writes, and its one clock write
 * opens by clearing the bit. A protected
 * chip reads perfectly and is written not at all, so the set comes back with
 * the old time and `date` reports that the clock did not take the new one.  The
 * clearing is the eighth transfer of the same loop, because write protect is the
 * chip's eighth clock register; it is not restored afterwards, because a system
 * that offers a way to set the clock wants a clock that can be set.
 *
 * Returns 0 on success, -1 if there was no answer from the chip.
 *
 * The read-back reaches this function's own `*time`, and the SYS_SETRTC handler
 * in kernel/syscalls.c copies that struct out to the caller, so the value `date`
 * compares against after a set is what the chip actually read back rather than
 * what it asked for.  That copy was missing when this was first written - the
 * handler passed a local of its own - and 7f4bd56 added it; the comment here
 * outlived the defect.  kernel/test_syscall_setrtc.c covers it, and reverting the
 * write-back makes that test exit 13.  Reading the clock with a separate `date`
 * is what actually tells you whether a set took.
 *
 * THE CALLER MUST SUPPLY IN-RANGE FIELDS, and this function does not check.
 * This is the honest state of it, so it is worth being exact about why rather
 * than repeating that there was no room:
 *
 *   - The kernel's stack headroom is enforced by tools/check_kernel_layout.rb,
 *     which fails the build below 1024 bytes.  The headroom is well over that,
 *     but _DATA is placed by tools/kernel_data_base.rb on a 0x100 page above the
 *     end of _CODE, so headroom moves in 256-byte steps and the next step down
 *     costs 256.  What is really available is the slack below _DATA's page, and
 *     that is tens of bytes rather than hundreds.
 *   - A range check on the seven fields measured 69 bytes - a table of maxima
 *     and one loop over the struct, which is the compact form.  That does not
 *     fit under this function's page.
 *
 * So the check is enforced in set_rtc() in lib/libc.c instead: that is where the
 * program supplying the bad value lives, and userspace has no such limit.  The
 * consequence is that a caller reaching muzix_zeta_rtc_set() other than
 * through set_rtc() - which today means only kernel/syscalls.c, serving
 * SYS_SETRTC - must range-check for itself.  An out-of-range value written here
 * is not refused by the chip: bin_to_bcd turns 60 seconds into 0x60, the write is
 * accepted, and the clock counts on through a time that was never one.
 */
int muzix_zeta_rtc_set(muzix_rtc_time_t *time);

#endif
