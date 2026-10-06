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
 *   - `/` and `%` compile to __divuint and __moduint, which no module in any
 *     userspace link provides.  The call goes to address 0, which on this
 *     platform is the ROM boot stub, and the program runs the stub's `ldir` and
 *     jumps back into kernel_main.  The symptom is a program that prints
 *     NOTHING, which reads as a hang rather than a link error.  Written down in
 *     lib/libc.c.
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
 * in Q14, and print_q14() is the same function that formatted the boot figure, so
 * a second number line is a second call and no new arithmetic - no divide, no
 * multiply, nothing that could reach a helper this link does not have.
 */

#include "../lib/libc.h"

#define Q14 16384u          /* the busy fraction, scaled: 1.0 == 16384 */

/* spent / capacity, as Q14, by subtraction.
 *
 * `spent` is clamped to `capacity` by the caller, so the quotient is at most
 * Q14 and the loop runs at most 16384 times - and the interval is a boot, not a
 * frame, so it runs exactly once.
 */
static uint32_t busy_fraction(uint32_t spent, uint32_t capacity)
{
    uint32_t scaled = spent << 14;
    uint32_t whole = 0;

    if (capacity == 0) {
        return 0;
    }
    while (scaled >= capacity) {
        scaled -= capacity;
        whole++;
    }
    return whole;
}

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

    /* And the last two digits by subtraction rather than by / and % on a
     * constant - a constant divisor is still a divisor, and ten is not special
     * to the compiler on a target where division is a call to nowhere.
     * hundredths is under 100, so this is at most nine iterations. */
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

int main(int argc, char *argv[])
{
    struct proc_info info[MUZIX_PROC_INFO_SLOTS];
    uint32_t windows[3];
    uint32_t charged = 0;
    uint32_t capacity = 0;
    uint32_t t;
    int32_t seconds;
    int slot;

    (void)argc;
    (void)argv;

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
    printf("loadavg: ");
    print_q14(windows[0]);
    printf(" ");
    print_q14(windows[1]);
    printf(" ");
    print_q14(windows[2]);
    printf("\n");

    /* Every slot, the same sweep apps/top.c does.  A slot that cannot be read is
     * a slot that has never run, and is left as zero rather than as whatever the
     * buffer held before: a stale counter would be worse than no counter,
     * because it would make this program's number wrong in a way nothing else
     * would correct.
     *
     * The kernel slot is charged nothing of its own, so including it changes
     * nothing; it is summed anyway rather than excluded by a test, so that
     * whatever it is charged to, it is counted here. */
    for (slot = 0; slot < MUZIX_PROC_INFO_SLOTS; slot++) {
        if (sys_proctab(&info[slot], slot) != 0) {
            continue;
        }
        charged += (uint32_t)info[slot].user_time +
                   (uint32_t)info[slot].sys_time;
    }

    if (uptime(&seconds, 0) != 0 || seconds <= 0) {
        printf("process-only CPU utilization unavailable: the clock did not answer at boot, or has been set\n");
        printf("            back since - the same thing uptime reports\n");
        return 1;
    }

    /* capacity is the boot interval in ticks, accumulated rather than
     * multiplied: 32-bit multiply is __mullong.  seconds is an int32 of wall
     * seconds and cannot be negative here. */
    for (t = 0; t < (uint32_t)seconds; t++) {
        capacity += MUZIX_PROC_TICK_HZ;
    }
    if (charged > capacity) {
        charged = capacity;   /* a counter that wrapped, or one that never started at zero */
    }

    printf("process-only CPU utilization since boot: ");
    print_q14(busy_fraction(charged, capacity));
    printf("\n");
    return 0;
}
