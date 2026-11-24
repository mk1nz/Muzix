/*
 * loadavg - print how busy the CPU has been, once, and exit
 *
 * The point measurement, where apps/top.c is the display: top averages over
 * three windows and redraws every two seconds, this reads the history once,
 * prints a line and is gone - so it costs a program slot and no time.
 *
 * WHAT IT PRINTS
 * --------------
 * Three smoothed fractions of wall time that the CPU was not idle, to two
 * decimals. Not the textbook load average, and the name is the only concession
 * to habit here: that is the number of processes waiting for the processor,
 * which on this machine is not something anyone can measure - see
 * "Do not buy room" and the scheduler notes in AGENTS.md.  What can be measured
 * is time, and time is what the kernel accounts.
 *
 * The accounting is the kernel's, entirely.  muzix_proc_table_charge() charges a
 * process's two counters at both ends of every system call, so between two
 * charges the counters tick at the CTC's 120 Hz, and the kernel records the
 * clock value it saw at start-up so the boot instant is recoverable from
 * userspace.  This program only divides the one by the other; there is no
 * sampling, no history and nothing to average, because it is a program and not a
 * scheduler.
 *
 * THE THREE WINDOWS, WHICH IT USED NOT TO HAVE
 * ------------------------------------------
 * This printed one number for a long time, on a stated reason: "a single reading
 * cannot have a window it did not watch, and printing three numbers here would be
 * three copies of the same measurement with different labels on them."  That was
 * correct when it was written and it is the reason the program is called loadavg
 * while printing one number, which is not a small inconsistency to have shipped.
 *
 * The reason is gone.  SYS_LOAD exists now, and the three windows are the
 * kernel's own history under 1, 5 and 15 minute constants - three different time
 * constants over the same record, not three labels on one reading.  They diverge:
 * after two minutes with the CPU idle the 1-minute window has fallen to 0.14 and
 * the 15-minute window is still at 0.88.  kernel/test_load.c asserts that spread,
 * because asserting only the ordering would let a module that returned three
 * identical numbers pass.
 *
 * The boot figure is process-only CPU utilization: process-accounted UTIME+STIME
 * divided by wall time. It excludes kernel work outside process accounting, so
 * it is not a whole-machine CPU busy fraction and must not be compared directly
 * with the three SYS_LOAD windows.
 *
 * IT WILL NOT READ YOUR TERMINAL TIL DONE
 * --------------------------------------
 * It prints and exits; it does not block.  It cannot be interrupted either - the
 * kernel delivers no signals from the keyboard - so there is nothing here to
 * wait for.  The exit is the exit.
 *
 * NO `/`, NO `%`, NO 32-BIT MULTIPLY
 * ----------------------------------
 * All three have cost this program its only output before it printed any:
 *
 *   - `/` and `%` compile to __divuint and __moduint.  They DO resolve -
 *     lib/math.s defines them and every app map file lists them - but they
 *     used to return the result in HL while the compiled caller reads DE, so
 *     a division through them was a silent wrong answer.  The practice of
 *     avoiding them stands on that, not on a missing definition.
 *   - a 32-bit multiply is __mullong, which resolves and is enormous;
 *   - a 32-bit shift by a VARIABLE count is a third route to __mullong.
 *
 * So the ratio is a shift by a literal and repeated subtraction, and the two
 * decimals are produced by shifts alone.  Both are the shapes apps/top.c uses
 * for the same quantity; they are spelled out again rather than shared, because
 * userspace here has no way to link a helper between two programs other than
 * adding it to libc, and this is eight lines of arithmetic.
 *
 * Adding the three windows cost none of it.  They arrive from the kernel already
 * in Q14, and print_q14() formats them directly - no divide, no multiply,
 * nothing that could reach a helper this link does not have.
 */

#include "../lib/libc.h"
#include "../lib/lcd.h"
#include "../lib/i2c.h"

#define Q14 16384u          /* the busy fraction, scaled: 1.0 == 16384 */

/* Q14 to "d.dd", by shifts only.
 *
 * The integer part is the top bits.  The fraction is 16384 counts across a
 * unit, and a hundredth is 164 of them; 164 is 2^7 + 2^5 + 2^2, which is the
 * only reason these three shifts appear.  Multiplying by 100 the way a C
 * programmer would reach for is the 32-bit multiply this file cannot afford.
 */
static void print_q14(uint32_t value)
{
    uint32_t fraction = value & (Q14 - 1u);
    uint32_t hundredths;

    putchar('0' + (int)(value >> 14));
    putchar('.');
    hundredths = ((fraction << 6) + (fraction << 5) + (fraction << 2)) >> 14;

    {
        uint32_t tens = 0;

        while (hundredths >= 10u) {
            hundredths -= 10u;
            tens++;
        }
        putchar('0' + (int)tens);
        putchar('0' + (int)hundredths);
    }
}

static void
print_q14_lcd(uint32_t value) {
    uint32_t fraction = value & (Q14 - 1u);
    uint32_t hundredths;
    uint32_t tens;

    lcd_print_uint((unsigned int)(value >> 14));
    lcd_print_char('.');
    hundredths = ((fraction << 6) + (fraction << 5) + (fraction << 2)) >> 14;

    tens = 0;
    while (hundredths >= 10u) {
        hundredths -= 10u;
        tens++;
    }
    lcd_print_uint((unsigned int)tens);
    lcd_print_uint((unsigned int)hundredths);
}

int
main(int argc, char *argv[]) {
    uint32_t windows[3];
    int lcd_mode = 0;

    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        lcd_mode = 1;
    }

    /* The three windows first, and unconditionally.  Everything below this can
     * fail - the clock, the sweep - and none of that failures should cost the
     * caller the figures they came for, so the headline is printed before any of
     * it is attempted rather than at the end of a sequence that may return early.
     *
     * A kernel that cannot answer is reported as three zeros rather than as a
     * line of stale numbers: a load figure that is quietly out of date is the one
     * thing a load display must not print. */
    if (sys_load(windows) != 0) {
        windows[0] = 0;
        windows[1] = 0;
        windows[2] = 0;
    }

    if (lcd_mode) {
        lcd_init();
        lcd_clear();
        lcd_print("loadavg:");
        lcd_set_cursor(0, 1);
        print_q14_lcd(windows[0]);
        lcd_print_char(' ');
        print_q14_lcd(windows[1]);
        lcd_print_char(' ');
        print_q14_lcd(windows[2]);
        return 0;
    }

    printf("loadavg: ");
    print_q14(windows[0]);
    printf(" ");
    print_q14(windows[1]);
    printf(" ");
    print_q14(windows[2]);
    printf("\n");

    return 0;
}
