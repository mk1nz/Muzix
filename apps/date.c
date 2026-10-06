/*
 * date - print or set the system date and time
 *
 * The clock is the DS1302 on the Zeta V2's port $70, read and written through
 * the kernel, so what this prints is the chip's own idea of the time and not a
 * count of scheduler passes.  It advances whether or not anything is looking
 * at it.
 */

#include "../lib/libc.h"

static const char *const MONTHS[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const WEEKDAYS[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/*
 * Print a two-digit number.
 *
 * Two things here are deliberate.  Muzix's printf handles %s, %c, %d, %x and
 * %% and nothing else: a width or a zero-pad is not a modifier here, it falls
 * through to the default arm and prints a question mark for the character
 * after the percent, so "%02d" comes out as "?2d".  And the tens are found by
 * subtraction rather than division, because the division helper a `/` compiles
 * to is not linked into any userspace program - see print_unsigned() in
 * lib/libc.c, which is the same problem and the same fix.
 */
static void put2(int value)
{
    int tens = 0;

    while (value >= 10) {
        value -= 10;
        tens++;
    }
    putchar((char)('0' + tens));
    putchar((char)('0' + value));
}

static void print_time(const struct rtc_time *t)
{
    int weekday = (t->weekday >= 1 && t->weekday <= 7) ? t->weekday - 1 : 0;
    int month = (t->month >= 1 && t->month <= 12) ? t->month - 1 : 0;

    printf("%s %s %d ", WEEKDAYS[weekday], MONTHS[month], t->day);
    put2(t->hour);
    putchar(':');
    put2(t->minute);
    putchar(':');
    put2(t->second);
    printf(" %d\n", MUZIX_RTC_YEAR_BASE + t->year);
}

static void print_usage(void)
{
    puts("usage: date\n");
    puts("       date MMDDhhmm\n");
    puts("       date MMDDhhmmYYYY\n");
}

static const char HEXDIGITS[16] = "0123456789abcdef";

/* One byte as two hex digits.
 *
 * Not printf's %x: this libc prints the highest place a value reaches, so 0x0a
 * comes out as "a" and 0x06 as "6" (lib/libc.c, print_unsigned).  A fixed width
 * is what makes a column of these readable against another one. */
static void puthex2(int value)
{
    putchar(HEXDIGITS[(value >> 4) & 0x0F]);
    putchar(HEXDIGITS[value & 0x0F]);
}

/* What the chip sent, when the chip would not give a time.
 *
 * The kernel fills the caller's struct with the chip's own bytes even when it
 * refuses the read, and returns -1 (kernel/syscalls.c, the GETRTC handler).
 * Those bytes are the only thing that separates the three ways this fails: all
 * ones is a board where nothing is answering, an hour byte without bit 6 is a
 * board somebody left in 12-hour mode, and anything else is a wire that did not
 * do what was asked of it.  "cannot read the clock" alone distinguishes none of
 * them, which is why this exists: the person running it is looking at a board,
 * not at a test bench, and this is the data that tells them which.
 *
 * Seconds, minutes and hours are printed because they are the registers a
 * DS1302 answers first and the three a person can check against a battery-backed
 * one.  The struct's day, month, year and weekday carry the same raw bytes. */
static void report_chip_bytes(const struct rtc_time *t)
{
    puts("date: cannot read the clock; chip sent ");
    puts("sec=");
    puthex2(t->second);
    puts(" min=");
    puthex2(t->minute);
    puts(" hour=");
    puthex2(t->hour);
    puts("\n");
}

/* Read exactly `digits` decimal digits from `s` into `out`, checking each one.
 *
 * The length of the whole argument is checked once, by the caller, before any
 * of this runs; this only confirms that the characters at a given offset are
 * digits.  An earlier version also required a terminator immediately after the
 * digits it read, which is right for the last field of an argument and wrong
 * for the first: "122423452030" failed on the month, because the character
 * after it is another digit.
 *
 * atoi() is no use here either: it accepts a leading sign, it stops at the
 * first non-digit without saying how far it got, and it returns 0 both for "0"
 * and for "not a number at all".  A clock that silently sets itself to midnight
 * because the argument was malformed is worse than one that refuses. */
static int read_digits(const char *s, int digits, int *out)
{
    int value = 0;
    int i;

    for (i = 0; i < digits; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return 0;
}

/* Day of the week for a date, so the chip's register can be kept in step.
 *
 * Sakamoto's method: month offsets for a non-leap year, a correction for a
 * leap year after February, then the year and its quarter- and century-terms,
 * reduced mod 7 with Sunday as 0, which is the chip's day 1.
 *
 * The mod 7 is a subtraction loop rather than a `%`: see the note on put2.
 * The value is under 2800, so the loop is short. */
static int weekday_of(int year, int month, int day)
{
    static const int offset[12] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
    };
    int y = year;
    int v;

    if (month < 3) {
        y -= 1;
    }
    /* y is 1969-2099, which is two centuries and two 400-year eras, so the
     * year/100 and year/400 terms are comparisons rather than divisions. */
    v = y + (y >> 2) - ((y >= 2000) ? 20 : 19) + ((y >= 2000) ? 5 : 4) +
        offset[month - 1] + day;
    while (v >= 7) {
        v -= 7;
    }
    return v + 1;
}

/* Days in the month, so an impossible date is refused rather than written to
 * the chip, which would accept 31 February and count on from there.
 *
 * The leap test is a single mask because the range is 2000-2099 and every
 * year in it that is a multiple of four is an ordinary leap year: 2000 is
 * the only century year, and it is divisible by 400. */
static int days_in_month(int month, int year)
{
    static const int length[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month == 2 && (year & 3) == 0) {
        return 29;
    }
    return length[month - 1];
}

int main(int argc, char *argv[])
{
    struct rtc_time t;
    int month;
    int day;
    int hour;
    int minute;
    int year;

    if (argc == 1) {
        if (get_rtc(&t) != 0) {
            report_chip_bytes(&t);
            return 1;
        }
        print_time(&t);
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }

    if (argc == 2) {
        int len = (int)strlen(argv[1]);

        if (len != 8 && len != 12) {
            puts("date: expected MMDDhhmm or MMDDhhmmYYYY");
            print_usage();
            return 1;
        }
        if (read_digits(argv[1], 2, &month) != 0 ||
            read_digits(argv[1] + 2, 2, &day) != 0 ||
            read_digits(argv[1] + 4, 2, &hour) != 0 ||
            read_digits(argv[1] + 6, 2, &minute) != 0) {
            puts("date: not a date and time");
            print_usage();
            return 1;
        }
        if (len == 12) {
            if (read_digits(argv[1] + 8, 4, &year) != 0) {
                puts("date: not a date and time");
                print_usage();
                return 1;
            }
        } else {
            /* No year given: keep the one already set, so a bare time of day
             * does not throw the date away. */
            if (get_rtc(&t) != 0) {
                report_chip_bytes(&t);
                return 1;
            }
            year = MUZIX_RTC_YEAR_BASE + t.year;
        }
    } else {
        puts("date: expected at most one argument");
        print_usage();
        return 1;
    }

    if (month < 1 || month > 12) {
        puts("date: month out of range");
        return 1;
    }
    if (day < 1 || day > days_in_month(month, year)) {
        puts("date: day out of range for that month");
        return 1;
    }
    if (hour > 23 || minute > 59) {
        puts("date: time out of range");
        return 1;
    }

    /* The chip holds a two-digit year, so a four-digit one is taken modulo 100
     * and the century is not stored anywhere.  Say so rather than let 2026 and
     * 1926 look identical. */
    if (year < MUZIX_RTC_YEAR_BASE || year > MUZIX_RTC_YEAR_BASE + 99) {
        puts("date: year out of range (the chip stores 2000-2099)");
        return 1;
    }

    t.second = 0;
    t.minute = (uint8_t)minute;
    t.hour = (uint8_t)hour;
    t.day = (uint8_t)day;
    t.month = (uint8_t)month;
    t.year = (uint8_t)(year - MUZIX_RTC_YEAR_BASE);
    t.weekday = (uint8_t)weekday_of(year, month, day);

    if (set_rtc(&t) != 0) {
        puts("date: the clock did not answer");
        return 1;
    }

    /* set_rtc() hands back what the chip actually holds, not what was asked
     * for, so this is a real check rather than an echo of the argument: a write
     * that silently did not land leaves the old time in the struct and is
     * caught here.  Without the compare this would print the new time whatever
     * the chip had done, which is the failure worth being most careful about.
     *
     * The minute is deliberately not compared, and it is the whole bug: the
     * chip is running, so the minute read back is the minute it has reached,
     * not the one that was set.  Asking for 01:00 and reading 01:03 is a set
     * that worked perfectly and a compare that called it a failure.  The second
     * is not compared either, for the same reason on a shorter scale.
     *
     * The date and the hour are compared, and those do not move.  The hour can
     * roll over inside the operation - set at 23:59 and read at 00:00 - and
     * that case reports failure on a set that also worked.  It is one hour of
     * every twenty-four, against a check that has no false positives on any
     * other minute of any other day. */
    if (t.hour != hour || t.day != day || t.month != month ||
        t.year != (uint8_t)(year - MUZIX_RTC_YEAR_BASE)) {
        puts("date: the clock did not take the new time");
        print_time(&t);
        return 1;
    }

    print_time(&t);
    return 0;
}
