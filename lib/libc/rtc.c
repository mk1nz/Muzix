#include "libc.h"
#include "syscall.h"

/*
 * Days from 2000-01-01 to the given date, proleptic Gregorian.
 *
 * WRITTEN AS A COUNT AND NOT AS ARITHMETIC, and the reason is measured rather
 * than theoretical.  This function used to be
 *
 *     int leap = ((year % 4) == 0 && (year % 100) != 0) || (year % 400) == 0;
 *     int leaps = (yend / 4 - 1999 / 4) - (yend / 100 - 1999 / 100) +
 *                 (yend / 400 - 1999 / 400);
 *     return (year - MUZIX_RTC_YEAR_BASE) * 365 + leaps + doy;
 *
 * and on this target it produced `leap == 0` for every year and a `leaps` that
 * was the right answer plus 315.  The extra 315 is constant, so it cancels in
 * the difference uptime is made of; `leap == 0` does not, and it costs exactly
 * one day for any date after February in a leap year - so a machine that booted
 * in September and was asked for its uptime in March 2068 was a day short, and
 * nothing about the answer looked wrong.
 *
 * The cause is not a mistake in the expressions but the two runtime routines
 * they call.  `%` and `/` on 16-bit values become calls to __modsint and
 * __divsint, and lib/math.s then left the result in HL while SDCC's generated
 * caller reads D and E - the same disagreement that file already documents
 * and works around for __mulint, applied to only one of the five.  (Its
 * divide loop also compared against the divisor's low byte alone, so a
 * divisor of 400 divided by 144, and __divuint refused any divisor of 256 or
 * more.)  All four are fixed now - each writes the result to DE as well as
 * HL - but that is no reason to go back to `/` and `%`: this routine's
 * count-only form is the reason it is right, and the fixed helpers are the
 * argument for not needing them rather than for using them.  The previous
 * time() implementation did not exercise this conversion, so the day count
 * had not been executed on the target before the RTC-backed time and uptime
 * paths were added.
 *
 * So: no division, no modulo, and no multiply - only integer additions, which
 * SDCC expands correctly on this target (measured, and used by the arithmetic in
 * printf() and by the 32-bit subtraction this file already relies on).  At most
 * 99 iterations for a century, which is nothing.
 *
 * The leap rule over 2000-2099 is "divisible by four" and needs no century
 * terms: 2000 is divisible by 400 and is a leap year, and no other year in the
 * range is a century at all.  That is the same reasoning apps/date.c uses for
 * days_in_month(), so the two cannot disagree about how long February is.
 *
 * This is the only implementation of "what date is that" in the tree - the
 * kernel reads the chip and hands the seven fields over unconverted, because a
 * month table and a leap-year count do not fit in its remaining _CODE (see the
 * note in kernel/syscalls.c).  time() and uptime() both go through it, which is
 * the property uptime needs: it is the difference of two of these, so a
 * disagreement between two of them would be a whole day of wrong answer.
 */
/* uint16_t, not uint8_t, and the compiler says so: warning 158, "overflow in
 * implicit constant conversion", on the 334. Stored in a uint8_t that is 78, so
 * every December date came out 256 days early - days 78+31+29 rather than
 * 334+day-1. The other eleven entries fit, which is why nothing ever noticed:
 * the table is right everywhere except the one month that is late in the year,
 * and a date 256 days out is still a date that looks like a date. */
static const uint16_t muzix_month_start[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static int32_t
muzix_days_from_utc(int year, int month, int day)
{
    int32_t days;
    int32_t leap;
    int y;

    days = 0;
    leap = ((year & 3) == 0) ? 1 : 0;
    y = MUZIX_RTC_YEAR_BASE;

    for (y = MUZIX_RTC_YEAR_BASE; y < year; ++y) {
        days = days + 365;
        if ((y & 3) == 0) {
            days = days + 1;
        }
    }
    return days + (int32_t)muzix_month_start[month - 1] + (int32_t)day - 1 +
           ((leap && month > 2) ? 1 : 0);
}

/*
 * A 16-bit value scaled by a 32-bit factor, by Horner over its bits.
 *
 * value is 0..65535 and factor is 0..65535; the product may need 32 bits, and
 * there is no 32-bit multiply on this target: a 32-bit product lowers to a call
 * to __mullong, which no z80 build of SDCC provides, and an unresolved global
 * resolves to 0x0000 - the boot stub, so a program that multiplied restarted the
 * machine instead of returning.  tools/check_userspace_runtime.rb now fails the
 * build on any link that reports one, so that mistake cannot come back quietly.
 *
 * A 16-bit multiply (__mulint) is no use either, and not only because it
 * truncates: `hour * 3600` is one, and it overflows from ten o'clock onwards -
 * 12:00 came out as -22,336 rather than 43,200, which put every reading after
 * 10:00 a whole day out.  lib/math.s's __mulint returns the low half in HL and
 * DE, so the truncation is not a register disagreement, it is the arithmetic:
 * the product of two 16-bit values simply does not fit in 16 bits.
 *
 * Sixteen 32-bit additions, and a 32-bit addition is correct here: SDCC expands
 * it to a byte chain with a carry, the same shape as the 32-bit subtraction and
 * the 32-bit doubling this file is measured to get right.  Non-negative operands
 * only, which is all the callers below have.
 */
static int32_t
muzix_scale16(uint16_t value, int32_t factor)
{
    int32_t acc;
    int i;

    acc = 0;
    i = 15;

    for (i = 15; i >= 0; --i) {
        acc = acc + acc;
        if ((value >> i) & 1) {
            acc = acc + factor;
        }
    }
    return acc;
}

/*
 * The chip's reading as a count of seconds, and the interval since boot.
 *
 * The struct is 7 bytes and the kernel's copy is one short transfer, so there
 * is no chunking here - see the comment on SYS_READ's staging cap for what
 * happens when a copy is larger than the buffer.  MUZIX_RTC_STRUCT_SIZE is
 * checked against sizeof(struct rtc_time) in the kernel, which refuses a
 * mismatch rather than copying the wrong number of bytes.
 */

/* One reading of the chip as a count of seconds, from 2000-01-01 00:00:00.
 *
 * The single conversion in this file.  time() and uptime() both need it, and
 * two copies of a calendar conversion is two chances to disagree about what
 * date it is - which is not a theoretical worry, because the difference of two
 * absolute counts is only correct if both counts use the same day numbering, and
 * a mismatch of one day in the boot reading is a whole day of wrong uptime
 * rather than anything visible.
 *
 * The three scaled terms are what makes the output right past a boundary rather
 * than merely plausible: each reading becomes a point on a line, so the
 * difference of two of them is the distance between them, whatever month, year
 * or leap day happens to be in between.
 *
 * The result is seconds in an int32_t, so it wraps after 2^31 seconds - about
 * 68 years, or somewhere in 2068.  That limit is harmless for time(), which
 * reports one date, and irrelevant to uptime(), which subtracts a boot reading
 * of exactly zero from this number: a wrapped reading and an unwrapped one
 * differ by a multiple of 2^32, and the difference between them is still the
 * true interval.  Measured on the target, 2069-01-01 to 2099-12-31 comes out as
 * 11321 days 23:59:59.  See uptime() for what that does and does not buy.
 */
static int32_t
muzix_rtc_seconds(const struct rtc_time *t)
{
    int32_t days;
    int32_t minutes;

    days = muzix_days_from_utc(MUZIX_RTC_YEAR_BASE + t->year, t->month,
                               t->day);
    /* Minutes past midnight, 0..1439, scaled to seconds without the 16-bit
     * multiply that `hour * 3600` becomes - see muzix_scale16(). */
    minutes = muzix_scale16(t->hour, 60) + (int32_t)t->minute;

    return muzix_scale16((uint16_t)days, 86400) + muzix_scale16((uint16_t)minutes, 60) +
           (int32_t)t->second;
}

/*
 * Is this a time the chip can actually hold?
 *
 * The DS1302 does not refuse a value it is handed.  60 seconds is 0x60 in BCD,
 * the burst write is accepted, and the clock counts on through a time that was
 * never one, so an unchecked caller does not get an error - it gets a wrong
 * clock and no indication of it.
 *
 * The test is here rather than in the kernel's driver because that is where the
 * room is: the driver's own range check measured 69 bytes against the 45 the
 * kernel had left, and tools/check_kernel_layout.rb fails the build rather than
 * letting _DATA eat the C stack.  See platform/zeta-v2/rtc_ds1302.h.
 *
 * Weekday 0 is accepted, because it means the caller did not say; the driver
 * stores Sunday for it, since a burst write always sends that register.  Every
 * other field is checked at both ends: day and month are one-based, and 0 would
 * be written as 0 and counted from.
 */
static int
rtc_time_is_valid(const struct rtc_time *t)
{
    if (t->second > 59u || t->minute > 59u || t->hour > 23u) {
        return 0;
    }
    if (t->month < 1u || t->month > 12u) {
        return 0;
    }
    if (t->day < 1u || t->day > 31u) {
        return 0;
    }
    if (t->year > 99u || t->weekday > 7u) {
        return 0;
    }
    return 1;
}

int
time(int32_t *seconds)
{
    struct rtc_time now;
    int32_t value;

    if (sys_getrtc(&now) < 0) {
        return -1;
    }
    value = muzix_rtc_seconds(&now);
    if (seconds) {
        *seconds = value;
    }
    return value;
}

/*
 * Seconds since the first exec after power-on: wall clock now minus wall clock
 * at boot.
 *
 * The DS1302 is the wall-clock source, and time() converts its fields to
 * seconds since 2000-01-01. The 120 Hz scheduler tick and per-process counters
 * serve deadlines and accounting, not calendar time. Uptime is the difference
 * between two readings of the DS1302, the second of which the kernel took at
 * boot and kept (see exec_loader.h).
 *
 * CALENDAR ASSUMPTIONS, all of them forced by the hardware rather than chosen:
 *
 *  - Both readings are civil local time from the same chip and the same
 *    timezone, because struct rtc_time has no timezone and nothing in the tree
 *    has one. There is no conversion to apply and none is invented.
 *  - The chip's two-digit year is read in MUZIX_RTC_YEAR_BASE (2000), so both
 *    ends of the difference are in 2000-2099. The one case this gets wrong is a
 *    machine that has been up across 2099-12-31, where the year register wraps
 *    to 00 and the reading means 2100 rather than 2000: the answer would be off
 *    by a hundred years, and a hundred years of uptime is outside what the
 *    answer can hold in any case.
 *  - Proleptic Gregorian, with the leap rule in muzix_days_from_utc().  Over
 *    2000-2099 that rule is "divisible by four" and needs no century terms at
 *    all, and it is the same rule apps/date.c uses to decide how long February
 *    is, so this and `date` cannot disagree about a month's length.
 *  - No leap seconds, because the DS1302 keeps civil time and does not insert
 *    one; a second that is added to the clock's own count appears in the
 *    difference as a second of uptime, which is the only behaviour available.
 *  - The chip counts through a power-off. That is not a defect here, because
 *    the baseline is re-taken at every boot, so time spent off is never
 *    counted: uptime measures the interval since this power-on, not since the
 *    clock was last set.
 *
 * Returned with get_rtc()'s convention - 0 for success, -1 for failure - rather
 * than time()'s, because here the value has a legitimate zero and a legitimate
 * negative.  A negative *seconds is the chip reading EARLIER than it did at
 * boot, which is what running `date` to set the clock back looks like; it is
 * reported, not printed as a number.  A positive result is exact for any
 * interval below 2^31 seconds, as the note on muzix_rtc_seconds() explains; a
 * longer one is not representable and comes back negative, indistinguishable
 * from a clock set back.
 *
 * `boot` is handed back too, because the call has already read it and a program
 * that prints the baseline beside the figure it derived from it is describing
 * one instant rather than two.  It is optional: pass NULL for the number alone.
 */
int
uptime(int32_t *seconds, struct rtc_time *boot)
{
    struct rtc_time now;
    struct rtc_time at_boot;

    if (sys_getrtc_since(&now, &at_boot) < 0) {
        return -1;
    }
    if (seconds) {
        *seconds = muzix_rtc_seconds(&now) - muzix_rtc_seconds(&at_boot);
    }
    if (boot) {
        *boot = at_boot;
    }
    return 0;
}

int
get_rtc(struct rtc_time *out)
{
    if (!out) {
        return -1;
    }
    return sys_getrtc(out) < 0 ? -1 : 0;
}

int
set_rtc(struct rtc_time *in)
{
    /* The kernel reads the struct, writes the chip, and leaves the chip's own
     * reading back in the same seven bytes, so the result arrives in the
     * caller's buffer.  A struct copy, not a cast: the caller's memory is
     * theirs, and it is this function's business to hand back what came.
     *
     * The range test runs before the syscall, so a value the chip would accept
     * and then count on through is refused here and the clock is left alone.
     * The caller's buffer is untouched in that case: it asked to set something
     * and it did not happen, so what it passed in is still what it passed in. */
    struct rtc_time copy;

    if (!in || !rtc_time_is_valid(in)) {
        return -1;
    }
    copy = *in;
    if (sys_setrtc(&copy) < 0) {
        return -1;
    }
    *in = copy;
    return 0;
}
