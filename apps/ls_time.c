/*
 * ls_time.c - epoch-to-text conversion for the `ls` app's long format.
 *
 * Split out of ls.c so that the date-formatting logic can be exercised by a
 * host test (format_epoch only touches <stdint.h> and <string.h>, so it links
 * on the host without syscall stubs).
 *
 * Written for SDCC -mz80, where overlapping local automatic variables is a
 * real risk: the computed temporaries below are therefore `static`.  On the
 * host (gcc -std=c99) they are simply file-scope locals and behave identically.
 */

#include "ls_time.h"
#include <string.h>

void format_epoch(uint32_t epoch, char *buf) {
    static const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    static const char digits[] = "0123456789";
    /* Static temporaries: SDCC -mz80 overlaps local variables, so all
     * computed values go through _DATA to avoid corruption. */
    static uint16_t s_epoch_lo;
    static uint16_t s_epoch_hi;
    static uint16_t s_days;
    static uint8_t s_month;
    static uint8_t s_day;
    static uint8_t s_hour;
    static uint8_t s_min;
    static uint8_t s_sec;
    static uint16_t s_year;
    static uint8_t s_leap;
    static uint8_t s_tmp;

    if (epoch == 0) {
        strcpy(buf, "--.--.---- --:--:--");
        return;
    }

    /* Split epoch into 16-bit halves */
    s_epoch_lo = (uint16_t)(epoch & 0xFFFF);
    s_epoch_hi = (uint16_t)(epoch >> 16);

    /* Subtract 86400 seconds per day.
     * 86400 = 0x00015180 (low=0x5180, high=0x0001). */
    s_days = 0;
    while (s_epoch_hi > 0 && (s_epoch_hi > 1 || s_epoch_lo >= 0x5180U)) {
        if (s_epoch_lo >= 0x5180U) {
            s_epoch_lo -= 0x5180U;
            s_epoch_hi -= 1U;
        } else {
            /* Low borrow: subtract 0x5180 from (hi:lo).
             * s_epoch_lo += 0x10000 - 0x5180 = 0xAE80
             * borrow 1 from high, plus the day's high byte. */
            s_epoch_lo += 0xAE80U;
            s_epoch_hi -= 2U;  /* borrow + day high */
        }
        s_days++;
    }

    /* Remaining seconds in day = s_epoch_hi * 65536 + s_epoch_lo.
     * The loop above leaves s_epoch_hi in {0, 1}.  When it is 1, the
     * remaining seconds (>= 65536) cannot fit in s_epoch_lo alone, so
     * fold the carry into the hours extraction:
     *   65536 = 18 * 3600 + 736
     * Max s_epoch_lo at exit is 0x517F (20863), so 736 + 20863 = 21599
     * fits in uint16_t.  This must happen before the subtraction loops
     * below consume s_epoch_lo. */
    /* Extract hours */
    s_hour = 0;
    if (s_epoch_hi > 0) {
        s_hour = 18U;          /* floor(65536 / 3600)  */
        s_epoch_lo += 736U;    /*  65536 % 3600  */
    }
    while (s_epoch_lo >= 3600U) { s_epoch_lo -= 3600U; s_hour++; }
    /* Extract minutes */
    s_min = 0;
    while (s_epoch_lo >= 60U) { s_epoch_lo -= 60U; s_min++; }
    s_sec = s_epoch_lo;

    /* Compute year */
    s_year = 2000;
    while (s_days >= 365U) {
        s_leap = ((s_year % 4U == 0) && (s_year % 100U != 0)) || (s_year % 400U == 0);
        if (s_days >= 365U + s_leap) {
            s_days -= 365U + s_leap;
            s_year++;
        } else {
            break;
        }
    }

    /* Compute month */
    s_month = 0;
    while (s_month < 12U) {
        s_tmp = days_in_month[s_month];
        /* In a leap year, February has 29 days instead of 28 */
        if (s_month == 1U && (((s_year % 4U == 0) && (s_year % 100U != 0)) || (s_year % 400U == 0))) {
            s_tmp = 29U;
        }
        if (s_days >= s_tmp) {
            s_days -= s_tmp;
            s_month++;
        } else {
            break;
        }
    }
    s_day = (uint8_t)(s_days + 1U);

    /* Format DD */
    buf[2] = '.';
    s_tmp = s_day;
    if (s_tmp >= 30U) { buf[0] = '3'; s_tmp -= 30; }
    else if (s_tmp >= 20U) { buf[0] = '2'; s_tmp -= 20; }
    else if (s_tmp >= 10U) { buf[0] = '1'; s_tmp -= 10; }
    else { buf[0] = '0'; }
    buf[1] = digits[s_tmp];

    /* Format MM */
    buf[5] = '.';
    s_tmp = s_month + 1U;
    if (s_tmp >= 10U) { buf[3] = '1'; s_tmp -= 10; }
    else { buf[3] = '0'; }
    buf[4] = digits[s_tmp];

    /* Format YYYY via subtraction (avoids SDCC uint8_t * 1000 mul bug) */
    buf[10] = ' ';
    s_tmp = 0;
    while (s_year >= 1000U) { s_year -= 1000U; s_tmp++; }
    buf[6] = digits[s_tmp];
    s_tmp = 0;
    while (s_year >= 100U) { s_year -= 100U; s_tmp++; }
    buf[7] = digits[s_tmp];
    s_tmp = 0;
    while (s_year >= 10U) { s_year -= 10U; s_tmp++; }
    buf[8] = digits[s_tmp];
    buf[9] = digits[s_year];

    /* Format HH:MM:SS */
    buf[13] = ':';
    buf[16] = ':';

    s_tmp = s_hour;
    if (s_tmp >= 20U) { buf[11] = '2'; s_tmp -= 20; }
    else if (s_tmp >= 10U) { buf[11] = '1'; s_tmp -= 10; }
    else { buf[11] = '0'; }
    buf[12] = digits[s_tmp];

    s_tmp = s_min;
    if (s_tmp >= 50U) { buf[14] = '5'; s_tmp -= 50; }
    else if (s_tmp >= 40U) { buf[14] = '4'; s_tmp -= 40; }
    else if (s_tmp >= 30U) { buf[14] = '3'; s_tmp -= 30; }
    else if (s_tmp >= 20U) { buf[14] = '2'; s_tmp -= 20; }
    else if (s_tmp >= 10U) { buf[14] = '1'; s_tmp -= 10; }
    else { buf[14] = '0'; }
    buf[15] = digits[s_tmp];

    s_tmp = s_sec;
    if (s_tmp >= 50U) { buf[17] = '5'; s_tmp -= 50; }
    else if (s_tmp >= 40U) { buf[17] = '4'; s_tmp -= 40; }
    else if (s_tmp >= 30U) { buf[17] = '3'; s_tmp -= 30; }
    else if (s_tmp >= 20U) { buf[17] = '2'; s_tmp -= 20; }
    else if (s_tmp >= 10U) { buf[17] = '1'; s_tmp -= 10; }
    else { buf[17] = '0'; }
    buf[18] = digits[s_tmp];

    buf[19] = 0;
}
