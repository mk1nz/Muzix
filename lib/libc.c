#include "libc.h"
#include "syscall.h"
#include "tty_ioctl.h"

#include <stdarg.h>

/* String functions */
size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d != '\0') {
        d++;
    }
    while ((*d++ = *src++) != '\0');
    return dest;
}

/* Memory functions */
void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *c1 = (const unsigned char *)s1;
    const unsigned char *c2 = (const unsigned char *)s2;
    while (n--) {
        if (*c1 != *c2) {
            return *c1 - *c2;
        }
        c1++;
        c2++;
    }
    return 0;
}

/* Character functions */
int isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 'A';
    }
    return c;
}

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 'a';
    }
    return c;
}

/* Conversion functions */
int atoi(const char *nptr)
{
    int result = 0;
    int sign = 1;
    
    while (isspace(*nptr)) {
        nptr++;
    }
    
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    } else if (*nptr == '+') {
        nptr++;
    }
    
    while (isdigit(*nptr)) {
        result = result * 10 + (*nptr - '0');
        nptr++;
    }
    
    return result * sign;
}

long atol(const char *nptr)
{
    return (long)atoi(nptr);
}

char *itoa(int value, char *str, int base)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buffer[33];
    char *ptr = buffer + sizeof(buffer) - 1;
    int negative = 0;
    
    if (base < 2 || base > 36) {
        return NULL;
    }
    
    *ptr = '\0';
    
    if (value < 0) {
        negative = 1;
        value = -value;
    }
    
    do {
        *--ptr = digits[value % base];
        value /= base;
    } while (value);
    
    if (negative) {
        *--ptr = '-';
    }
    
    strcpy(str, ptr);
    return str;
}

int putchar(int c)
{
    unsigned char value = (unsigned char)c;
    return sys_write(1, &value, 1) == 1 ? c : -1;
}

/* Return one byte, waiting for it.
 *
 * getchar() is read() on the console with a one-byte buffer, and it goes
 * through read() rather than sys_read() so that it inherits the parking.  It
 * used to call sys_read() directly and map everything that was not exactly 1 to
 * -1, which meant an idle console produced -1 rather than a wait: the caller
 * could only treat that as "no character", so a program reading the console one
 * character at a time looped on it, and the processor was held for every one of
 * those iterations.  read() is the function that waits; this is the same call
 * with a shorter buffer.
 *
 * The EOF convention is unchanged and still the only non-byte answer: a
 * zero-length read is Ctrl-D, and that is -1 here. */
int getchar(void)
{
    unsigned char value;

    if (read(0, &value, 1) != 1) {
        return -1;
    }
    return value;
}

void puts(const char *str)
{
    while (*str) {
        putchar(*str++);
    }
    putchar('\n');
}

/*
 * Print an unsigned value in the given base.
 *
 * This counts digits down through a table of place values and takes each digit
 * by repeated subtraction, rather than using `/` and `%`.  That is not a
 * stylistic choice.  On this build `value % base` and `value / base` compile
 * to calls to __moduint and __divuint, and no module in any userspace link
 * line provides them - lib/libc.rel, lib/syscall.rel, lib/math.rel and
 * lib/sdcc_runtime.rel are all that get linked - so the calls went to address
 * 0, which on this platform is the ROM boot stub.  A program that printed a
 * number did not print a number: it ran the boot stub's `ldir` and jumped
 * back into kernel_main.
 *
 * That is why no %d appears anywhere in the existing userspace, and it is how
 * this went unnoticed.  Measured: `date` with a single %d in it printed
 * everything before the conversion and then hung, with the PC marching
 * linearly up through the boot stub.
 *
 * The tables cover the full range of a 16-bit unsigned int, which is the
 * widest value printf can be handed.
 */
static void print_unsigned(unsigned int value, unsigned int base)
{
    static const char digits[] = "0123456789abcdef";
    static const unsigned int places_dec[] = { 10000, 1000, 100, 10, 1 };
    static const unsigned int places_hex[] = { 4096, 256, 16, 1 };
    const unsigned int *places = (base == 16) ? places_hex : places_dec;
    unsigned int nplaces = (base == 16) ? 4u : 5u;
    char buffer[sizeof(unsigned int) * 2 + 1];
    char *out = buffer;
    unsigned int start;
    unsigned int i;

    /* The highest place the value reaches, so that 7 prints as "7" rather
     * than "00007".  The units place is always emitted, so the walk stops one
     * short of the end. */
    for (start = 0; start + 1u < nplaces && value < places[start]; start++) {
        continue;
    }

    /* Digits are appended most significant first and the result is then walked
     * forwards.  The previous version filled the buffer from the back towards
     * the front and printed forwards from the front, which emits the digits in
     * reverse: 19 printed as 91, 12345 as 54321, 1996 as 6991.  Single-digit
     * values and 0xff are their own reverses, so nothing that used to be
     * printed here would have shown it. */
    for (i = start; i < nplaces; i++) {
        unsigned int digit = 0;

        while (value >= places[i]) {
            value = (unsigned int)(value - places[i]);
            digit++;
        }
        *out++ = digits[digit];
    }
    *out = '\0';

    for (out = buffer; *out; out++) {
        putchar(*out);
    }
}

void printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            putchar(*fmt++);
            continue;
        }
        fmt++;
        switch (*fmt++) {
        case '%':
            putchar('%');
            break;
        case 'c':
            putchar(va_arg(args, int));
            break;
        case 's': {
            const char *value = va_arg(args, const char *);
            while (*value) {
                putchar(*value++);
            }
            break;
        }
        case 'd': {
            int value = va_arg(args, int);
            if (value < 0) {
                putchar('-');
                value = -value;
            }
            print_unsigned((unsigned int)value, 10);
            break;
        }
        case 'x':
            print_unsigned(va_arg(args, unsigned int), 16);
            break;
        default:
            putchar('?');
            break;
        }
    }
    va_end(args);
}

/* Process control - System Call Interface */
int fork(void)
{
    return sys_fork();
}

int exec(const char *path, const char *const *argv)
{
    return sys_exec(path, argv);
}

void exit(int status)
{
    sys_exit(status);
}

int wait(int *status)
{
    return sys_wait(status);
}

int getpid(void)
{
    return sys_getpid();
}

int getppid(void)
{
    return sys_getppid();
}

/* File operations - System Call Interface */
int open(const char *pathname, int flags)
{
    return sys_open(pathname, flags);
}

int close(int fd)
{
    return sys_close(fd);
}

/* read() blocks by parking, not by asking.
 *
 * The kernel read returns what it has and stops when it has nothing; blocking is a
 * loop HERE, in the process, because the point to continue at is in the process.  A
 * kernel that waited instead would be waiting WITH the processor, and the machine
 * would read as 100% busy while doing nothing - the condition this whole mechanism
 * exists to remove.
 *
 * Two seconds at a time rather than for ever, so a process woken for any other
 * reason - a signal, another process finishing - can make progress, and so that
 * timing out stays the business of interrupts and devices rather than of this
 * loop.
 *
 * WHAT TO PARK ON, which is the whole content of this loop and was wrong.
 *
 * The condition is `n < 0`, and it was `n != 0`.  The kernel answers "nothing
 * to give you right now" with -1, not 0: fs/tty_device.c returns -1 when its
 * poll found no byte (fs/test_tty_device.c cases 31-33 pin that), and
 * SYS_READ propagates it at kernel/syscalls.c:1046.  Zero is end of file, and
 * only end of file: a zero-length read is Ctrl-D, which is a fact about the
 * input and must reach the caller.
 *
 * So `n != 0` returned -1 to the caller on the first idle read, this loop was
 * never entered, and nothing on this machine ever parked.  The spin was not in
 * the kernel and not in the park path - it was a caller's `while (got < 0)`
 * around a read() that had already decided not to wait.  Every tick went
 * unbooked because the interval was charged, and the figure read 1.00 from
 * before the first command to after the last.
 *
 * A negative answer is not only "try again" though: a bad descriptor is -1 as
 * well, and parking on that would hang the caller for ever on a mistake that
 * will never fix itself.  MUZIX_READ_AGAIN is the kernel's "nothing yet" and it
 * is a distinct value from the errors, so that is what is parked on, and a real
 * error still returns. */
int read(int fd, void *buf, size_t count)
{
    int n;

    for (;;) {
        n = sys_read(fd, buf, count);
        if (n == MUZIX_READ_AGAIN) {
            (void)sys_park(240);
            continue;
        }
        return n;
    }
}

int write(int fd, const void *buf, size_t count)
{
    return sys_write(fd, buf, count);
}

int seek(int fd, int offset)
{
    return sys_seek(fd, offset, 0);
}

/* Directory operations - System Call Interface */
int mkdir(const char *pathname, int mode)
{
    return sys_mkdir(pathname, mode);
}

int rmdir(const char *pathname)
{
    return sys_rmdir(pathname);
}

int chdir(const char *path)
{
    return sys_chdir(path);
}

char *getcwd(char *buf, size_t size)
{
    return sys_getcwd(buf, size);
}

int readdir(int fd, int index, struct dirent *entry)
{
    return sys_readdir(fd, index, entry);
}

int unlink(const char *pathname)
{
    return sys_unlink(pathname);
}

int rename(const char *oldpath, const char *newpath)
{
    return sys_rename(oldpath, newpath);
}

int stat(const char *pathname, void *statbuf)
{
    return sys_stat(pathname, statbuf);
}

void sync(void)
{
    sys_sync();
}

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
 * __divsint, and lib/math.s leaves the result in HL while SDCC's generated
 * caller reads D and E - the same disagreement that file already documents and
 * works around for __mulint, applied to only one of the five.  (Its divide loop
 * also compares against the divisor's low byte alone, so a divisor of 400
 * divides by 144.) The previous time() implementation did not exercise this
 * conversion, so the day count had not been executed on the target before the
 * RTC-backed time and uptime paths were added.
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

static int32_t muzix_days_from_utc(int year, int month, int day)
{
    int32_t days = 0;
    int32_t leap = ((year & 3) == 0) ? 1 : 0;
    int y;

    for (y = MUZIX_RTC_YEAR_BASE; y < year; y++) {
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
static int32_t muzix_scale16(uint16_t value, int32_t factor)
{
    int32_t acc = 0;
    int i;

    for (i = 15; i >= 0; i--) {
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
static int32_t muzix_rtc_seconds(const struct rtc_time *t)
{
    int32_t days = muzix_days_from_utc(MUZIX_RTC_YEAR_BASE + t->year, t->month,
                                       t->day);
    /* Minutes past midnight, 0..1439, scaled to seconds without the 16-bit
     * multiply that `hour * 3600` becomes - see muzix_scale16(). */
    int32_t minutes = muzix_scale16(t->hour, 60) + (int32_t)t->minute;

    return muzix_scale16((uint16_t)days, 86400) + muzix_scale16((uint16_t)minutes, 60) +
           (int32_t)t->second;
}

int time(int32_t *seconds)
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
int uptime(int32_t *seconds, struct rtc_time *boot)
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

int get_rtc(struct rtc_time *out)
{
    if (!out) {
        return -1;
    }
    return sys_getrtc(out) < 0 ? -1 : 0;
}

/*
 * Has anybody typed anything?  See the note in lib/libc.h.
 *
 * Descriptor 0, not 1.  All three of the console descriptors the kernel opens
 * at boot are the same device, so either would do, and 0 is the one that means
 * input.  A program with no console of its own still gets the console's answer,
 * which on this machine is the same one: there is one tty.
 *
 * The count the device returns is a count of places, not of bytes, so it is
 * reduced to the yes or no the name promises.  The negative is passed through
 * rather than folded into "no": a caller that cannot ask has not been told
 * there is nothing, and the two answers are worth telling apart.
 */
int tty_input_pending(void)
{
    uint16_t pending = 0;

    if (sys_ioctl(0, MUZIX_TTY_IOCTL_PENDING, &pending) != 0) {
        return -1;
    }
    return pending != 0;
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
static int rtc_time_is_valid(const struct rtc_time *t)
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

int set_rtc(struct rtc_time *in)
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

int system(const char *command)
{
    int pid = fork();
    if (pid == 0) {
        const char *argv[] = {"shell", "-c", (char *)command, NULL};
        exec("/bin/shell", argv);
        exit(1);
    } else if (pid > 0) {
        int status = 0;
        wait(&status);
        return status;
    }
    return -1;
}
