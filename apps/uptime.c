/*
 * uptime - how long this power-on has been up, as days, hours, minutes, seconds
 *
 *     up 3 days, 7:42:11
 *     since 2026-09-30 02:10:28
 *
 * WHY THIS IS A DIFFERENCE OF TWO CLOCK READINGS, which is the whole content of
 * the program:
 *
 * The system has a 120 Hz CTC tick and per-process accounting, but the
 * scheduler is cooperative and those counters are not a wall-clock source.
 * The DS1302 supplies calendar time; time() converts it to seconds since the
 * project's 2000-01-01 epoch. Uptime is the difference between the current RTC
 * reading and the RTC baseline saved at boot.
 *
 * The DS1302 on port $70 keeps calendar time independently of the scheduler.
 * So uptime is
 *
 *     (the clock now) - (the clock at boot)
 *
 * and the second half of that has to be stored, taken once, and never re-taken.
 * The kernel does that in muzix_exec_load() - the one function every exec goes
 * through, and the one the kernel calls directly to start the shell, so the
 * first exec after power-on is the one that records it.  A syscall handler is
 * the wrong place for exactly this reason: the kernel enters the first process
 * through muzix_kernel_loop_run() with no syscall on the way, so a baseline
 * recorded in a handler would never exist for the process the user is looking
 * at.  It is the same trap as the process name `top` reads, and for the same
 * reason the loader is where that is filled in too.
 *
 * One call returns both ends of the difference.  SYS_GETRTC fills the first
 * struct with the clock now, and - because arg2 is not null - the second with
 * the reading the kernel kept from boot, out of the same read of the same chip.
 * There is no second clock anywhere in this and no second time source to be
 * confused with: the baseline is not a clock, it is one reading of the clock the
 * caller already has in hand.
 *
 * THE ARITHMETIC, and why it is not written as a subtraction of fields:
 *
 * Taking the seconds field's difference, or the day-of-month's, is the obvious
 * way to write this and every version of it is wrong somewhere.  Seconds wrap
 * every minute, the day-of-month wraps at every month length, and the year
 * wraps at a hundred.  So each reading is turned into an absolute count of
 * seconds first - days since 2000-01-01, times 86400, plus the time of day -
 * and the two counts are subtracted.  A date is a point on a line, so the
 * difference of two dates is the distance between them, and that is true across
 * midnight, across 30 April and across 31 December without a case for any of
 * them.  This is what a clock that reads "up 0 days, 0:04:11" the morning after a
 * month rollover would take if the month had been ignored.
 *
 * The calendar assumptions are not choices, and they are stated in full in
 * lib/libc.c next to the conversion, because they are forced by the hardware:
 * the chip has a two-digit year and no century, so both ends of the difference
 * are read in 2000-2099; the calendar is proleptic Gregorian, over which the
 * full leap rule and a plain divisible-by-four agree on every year in that
 * window; there is no timezone, because struct rtc_time has no field for one
 * and nothing in the tree has a database of them; and there are no leap
 * seconds, because the DS1302 keeps civil time and does not insert one.
 *
 * So uptime is the interval since the machine was switched on, and it restarts
 * at every power-on - the chip counts through a power-off, but the baseline is
 * re-read, so time spent off is not counted.  It does NOT survive a `date` that
 * sets the clock backwards: the chip then reads earlier than it did at boot, and
 * that is reported rather than printed as a number.
 *
 * Nothing here multiplies two 32-bit values, and that is not a style choice.
 * SDCC lowers a 32-bit multiply to a call to __mullong, which no z80 build of
 * SDCC 4.5 provides; the linker resolves the undefined name to 0x0000, which is
 * the boot stub, and the program restarts the machine.  `days * 86400` is five
 * shifts in lib/libc.c and `/ 86400` is a subtraction loop in split() below,
 * for that reason and no other; tools/check_userspace_runtime.rb now fails the
 * build on any userspace link that reports an undefined global, so the mistake
 * cannot come back as a silent restart.
 */

#include "../lib/libc.h"

/* Output goes through a line buffer rather than putchar().
 *
 * The same reason top.c assembles a line and writes it once: putchar() is one
 * SYS_WRITE per character, and this program is two lines of at most 88
 * characters.  One write per line is two syscalls.
 *
 * 88 rather than top.c's 80 because the second line here is longer - it carries
 * the baseline, which is the field that makes a wrong answer diagnosable rather
 * than merely wrong.  out() flushes rather than dropping if a line ever did not
 * fit, so a tail comes out in a second piece instead of being lost. */
static char line[88];
static int used;

static void out_flush(void);

static void out(char c)
{
    if (used >= (int)sizeof(line)) {
        out_flush();
    }
    line[used++] = c;
}

static void out_str(const char *s)
{
    while (*s) {
        out(*s++);
    }
}

static void out_end(void)
{
    out('\n');
    out_flush();
}

static void out_flush(void)
{
    if (used != 0) {
        write(1, line, (size_t)used);
        used = 0;
    }
}

/* A value of exactly two digits, zero-padded.
 *
 * put2() from top.c and date.c, by repeated subtraction: this libc's printf has
 * no width modifier, so "%02d" prints as "?2d", and a `/` is a call to a divide
 * helper no userspace link carries.  Every value reaching this is under 100 by
 * construction - the callers pass a minutes, a seconds, a month or a day. */
static void out2(int value)
{
    int tens = 0;

    while (value >= 10) {
        value -= 10;
        tens++;
    }
    out((char)('0' + tens));
    out((char)('0' + value));
}

/* A value of up to five digits, no padding.
 *
 * Place values and a repeated subtraction per place, the way print_unsigned() in
 * lib/libc.c does it, and for the reason given there: `/` and `%` on this build
 * are calls to __divuint and __moduint, which no userspace link line provides,
 * and an unresolved global is a definition at 0x0000 - the boot stub.  A
 * program that divided would run the stub's ldir and jump back into
 * kernel_main, which is what a `date` with one %d in it used to do.
 *
 * put_field() in top.c counts the same subtractions but writes into
 * `char digits[10]`, so it is a two-digit printer: given 2026 it counts 202
 * subtractions and writes at digits[202], 192 bytes past the end.  That is safe
 * there because every value it is given is a pid or a counter under a thousand,
 * and it is not safe to copy for a year.  The first version of this file copied
 * it, and printed four hundred characters of stack for the year.
 *
 * The places cover 0-99999, which is the whole of what reaches this: a year in
 * 2000-2099, and a day count bounded by 2^31 seconds over 86400 seconds to the
 * day, which is 24855. */
static void out_number(int32_t value)
{
    static const int32_t places[] = { 10000, 1000, 100, 10, 1 };
    int start;
    int i;

    if (value < 0) {
        out('-');
        value = -value;
    }

    /* Start at the highest place this value reaches, so 7 prints as "7" rather
     * than "00007", and then emit one digit per place, most significant first.
     * Straight into the line buffer: the first version collected them into a
     * local array and walked it backwards, and printed 2026 as 6202. */
    for (start = 0; start < 4 && value < places[start]; start++) {
        continue;
    }
    for (i = start; i < 5; i++) {
        int digit = 0;

        while (value >= places[i]) {
            value -= places[i];
            digit++;
        }
        out((char)('0' + digit));
    }
}

/* Elapsed seconds into days, and hours, minutes and seconds within the day.
 *
 * By subtraction rather than division, and the reason is the same one as
 * everywhere else in this tree: `/` on an int32_t is a call to __divsint, which
 * no userspace link line provides, and an unresolved global is a definition at
 * 0x0000 - the boot stub, so a program that divided would restart the machine
 * instead of printing.
 *
 * The hours loop is entered at most 23 times and the day loop once per day of
 * uptime; the day loop is the reason the whole thing takes a moment rather than
 * being instant, and it is bounded by the same 2^31 seconds the answer has to
 * fit in, so it cannot spin. */static void split(int32_t seconds, int32_t *days, int *hour, int *minute,
                 int *second)
{
    *days = 0;
    while (seconds >= 86400) {
        seconds -= 86400;
        (*days)++;
    }
    *hour = 0;
    while (seconds >= 3600) {
        seconds -= 3600;
        (*hour)++;
    }
    *minute = 0;
    while (seconds >= 60) {
        seconds -= 60;
        (*minute)++;
    }
    *second = (int)seconds;
}

static void print_elapsed(int32_t seconds)
{
    int32_t days;
    int hour;
    int minute;
    int second;

    split(seconds, &days, &hour, &minute, &second);
    out_str("up ");
    out_number(days);
    out_str(days == 1 ? " day, " : " days, ");
    out_number(hour);
    out(':');
    out2(minute);
    out(':');
    out2(second);
    out_end();
}

/* The baseline, in the form a reader can check it against a battery-backed
 * clock: 2026-09-30 02:10:28.
 *
 * Printed on every run and not only when something looks wrong, because the two
 * situations where the elapsed figure is wrong are both invisible without it.  A
 * baseline that reads an hour ago on a board that was switched on five minutes
 * ago is a clock that was adjusted after boot, and a baseline that is obviously
 * wrong is a boot reading that never happened - and neither is something the
 * elapsed number alone can distinguish from a correct answer. */
static void print_since(const struct rtc_time *t)
{
    out_str("since ");
    out_number(MUZIX_RTC_YEAR_BASE + t->year);
    out('-');
    out2(t->month);
    out('-');
    out2(t->day);
    out(' ');
    out2(t->hour);
    out(':');
    out2(t->minute);
    out(':');
    out2(t->second);
    out_end();
}

/* What the chip sent, when the chip would not give a time.
 *
 * The kernel fills the caller's struct with the chip's own bytes even when it
 * refuses the read and returns -1 (kernel/syscalls.c, the GETRTC handler), and
 * those bytes are the only thing that separates the ways this fails: all ones is
 * a board with nothing answering, an hour byte without bit 6 is a board left in
 * 12-hour mode, and anything else is a wire that did not do what was asked.
 * "cannot read the clock" alone distinguishes none of them, which is the same
 * reason date.c prints them. */
static const char HEXDIGITS[16] = "0123456789abcdef";

static void outhex2(int value)
{
    out(HEXDIGITS[(value >> 4) & 0x0F]);
    out(HEXDIGITS[value & 0x0F]);
}

static void report_chip_bytes(const struct rtc_time *t)
{
    out_str("uptime: cannot read the clock; chip sent sec=");
    outhex2(t->second);
    out_str(" min=");
    outhex2(t->minute);
    out_str(" hour=");
    outhex2(t->hour);
    out_end();
}

int main(int argc, char *argv[])
{
    int32_t seconds;
    struct rtc_time now;
    struct rtc_time boot;

    if (argc > 1) {
        out_str("usage: ");
        out_str(argv[0]);
        out_end();
        return 1;
    }

    /* Read both ends of the subtraction in one call, so the two cannot come from
     * different instants of a chip somebody is adjusting.  The kernel refuses
     * this if the live read was refused OR if there is no baseline, and the two
     * are told apart by asking the clock again on its own: a chip that answers
     * now but has no baseline is a machine whose clock did not answer at boot,
     * and a chip that does not answer is a machine with a dead clock. */
    if (uptime(&seconds, &boot) != 0) {
        if (get_rtc(&now) != 0) {
            report_chip_bytes(&now);
        } else {
            out_str("uptime: no boot time was recorded; the clock did not");
            out_str(" answer when this process first started");
            out_end();
        }
        return 1;
    }

    if (seconds < 0) {
        /* Two causes, one value, and the point of saying so.
         *
         * The common one: the clock has been set back since boot - someone ran
         * `date` to correct it, or the chip was adjusted - and the chip now
         * reads earlier than it did at power-on.  A negative interval is not an
         * interval, and printing one as days and hours would be arithmetic on a
         * number that does not mean what its fields say.
         *
         * The other one: the difference of two 32-bit counts has wrapped, which
         * happens once the true interval passes 2^31 seconds - 68 years, and
         * outside anything the chip can even express for a single machine.  It
         * is reported as what it is rather than hidden, because a number that
         * silently becomes a different number is worse than one that stops.
         *
         * The baseline is still printed: it is what tells the two apart. */
        out_str("uptime: no interval to report: the clock now reads EARLIER");
        out_str(" than at boot, or the interval is over 2^31 seconds (68");
        out_str(" years).  The two are the same number here.");
        out_end();
        print_since(&boot);
        return 1;
    }

    print_elapsed(seconds);
    print_since(&boot);
    return 0;
}
