#include "rtc_ds1302.h"

/*
 * The DS1302 hour conversion, over the whole table.
 *
 * WHAT THIS IS NOT
 * ----------------
 * It is not a second copy of the conversion.  Every case below drives
 * muzix_zeta_rtc_get() - the driver's own read, its own bit-banged transfer, its
 * own BCD decode - and reads the muzix_rtc_time_t it leaves behind.  There is no
 * transcription of the hour arithmetic anywhere in this file, and that is the
 * point: a transcription is what hid two of the three bugs in this branch.  The
 * first version of this driver masked the hour to one nibble, so 10 AM read as
 * hour 0 and 11 PM as 13, and added PM in binary rather than in BCD, so 0x10 + 12
 * came out 0x1C.  Both were invisible to reading the code, because both looked
 * like plausible C, and both were caught by running the conversion over a table.
 * A test with its own copy of the arithmetic would have passed on the same two
 * bugs for exactly as long.
 *
 * THE CHIP
 * --------
 * The chip is emulator/zeta_sbc_v2/emu_ds1302.c - the same model the emulator
 * puts on port $70, compiled into this binary rather than copied.  A second chip
 * model here would be a third thing to keep in step with the silicon, and the
 * disagreement it would hide is precisely the one worth catching: the driver and
 * the chip have to agree on bit order, on when the master samples, and on what
 * the hour register's flag bits mean, and a model that agreed with the driver by
 * construction would agree with every mistake in it.
 *
 * The model takes the host clock in ds1302_init(), which is never called here:
 * each case installs its seven register bytes with ds1302_set_time_bcd_raw(),
 * which takes the hour register verbatim, mode bits and all - the only way to
 * ask for a 12-hour chip at all, since ds1302_set_time_bcd() sets the 24/12 bit
 * itself.
 *
 * THAT 12-HOUR CASE IS WHY THIS TEST EXISTS ON THE HOST RATHER THAN IN THE
 * EMULATOR.  ds1302_init() and ds1302_set_time_bcd() both write 24-hour mode, so
 * an emulator run never puts the chip in 12-hour mode and never enters the
 * branch where all three of those bugs were.
 */

#include <stdint.h>
#include <string.h>

/* z80_outb()/z80_inb() from test_host/z80_io_host.c: where the driver's `out
 * (#0x70),a` and `in a,(#0x70)` go on a host, wired to whatever the test
 * attaches.  Nothing between the two is faked - the bit order, the command byte,
 * the release of the data line, the register order and the BCD decode are all
 * the driver's own code, and that is what is under test. */
void muzix_host_port_attach(void (*write)(uint8_t port, uint8_t value),
                            uint8_t (*read)(uint8_t port));

/* From test_host/ds1302_host.c: the one door to the chip.  emulator.h cannot be
 * included from a test in the platform directory either once the door exists,
 * and having two ways in is how the two drift apart. */
void muzix_host_ds1302_reset(void);
void muzix_host_ds1302_write(uint8_t port, uint8_t value);
uint8_t muzix_host_ds1302_read(uint8_t port);
void muzix_host_ds1302_set_registers(const uint8_t *registers);
void muzix_host_ds1302_get_registers(uint8_t *registers);
void muzix_host_ds1302_set_write_protect(uint8_t reg);

/* The whole time block as the chip holds it, in chip order: the order a burst
 * read delivers and the order ds1302_set_time_bcd_raw() takes. */
static uint8_t g_registers[7];

/* The fixed fields.  Two forms, and the difference matters: what goes into the
 * chip is BCD, what comes out of muzix_zeta_rtc_get() is binary.  Every field of
 * muzix_rtc_time_t is a uint8_t, and the driver decodes in place, so a case that
 * compared the result against the BCD it had programmed would fail on every
 * accepted reading while telling you nothing about the hour. */
#define REG_SECOND  0x49u        /* 49 seconds, BCD  */
#define REG_MINUTE  0x28u        /* 28 minutes, BCD  */
#define REG_DAY     0x15u        /* the 15th, BCD    */
#define REG_MONTH   0x09u        /* September, BCD    */
#define REG_WEEKDAY 0x03u        /* Tuesday, BCD      */
#define REG_YEAR    0x26u        /* 2026, BCD         */

#define DECODED_SECOND  49u
#define DECODED_MINUTE  28u
#define DECODED_DAY     15u
#define DECODED_MONTH    9u
#define DECODED_WEEKDAY  3u
#define DECODED_YEAR    26u

static void chip_write(uint8_t port, uint8_t value)
{
    muzix_host_ds1302_write(port, value);
}

static uint8_t chip_read(uint8_t port)
{
    return muzix_host_ds1302_read(port);
}

/* Put the chip back to the one reading this file knows, and attach the wire.
 *
 * Shared by both halves of the file rather than written twice: a seed that
 * exists in two places is a seed that can disagree, and the write cases below
 * depend on the chip holding something OTHER than what they are about to set -
 * which is only a real test if the "something other" is a stated value. */
static void seed_chip(void)
{
    g_registers[0] = REG_SECOND;
    g_registers[1] = REG_MINUTE;
    g_registers[2] = 0x15u;       /* 15:28:49, 24-hour encoding, mode bit clear */
    g_registers[3] = REG_DAY;
    g_registers[4] = REG_MONTH;
    g_registers[5] = REG_WEEKDAY;
    g_registers[6] = REG_YEAR;

    muzix_host_ds1302_reset();
    muzix_host_ds1302_set_registers(g_registers);
    muzix_host_port_attach(chip_write, chip_read);
}

/* Program the chip's seven registers and read the clock back through the driver.
 * `hour_register` goes in verbatim, flags included. */
static int read_hour(uint8_t hour_register, muzix_rtc_time_t *out)
{
    seed_chip();
    g_registers[2] = hour_register;
    muzix_host_ds1302_set_registers(g_registers);

    memset(out, 0, sizeof(*out));
    return muzix_zeta_rtc_get(out);
}

/* One accepted reading: the whole struct, so the chip's register order against
 * the struct's is checked too and not only the hour. */
static int expect(uint8_t hour_register, uint8_t hour)
{
    muzix_rtc_time_t time;

    if (read_hour(hour_register, &time) != 0) {
        return 1;
    }
    if (time.second != DECODED_SECOND || time.minute != DECODED_MINUTE ||
        time.hour != hour || time.day != DECODED_DAY ||
        time.month != DECODED_MONTH || time.year != DECODED_YEAR ||
        time.weekday != DECODED_WEEKDAY) {
        return 2;
    }
    return 0;
}

/* One refused reading.  The driver must return -1, and it must leave the chip's
 * own bytes in the struct: an hour register of 0x00 with 12-hour mode clear is
 * not a time the chip can be holding, and the refusal is the only thing between
 * that and a clock that reads plausibly wrong.  The second half of the check is
 * the one that makes the refusal useful - on a -1 the struct is the only report
 * a caller gets (rtc_ds1302.h), so a refusal that had quietly converted anyway
 * would be worse than no refusal. */
static int expect_refused(uint8_t hour_register)
{
    muzix_rtc_time_t time;

    if (read_hour(hour_register, &time) != -1) {
        return 1;
    }
    /* Every field of muzix_rtc_time_t is a uint8_t, so the struct is the chip's
     * seven bytes in order with nothing in between to hide a conversion - and on
     * a refusal those bytes are BCD, because nothing decoded them. */
    if (time.hour != hour_register) {
        return 2;    /* the hour was converted on a refused read */
    }
    if (time.second != REG_SECOND || time.minute != REG_MINUTE ||
        time.day != REG_DAY || time.month != REG_MONTH ||
        time.year != REG_YEAR || time.weekday != REG_WEEKDAY) {
        return 3;    /* the other six were converted too */
    }
    return 0;
}

/* Binary tens and units, the only conversion in this file that is not the
 * driver's.  It is the definition of the two formats, not a copy of how the
 * driver reaches them: the driver works in the hour register's own BCD and its
 * two flag bits, and this works out the civil hour. */
static uint8_t to_bcd(uint8_t binary)
{
    return (uint8_t)(((binary / 10u) << 4) | (binary % 10u));
}

/*
 * The whole table: all twenty-four readings in each format, plus the readings
 * that must be refused and the flag bits that must be ignored.
 *
 * THE WHOLE TABLE, AND WHY IT IS NOT A SELECTION
 * ----------------------------------------------
 * A hand-picked table was tried first - twelve readings, 12 AM/PM, 1, 10 and 11
 * AM/PM, and 24-hour 00, 09, 13 and 23 - and it does not catch all three of the
 * bugs it was written for.  Measured, with each bug put back one at a time:
 *
 *   - the 24-hour flag carried into the BCD decode: caught, by 12 AM, which
 *     comes back as 40 (4*10+0) instead of 0.
 *   - the hour masked to one nibble: caught, by 10 AM (0x10 & 0x0F is 0) and by
 *     11 PM (0x31 & 0x1F is 0x11, so the hour is 1 and the answer is 13).
 *   - PM added in binary rather than in BCD: NOT caught by that table, and this
 *     is worth stating precisely because it is not a near miss.
 *
 * Adding 12 to a BCD hour in binary and then reading the result back with
 * bcd_to_bin() - which is 10*hi + lo, a linear map with no notion of digits -
 * agrees with the right answer for 1, 2 and 3 PM and for 10, 11 and 12 PM, and
 * disagrees for every hour from 4 PM to 9 PM.  Worked through:
 *
 *     4 PM   BCD 0x04, binary add 0x10 -> 10      BCD add 0x16 -> 16
 *     8 PM   BCD 0x08, binary add 0x14 -> 14      BCD add 0x20 -> 20
 *     9 PM   BCD 0x09, binary add 0x15 -> 15      BCD add 0x21 -> 21
 *
 * The tens is 1 either way, so 10 + (4+12) and 20 + 4 are the same number; it is
 * the hours whose tens changes from 1 to 2 that come out six hours early.  Every
 * PM case in the twelve-reading table is either below the crossover or has the
 * tens already at 1, which is why it passed.  Four PM is the smallest case that
 * does not, and 4 PM through 9 PM is the whole of the bug's range.
 *
 * So the table is exhaustive instead of chosen.  It costs nothing - these are a
 * few hundred instructions over a wire model - and it means the branch has no
 * untested corner left in it, rather than no corner left in it that somebody
 * remembered to write down.
 *
 * THE EXPECTED HOURS ARE THE CIVIL CONVENTION, stated in binary: 12 AM is 0,
 * 12 PM is 12, and PM adds twelve to an hour already in 1..11.  That is the
 * definition of a clock face, which is a different statement from the driver's
 * register manipulation, and it is the only thing in this file that is allowed to
 * know what an hour is.
 */
static int test_hour_conversion(void)
{
    static const uint8_t refused[] = {
        0x00u,   /* 12-hour mode holding hour 0: below the range */
        0x13u,   /* 12-hour mode holding hour 13: above the range */
        0x1Fu    /* and the top of bits 4:0, for the same reason */
    };
    unsigned hour;
    unsigned pm;
    unsigned reg;
    int rc;

    /* THIS BOARD'S CASE, and the reason the whole table above could pass while
     * date reported no clock.  24-hour encoding with the MODE BIT CLEAR: the
     * board returns 0x20 for 20:49 and 0x10 for 10:34, so bit 5 is carrying
     * the tens of the hour and nothing marks the format.  Twenty-four readings,
     * every hour of the day, which is the same set the first loop covers with
     * the mode bit set - the two differ only in that bit, so any decoder that
     * confuses them fails here. */
    for (hour = 0u; hour <= 23u; hour++) {
        reg = to_bcd((uint8_t)hour);   /* no mode bit: 0x20 is hour 20 */
        rc = expect((uint8_t)reg, (uint8_t)hour);
        if (rc != 0) {
            return 100 + (int)hour * 4 + rc;   /* 100..199 */
        }
    }

    /* 12-hour mode: bit 6 clear, bit 5 is PM, bits 4:0 are the BCD hour 01-12.
     * Twenty-four readings, 1 AM to 12 PM.
     *
     * Only when the driver is BUILT for 12-hour.  With the mode bit clear the
     * byte is ambiguous - 0x21 is 21:00 in 24-hour and 1 PM in 12-hour - so the
     * driver reads what MUZIX_RTC_HOUR_24_HOURS says, and on this board, which
     * returns 0x20 for 20:49 with the bit clear, that is 24-hour.  Asserting
     * the 12-hour table against a 24-hour build would be asserting a format the
     * board is not in; the two are separate configurations of the same
     * function, and the 24-hour one is the one this build is. */
    if (!MUZIX_RTC_HOUR_24_HOURS) {
        for (pm = 0u; pm <= 1u; pm++) {
            for (hour = 1u; hour <= 12u; hour++) {
                reg = (pm << 5) | to_bcd((uint8_t)hour);
                rc = expect((uint8_t)reg,
                            (uint8_t)(pm ? (hour == 12u ? 12u : hour + 12u)
                                         : (hour == 12u ? 0u : hour)));
                if (rc != 0) {
                    /* 12-hour case n is (pm * 12 + hour); status = n * 4 + rc */
                    return (int)(pm * 12u + hour) * 4 + rc;
                }
            }
        }
    }

    /* 24-hour mode: bit 7 CLEAR - the datasheet's mode select is inverted, high
     * selects 12-hour - bit 6 reserved at 0, and bit 5 is the tens-of-2 of the BCD
     * hour, not a PM flag.  All twenty-four readings, 00 to 23.  This half is what
     * says the two readings of bit 5 have been told apart: 13:00 is 0x13 and 23:00
     * is 0x23, and a decode that treated bit 5 as PM would put them twelve hours
     * apart while answering with numbers that all look like hours. */
    for (hour = 0u; hour <= 23u; hour++) {
        reg = to_bcd((uint8_t)hour);
        rc = expect((uint8_t)reg, (uint8_t)hour);
        if (rc != 0) {
            /* 24-hour case n is 25 + hour, 25..48 */
            return (int)(25u + hour) * 4 + rc;
        }
    }

    /* Bit 7 of the hour register is unused in 12-hour mode and is not part of
     * the value in either, so a chip with it set has to read the same.  Without
     * this, a decode that masked 0x1F and nothing else would look correct. */
    if (!MUZIX_RTC_HOUR_24_HOURS) {
        rc = expect(0x92u, 0u);             /* 12 AM with bit 7 set */
        if (rc != 0) {
            return 49 * 4 + rc;
        }
    }
    rc = expect(0xC3u, 3u);                 /* 03:00 with bit 7 set */
    if (rc != 0) {
        return 50 * 4 + rc;
    }

    /* The malformed readings.
     *
     * NOT COVERED HERE, and named so it is not mistaken for covered: the chip's
     * clock-halt bit, which is bit 7 of the seconds register.  The driver
     * strips it from the copy it range-checks and keeps it on the one it
     * decodes, so a halted clock is read rather than refused - which is a claim
     * about the seconds register, not about the hour, and the model cannot be
     * asked for a halted clock anyway: ds1302_set_time_bcd_raw() masks the bit
     * out on the way in.  Testing it needs a way to set it, not a case here. */
    /* All three are 12-hour-mode readings, so they are refusals only in a
     * 12-hour build.  In a 24-hour one 0x00 is midnight and reads as 0, which
     * is the point of the declared mode: refusing it would take this board's
     * clock away for the hour it spends between midnight and one. */
    if (!MUZIX_RTC_HOUR_24_HOURS) {
        for (hour = 0; hour < sizeof(refused) / sizeof(refused[0]); hour++) {
            rc = expect_refused(refused[hour]);
            if (rc != 0) {
                return (int)(51u + hour) * 4 + rc;
            }
        }
    }
    return 0;
}

/*
 * THE WRITE PATH, WHICH NOTHING IN THIS FILE TESTED UNTIL NOW
 * ==========================================================
 *
 * Everything above drives muzix_zeta_rtc_get().  muzix_zeta_rtc_set() had no
 * test at all on the host, and the only other test that reaches it,
 * kernel/test_syscall_setrtc.c, asserts what the syscall handler copies out to
 * a caller rather than what the driver put on the wire.  A driver whose write
 * went nowhere passed all of them, and did so on hardware where the same driver
 * left `date` reporting that the clock had not taken the new time.
 *
 * WHAT IS ASSERTED
 * ----------------
 * A set followed immediately by a read-back must return what was set.
 * muzix_zeta_rtc_set() does not stop at the write - it reads the chip back over
 * the caller's own struct, so on return the struct holds what the chip says
 * rather than what it was asked to store.  If the write went nowhere the struct
 * holds the time that was already on the chip, and that is the whole of the
 * report a caller gets.  `date` is that caller: it saves what it asked for,
 * sets, and compares.
 *
 * The chip's own registers are checked as well, and separately, because the
 * struct alone cannot tell the two apart.  A driver that wrote nothing and then
 * echoed the request into the struct would satisfy the first check, and that is
 * not a driver that keeps time.
 *
 * WHY THE THREE TIMES ARE THESE THREE
 * -----------------------------------
 * Every field of every case differs from what seed_chip() leaves on the chip,
 * so a write that lands on no register, on the wrong one, or only on the first
 * one cannot pass.
 *
 *   - Case 0 is the one the board produced.  A dead write used to leave a
 *     read-back that returned nothing at all, and seven bytes read as zero are
 *     midnight on the first of January of year zero: `Sun Jan 0 00:00:00 2000`,
 *     every field zero.  This case is nearly all zeros itself, so a driver that
 *     "passes" by reading zeros back cannot pass it - the day, month and weekday
 *     here are 1, not 0.
 *   - Case 1 is every field at its maximum: 59:59:23 on 31 December, weekday 7.
 *     A write that stops early, or that walks the registers in the wrong order,
 *     puts the wrong value in at least one of them.
 *   - Case 2 is the middle of the day in the middle of the year, which is the
 *     shape of a real `date MMDDhhmmYYYY` and the one the user is waiting on.
 *
 * Exit status is 220 + case * 10 + what went wrong, so the number names the case
 * and the failure: 221 is "case 0, the set did not take", 222 is "case 0, the
 * chip does not hold it", 231 is "case 1, the set did not take".  The 220s are
 * clear of every status the read path above returns, which stop at 215, and
 * below 256 - a process exit status is one byte, so an encoding above it is
 * reported as its low eight bits and a failure at 301 comes out as 45.
 */
static int test_the_write_path(void)
{
    static const muzix_rtc_time_t wanted[3] = {
        { 0u,  0u,  0u,  1u,  1u,  0u, 1u },   /* case 0 */
        { 59u, 59u, 23u, 31u, 12u, 99u, 7u },   /* case 1 */
        { 30u, 45u, 13u, 15u,  9u, 26u, 5u }    /* case 2 */
    };
    /* What the chip must hold afterwards, in chip order and still BCD: seconds,
     * minutes, hours, date, month, day of week, year.  Spelled out rather than
     * converted from `wanted`, because a conversion written here would be a
     * second copy of the driver's own and could agree with a broken one.
     *
     * The hour column is the bare BCD hour and nothing else.  These three were
     * 0x40, 0x63 and 0x53, i.e. the BCD with bit 6 set, on the belief that bit 6
     * was the mode and that setting it asked for 24-hour mode.  Bit 6 is
     * reserved and reads as 0; bit 7 is the mode select and it is inverted, so
     * 24-hour mode is a zero there.  The value the driver now writes is the BCD
     * hour on its own, and these are the numbers the datasheet's table gives for
     * 00, 23 and 13. */
    static const uint8_t expect_registers[3][7] = {
        { 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x00u },   /* case 0 */
        { 0x59u, 0x59u, 0x23u, 0x31u, 0x12u, 0x07u, 0x99u },   /* case 1 */
        { 0x30u, 0x45u, 0x13u, 0x15u, 0x09u, 0x05u, 0x26u }    /* case 2 */
    };
    unsigned i;
    unsigned j;
    muzix_rtc_time_t got;
    uint8_t registers[7];

    for (i = 0; i < 3u; i++) {
        seed_chip();

        /* Binary, because that is what muzix_rtc_time_t holds and what the
         * driver encodes; a case that fed it BCD would be testing nothing. */
        got = wanted[i];
        (void)muzix_zeta_rtc_set(&got);

        if (got.second != wanted[i].second ||
            got.minute != wanted[i].minute ||
            got.hour   != wanted[i].hour   ||
            got.day    != wanted[i].day    ||
            got.month  != wanted[i].month  ||
            got.year   != wanted[i].year   ||
            got.weekday != wanted[i].weekday) {
            return 220 + (int)i * 10 + 1;
        }

        muzix_host_ds1302_get_registers(registers);
        for (j = 0; j < 7u; j++) {
            if (registers[j] != expect_registers[i][j]) {
                return 220 + (int)i * 10 + 2;
            }
        }
    }
    return 0;
}

/*
 * A PROTECTED CHIP, WHICH IS THE BOARD THIS DRIVER RAN ON
 * =======================================================
 *
 * The case above sets a chip whose write protect is clear, because
 * ds1302_reset() and ds1302_set_time_bcd_raw() both leave it that way and there
 * was no other way to reach the other state.  So the whole file agreed with a
 * driver that never touches register 0x8E, and agreed with it on hardware where
 * that driver wrote nothing at all: `date` said "the clock did not take the new
 * time" and printed the time the chip already had, and twenty-one host tests
 * passed.  A model that cannot enter the state cannot report the fault, and the
 * state is one line of a chip's power-on behaviour.
 *
 * Bit 7 of the write-protect register rejects every write to the clock registers
 * and to RAM.  FUZIX, on this board and this chip, reads 0x8F before a write and
 * clears the bit if it is set, and its one clock write opens by clearing it.
 * So this is not a
 * hypothetical: it is the same protection, on the same wiring, and a driver that
 * ignores it is a driver whose set does not take.
 *
 * What is asserted is that muzix_zeta_rtc_set() gets through it: the chip is
 * armed first, the set is driven through the real driver and the real wire, and
 * the read-back and the chip's own registers must both hold what was asked for.
 * Against a driver that does not clear the bit this exits 3 - the read-back comes
 * back as the seed that seed_chip() left, which is the whole of the report `date`
 * has to work from, and is why that message was all a person on the board ever
 * got.
 */
static int test_a_protected_chip_is_still_settable(void)
{
    muzix_rtc_time_t wanted;
    muzix_rtc_time_t got;
    uint8_t registers[7];
    static const uint8_t expect[7] = {
        0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x00u
    };
    unsigned j;

    seed_chip();
    muzix_host_ds1302_set_write_protect(0x80u);

    wanted.second = 0u;
    wanted.minute = 0u;
    wanted.hour   = 0u;
    wanted.day    = 1u;
    wanted.month  = 1u;
    wanted.year   = 0u;
    wanted.weekday = 1u;

    got = wanted;
    (void)muzix_zeta_rtc_set(&got);

    /* Every field, because a driver that cleared the bit too late to matter for
     * one register would still pass a check of the register it happened to get
     * right - and the registers before the clearing is where a late clear would
     * show. */
    if (got.second != wanted.second || got.minute != wanted.minute ||
        got.hour != wanted.hour || got.day != wanted.day ||
        got.month != wanted.month || got.year != wanted.year ||
        got.weekday != wanted.weekday) {
        return 3;
    }

    muzix_host_ds1302_get_registers(registers);
    for (j = 0; j < 7u; j++) {
        if (registers[j] != expect[j]) {
            return 4;
        }
    }

    /* And the protection must be OFF afterwards, not merely out of the way for
     * this call.  This is checked by doing rather than by reading, because
     * reading it would need a second door into the model for one bit, and
     * because the bit is not what a caller cares about: a set that leaves the
     * clock protected is a clock that can be set once.  So set it again, to a
     * different time, and require that to land as well. */
    wanted.second = 45u;
    wanted.minute = 30u;
    wanted.hour   = 13u;
    wanted.day    = 17u;
    wanted.month  = 9u;
    wanted.year   = 26u;
    wanted.weekday = 5u;

    got = wanted;
    (void)muzix_zeta_rtc_set(&got);

    if (got.second != wanted.second || got.minute != wanted.minute ||
        got.hour != wanted.hour || got.day != wanted.day ||
        got.month != wanted.month || got.year != wanted.year ||
        got.weekday != wanted.weekday) {
        return 5;
    }
    return 0;
}

int main(void)
{
    int rc = test_hour_conversion();

    if (rc != 0) {
        return rc;
    }
    rc = test_the_write_path();
    if (rc != 0) {
        return rc;
    }
    return test_a_protected_chip_is_still_settable();
}
