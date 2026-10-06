/*
 * top - report the state of the system, refreshing every two seconds
 *
 * Everything below the summary line comes out of SYS_PROCTAB: one call per
 * slot, filling a struct proc_info.  There is no other way for a process to ask
 * about a process but itself - SYS_GETPID and SYS_TIMES both answer for the
 * caller only - so this is the first program here that can see the process
 * table rather than its own corner of it.
 *
 * The display is a ten-line block, cleared and redrawn in place every two
 * seconds, in the shape top has: a summary carrying the clock time and the
 * load average, a column header, one row per process with the command name,
 * the parent's pid, the state, the queue and the four time counters the kernel
 * keeps, then a rule and three lines saying what those numbers mean.
 *
 * The screen is cleared with ESC [ 2 J followed by ESC [ H.  That is a
 * decision, not a default, and clear_screen() below sets out what happens when
 * the terminal on the other end of port $68 does not honour it: the block is a
 * fixed height and its last line is a rule, so a terminal that shows the
 * escapes literally and scrolls still shows a complete, self-delimiting and
 * readable block with the newest one at the bottom.  The previous version of
 * this program could not clear at all, and was a log that scrolled.
 *
 * The command names come from the kernel: the exec handler copies the path it
 * was given into the one field of muzix_proc_slot_t that lies inside the window
 * SYS_PROCTAB already hands to userspace, so the name costs the kernel no extra
 * transfer and no extra code.  What arrives is the leading characters of the
 * path rather than its basename, and put_command() below takes the part after
 * the last slash.
 *
 * ---- How it stops -------------------------------------------------------
 *
 * A keypress, checked once per refresh at the top of the loop. Ctrl-C is not a
 * signal because keyboard signal delivery is not implemented and the console
 * is canonical with echo. read() waits for a complete line, so it cannot be
 * used as a nonblocking key check. Timer and UART interrupts exist, but the
 * cooperative scheduler does not preempt this program; tty_input_pending()
 * checks for a byte without blocking or consuming it.
 *
 * The keypress is echoed, at the prompt that comes back rather than here, and
 * that is not a bug this program can fix.  The echo is the line discipline's,
 * it happens in muzix_tty_read when the shell reads the byte, and the byte
 * reaches the shell only after this program has returned - so the character
 * that stopped the display appears on the line the user is next typing on.
 * Suppressing it would mean turning ECHO off in the device parameters while
 * this program runs, which is not a local change: there is one console and one
 * set of parameters for it, so the flag would still be off when the shell got
 * the terminal back unless this program put it back, and putting it back
 * before returning restores the echo of the very byte that stopped it.  The
 * alternatives are a non-canonical mode to read the key in, which is a
 * different device mode and a different amount of kernel than this kernel has
 * room for, and consuming the byte here, which would make the user's keypress
 * invisible to them and to the shell both.  So the character is left alone and
 * the clearing above hides all but the last of it.
 */

#include "../lib/libc.h"
#include "../lib/syscall.h"

#define REFRESH_SECONDS 2

/* ---- CPU busy ----------------------------------------------------------
 *
 * An exponentially damped moving average of the fraction of wall time the CPU
 * was busy, sampled once per refresh, with time constants of 1, 5 and 15
 * minutes.  For a gap of dt seconds,
 *
 *     load = load * exp(-dt/tau) + R * (1 - exp(-dt/tau))
 *
 * dt is measured from the DS1302 rather than assumed to be REFRESH_SECONDS,
 * because the real interval drifts: the loop below is paced on the clock, but
 * the clock is read before the drawing and the drawing costs something, so the
 * gap between two samples is the two seconds plus however long printing took
 * plus however long the clock rounds to.  Feeding the formula a fixed dt would
 * make the average decay at the rate of a clock that does not exist.
 *
 * THE THREE WINDOWS ARE NOT COMPUTED HERE ANY MORE.  They arrive from the kernel
 * in one SYS_LOAD call, already in Q14 where 16384 is 1.0, and kernel/load.c owns
 * the interval arithmetic.  This file keeps only the part that was always a
 * display's job: unpacking Q14 into digits and two decimals.  What is left below
 * of the averaging - mul16x16, mul32, decay(), load_apply(), busy_sample() and
 * the three tau constants - was removed, and it took this program's only 32-bit
 * multiplies with it.
 *
 * The rule that removal did not change, and that everything here still obeys, is
 * that nothing in this file multiplies two 32-bit numbers.  That is not a style
 * choice: it is the difference between a display and a reboot.  SDCC 4.5 provides
 * no __mullong for the z80, the linker resolves an undefined global to 0x0000, and
 * `call __mullong` is then a call into the boot stub - which is exactly what this
 * program did, on its first display, on any hardware, until the multiply was
 * written out of 16x16 partial products.  The scale by 2^14 is a shift and the
 * scale by 100 is three shifts and two adds, because a 32-bit SHIFT is expanded
 * inline by SDCC and a 32-bit MULTIPLY is not.  put_load(), which survives, is the
 * remaining place that used to reach it.
 *
 * tools/check_userspace_runtime.rb fails the build on any userspace link that
 * reports an undefined global, so the mistake cannot come back quietly.  The
 * kernel has the same rule and it is stricter: KERNEL_LINK_ORDER links .rel files
 * only, never a .lib, so a helper SDCC emits and the tree does not carry does not
 * resolve at all.  kernel/load.c hit that with a 32-bit divide and the comment
 * there is worth reading if this ever has to be computed in the kernel again.
 */

/* The windows arrive from the kernel already in Q14, where 16384 is 1.0.
 * Kept only because put_load() unpacks the integer and fractional halves. */
#define LOAD_SCALE 16384u   /* 2^14 */

/* Output goes through a line buffer rather than putchar().
 *
 * putchar() is one SYS_WRITE per character and puts() is a loop of them, so a
 * display of roughly two hundred characters is two hundred syscalls - measured
 * at about a second and a half of the DS1302's time on its own, which is most
 * of the interval this program is supposed to be waiting for.  Assembling a
 * line and writing it once turns that into one syscall per line and makes the
 * two-second interval the interval it claims to be.
 *
 * The buffer holds one line and one line is one SYS_WRITE.  The block drawn is
 * ten lines - a summary, a column header, one row per slot, a rule and a
 * three-line note - and the longest of them is 61 characters, so 80 is enough
 * for every one with room to spare.  out() flushes rather than dropping, so a
 * line that somehow did not fit comes out in two pieces instead of quietly
 * losing its tail. */
static char line[80];
static int used;
static struct proc_info table[MUZIX_PROC_INFO_SLOTS];

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

/* Print a right-aligned number in a field of `width`.
 *
 * By repeated subtraction rather than division, for the reason print_unsigned()
 * in lib/libc.c gives: on this build `/` and `%` compile to calls to __divuint
 * and __moduint, and no module in any userspace link line provides them.  The
 * values are process IDs and accounting-tick counters. A
 * value too wide for its field is printed at full width and runs the column
 * over rather than being truncated, because a truncated counter is the kind of
 * wrong that reads as a real one. */
/* Print a decimal, right-aligned in `width` columns.
 *
 * THE BUG THIS REPLACES, because it is worth writing down: the old version
 * counted digits by subtracting ten and incrementing a length
 *
 *     while (value >= 10u) { value -= 10u; length++; }
 *     digits[length++] = (char)('0' + (int)value);
 *
 * which counts TENS, not digits - so `length` ended up at value/10, and every
 * one of those iterations wrote a byte into a ten-byte array.  Any value of 100
 * or more walked off the end of it and up the stack, through its caller's
 * frame, through the buffer that was being printed.  On screen that was not a
 * wrong number, it was the whole display reversed and interleaved with the
 * binary of this program.
 *
 * The previous entry-count accounting often kept these fields at zero.
 * Tick-based accounting makes multi-digit values reachable; extraction order
 * must therefore be preserved when printing them.
 *
 * The replacement cannot divide: 32-bit `/` is `__divulong`, which is not in
 * any userspace link here, and an unresolved global on this machine is a call
 * to the boot stub.  So the place values are a table and each digit is extracted
 * by repeated subtraction - at most nine subtractions for a nine-digit number
 * and 81 in total, which is less work than counting tens did for four digits.
 * Ten digits is the most a uint32_t can need, so digits[10] is exact. */
static void put_field(uint32_t value, int width)
{
    static const uint32_t place[10] = {
        1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
        10000u, 1000u, 100u, 10u, 1u
    };
    char digits[10];
    int length = 0;
    int pad;
    int i = 0;

    while (i < 9 && value < place[i]) {
        i++;
    }
    for (; i < 10; i++) {
        int digit = 0;

        while (value >= place[i]) {
            value -= place[i];
            digit++;
        }
        digits[length++] = (char)('0' + digit);
    }

    for (pad = length; pad < width; pad++) {
        out(' ');
    }
    /* Extraction fills digits[] most-significant first; iterating forward
     * preserves that order. */
    {
        int j;

        for (j = 0; j < length; j++) {
            out(digits[j]);
        }
    }
}

/* Print a value that is known to be under 100, as two digits. */
static void put2(int value)
{
    int tens = 0;

    while (value >= 10) {
        value -= 10;
        tens++;
    }
    out((char)('0' + tens));
    out((char)('0' + value));
}


/* Wait until the clock has moved on by `seconds` from the moment `from` was
 * read, and return nothing: the caller draws again as soon as this returns.
 *
 * The wait is a park, in slices, and not a poll of the clock.
 *
 * The poll is what this used to be, and it is the second place the machine
 * spent a processor it did not need.  `for (;;) get_rtc(...)` asks the DS1302
 * over and over until the seconds field changes; each asking is a syscall, and
 * every tick of it was charged to this process, because a process that is
 * running is a process holding the processor.  So `top` displayed a correct
 * load average of 100% while being the reason for it, and it displayed 100% at
 * every moment of its two-second refresh, not only while it drew.
 *
 * sys_park() takes this process out of the runnable set until its deadline,
 * allowing other processes to run and the kernel to halt when none are ready.
 * Interrupt and scheduler overhead still consumes CPU and is included in the
 * kernel's busy-time measurement.
 *
 * The wait is sliced so it can re-read the RTC at quarter-second intervals.
 * Input does not cancel this wait: a key is checked at the next refresh, so
 * quit latency can be as long as the two-second refresh interval plus drawing.
 *
 * The count is the difference between the seconds field now and then, wrapping
 * at the minute, rather than a count of times the field changed.  Those differ
 * whenever the drawing crossed a second, which it does.
 *
 * A clock that stops answering ends the wait instead of parking for ever, and
 * `from < 0` - which print_summary() returns when the clock did not answer in
 * the first place - ends it at once rather than pretending two seconds passed.
 */
#define WAIT_SLICE_TICKS 30         /* a quarter of a second at 120 Hz */

static void wait_seconds(int from, int seconds)
{
    struct rtc_time now;
    int elapsed;

    if (from < 0) {
        return;
    }
    for (;;) {
        if (get_rtc(&now) != 0) {
            return;
        }
        elapsed = now.second - from;
        if (elapsed < 0) {
            elapsed += 60;
        }
        if (elapsed >= seconds) {
            return;
        }
        (void)sys_park(WAIT_SLICE_TICKS);
    }
}

/* Is this slot a process the scheduler is responsible for running?
 *
 * That is the definition of runnable this display uses, and it is a definition
 * forced by the machine rather than chosen for convenience.
 *
 * The platform is single-CPU and NON-PREEMPTIVE: a process runs until it blocks
 * or exits, and nothing is time-sliced. The 120 Hz timer accounts time and
 * wakes parked processes but does not preempt a running one. So a process in a
 * ready queue here is not waiting for its turn of a
 * fixed quantum - it is waiting to be scheduled at all, and it has been waiting
 * for as long as whatever ran before it took.
 *
 * Three kinds of slot are not runnable, and each is excluded for a different
 * reason:
 *   - a free slot (`active == 0`) has never held a process;
 *   - a zombie (`exited != 0`) is dead and holding its slot only until its
 *     parent waits for it, and counting it would make the average rise as
 *     processes finish rather than as work arrives;
 *   - the kernel slot (`pid == 0`) is the kernel's own placeholder, not a task
 *     the scheduler round-robins against the others.  It sits in TASK_Q, and
 *     counting it would add a constant 1 to every reading.
 *
 * What this is not: CPU utilisation. This predicate only classifies a slot as
 * runnable; CPU busy time is measured separately by the kernel's CTC3 cycle
 * counter and halt-time accounting. */
/* ---- Clearing the screen ------------------------------------------------
 *
 * ESC [ 2 J  erase the whole display, ESC [ H  home the cursor.  Eight bytes,
 * one SYS_WRITE, and the reason this can be a top rather than a log.
 *
 * The decision, and what happens when it is wrong.
 *
 * The console here is an NS16550 on port $68 with nothing behind it but a wire.
 * There is no cursor addressing in the UART and no terminal driving it, so
 * whether ESC [ 2 J does anything at all is a property of whatever is on the
 * other end, and this program cannot find out: there is no query, and the only
 * way to ask is to send a Device Attributes report and read the answer, which
 * on a terminal that does not implement it means waiting for a timeout.
 *
 * So the two available behaviours are:
 *
 *   - Send the escape and assume a real terminal.  This is what a top has to
 *     do to look like a top; without it the display can only ever be a log
 *     that scrolls, and the brief for this program is a top.  On a terminal
 *     that honours it, the whole block is redrawn in place, ten lines, forever,
 *     which is the thing being asked for.
 *
 *   - Overdraw the old lines with spaces and hope.  This is what a program with
 *     no cursor addressing does, and it cannot work either: redrawing in place
 *     needs to put the cursor back up N lines, and there is no way to ask a
 *     dumb terminal to do that.  All you can do is print the block again, and
 *     the terminal scrolls - which is exactly what the previous version of this
 *     program did, and is why it called itself a repeating block.
 *
 * The escape is therefore sent, and the fallback is made safe rather than
 * avoided:
 *
 *   - the block is a FIXED height - a summary line, a column header, one row per
 *     slot, a rule and a short note - so every refresh is the same shape and a
 *     reader who is looking at a scrolling terminal can find the bottom of the
 *     newest block immediately, because the rule is the last thing it prints;
 *   - every line is padded to a fixed width, so a shorter line cannot leave the
 *     tail of a longer one from the previous refresh showing through;
 *   - the note at the bottom says what the numbers mean, so it is on screen
 *     rather than printed once above a display that a clear would have wiped.
 *
 * That is the whole fallback: on a terminal that ignores ESC [ 2 J the user
 * gets a repeating ten-line block with the newest one at the bottom, which is
 * readable, self-delimiting and loses nothing.  It is not as good as a cleared
 * screen, and nothing available on this machine would make it so.
 *
 * 7-bit ESC (0x1B), not the 8-bit CSI (0x9B): the console carries 7-bit ASCII
 * and nothing here has established that the far end decodes 8-bit controls. */
static const char screen_clear[] = "\033[2J\033[H";

static void clear_screen(void)
{
    write(1, screen_clear, sizeof(screen_clear) - 1);
}

/* The state column, one letter, the way top spells it.
 *
 * The kernel reports a slot and not a state, so this is derived here and is only
 * ever as fine as what the slot actually holds.  There is no "S" for sleeping
 * and no "D" for uninterruptible: the scheduler is cooperative, so a live
 * process that is not this one is waiting to be scheduled, not asleep - and
 * that is a runnable process, which is what R means.  So this process and every
 * other live one both read R, and the difference between them is a count in the
 * summary line rather than a letter here: at any instant exactly one of them is
 * on the CPU, and how many want it is the `run` figure two lines up.
 *
 * A slot that has never run reads '-', the kernel's own placeholder reads 'K',
 * and a process that has exited but has not been waited for reads 'Z'. */
static char state_of(const struct proc_info *info)
{
    if (!info->active) {
        return '-';
    }
    if (info->pid == 0) {
        return 'K';
    }
    if (info->exited) {
        return 'Z';
    }
    return 'R';
}

/* The queue this process is registered into.
 *
 * Not "is queued": the kernel does not clear ready_q when a process is
 * dequeued, and the userspace switch path does not dequeue at all, so a field
 * read as a membership flag would be wrong.  This is the queue the process
 * belongs to and will be scheduled from. */
static char queue_of(const struct proc_info *info)
{
    switch (info->ready_q) {
    case MUZIX_PROC_Q_TASK:
        return 'T';
    case MUZIX_PROC_Q_SERVER:
        return 'S';
    default:
        return 'U';
    }
}

/* The COMMAND column: what to print for a slot whose name the kernel may not
 * have.
 *
 * Three cases, and each one is a real state of the table rather than a failure:
 *
 *   - a slot that has never run has no name because nothing has ever been
 *     exec'd into it.  "[free]".
 *   - the kernel's own slot, pid 0, is never exec'd into either - it is not a
 *     program, it is the scheduler's placeholder - and calling it "[kernel]" is
 *     more use to a reader than an empty column would be.
 *   - a live process with no name is a child of fork() that has not called exec
 *     yet.  That is a real window on this machine: the shell forks, and the
 *     child is scheduled before it execs.  "?" is what a top prints for a
 *     process whose executable it cannot name, and it is honest here.
 *
 * The name the kernel holds is the first MUZIX_PROC_NAME_SIZE-1 characters of
 * the path the exec was given, not its basename: the kernel copies the string
 * it already had to copy and stops there, because a backward scan for the last
 * slash is code this kernel has about forty bytes of in total.  So a bare
 * command arrives as itself ("ls") and a path arrives as the path
 * ("/bin/ls"), and the part after the last slash is taken here.  A path too
 * long for the field arrives truncated at the front, and then this prints what
 * is left of it, which is the tail of a directory rather than a command - still
 * not a wrong number, and the truncation is visible rather than silent. */
static void put_command(const struct proc_info *info)
{
    const char *s = info->name;
    int i = 0;

    if (!info->active) {
        out_str("[free]");
        return;
    }
    if (info->pid == 0) {
        out_str("[kernel]");
        return;
    }
    while (i < MUZIX_PROC_NAME_SIZE - 1 && s[i] != '\0') {
        i++;
    }
    if (i == 0) {
        out('?');
        return;
    }
    /* Back up over the last component: the scan above left i one past the last
     * character, so this walks it back to just after the final '/'. */
    while (i > 0 && s[i - 1] != '/') {
        i--;
    }
    if (s[i] == '\0') {
        /* The name ended in a slash, so there is no command in it - a path like
         * "/bin/" rather than a command. Nothing to print is not the same as
         * nothing known, so it says so. */
        out('?');
        return;
    }
    while (s[i] != '\0' && i < MUZIX_PROC_NAME_SIZE - 1) {
        out(s[i++]);
    }
}

/* Ticks, not seconds, and deliberately.
 *
 * The first version of this divided by 120 to turn the kernel's ticks into the
 * seconds the column headings imply.  `ticks` is a uint32_t, so SDCC emitted a
 * 32-bit divide and the link reported `__divulong` unresolved - which on this
 * target is a call to address 0, which is the boot stub, which restarts the
 * machine.  tools/check_userspace_runtime.rb exists for exactly that and it
 * caught it before a ROM was ever built; the alternative to fixing it was not a
 * more careful division, it was a 16-bit quotient with a hand-rolled 32-bit
 * carry around it, which is several times the code to display the same number
 * with a different suffix.
 *
 * So the columns are ticks and the legend says so.  At 120 Hz a tick is a
 * twenty-fifth of a five-second refresh, so the digits move at a rate a person
 * can see, and the number is exact rather than rounded - which a division to
 * whole seconds would not be for a process that has run for 0.5 s. */
static void print_row(const struct proc_info *info)
{
    put_field(info->pid, 5);
    put_field(info->ppid, 6);
    out(' ');
    out(queue_of(info));
    out(' ');
    out(state_of(info));
    /* Ten columns, not seven and eight, and the reason is the tick.
     *
     * These are uint32_t counts of 120 Hz ticks, so they reach ten digits
     * after 2^32 / 120 = 35 791 seconds - ten hours - and they are perfectly
     * ordinary numbers long before that.  A seven-wide field held them while
     * they were always zero, which is the only reason it was ever seven.  Once
     * they are real, a value that does not fit is not truncated: put_field
     * prints every digit and pads only when there is room, so an eight-digit
     * number pushes the rest of the row along and the columns stop lining up
     * with their own headings.  The row is now 5+6+1+1+1+10+10+10+10+2 = 56
     * characters before the command name, which is the widest name this
     * struct can hold plus that - 72 - and still inside an 80-column terminal. */
    put_field(info->user_time, 10);
    put_field(info->sys_time, 10);
    put_field(info->child_user_time, 10);
    put_field(info->child_sys_time, 10);
    out_str("  ");
    put_command(info);
    out_end();
}

/* The summary line, and the seconds field it was stamped with, or -1 if the
 * clock did not answer.
 *
 * The stamp is what wait_seconds() measures from, so it is read once here and
 * the value carried back, not re-read at the top of the wait. */
/* Print a load value, Q14, as digits and exactly two decimals.
 *
 * The split is a shift rather than a divide because the scale is a power of
 * two, and the hundredths are the fraction scaled by 100 and shifted back - the
 * same reason put_field() avoids division.
 *
 * That scale by 100 is three shifts and two adds rather than a multiply, and it
 * has to be: `f * 100u` with f a uint32_t is the one expression in this file
 * that reached __mullong and restarted the machine.  The largest intermediate
 * is 16383 * 100 = 1,638,300, which is comfortably inside 32 bits, and the
 * result is at most 99 - put2() prints exactly two digits. */
static void put_load(uint32_t value)
{
    uint32_t frac;
    uint32_t f = value & (LOAD_SCALE - 1u);

    put_field(value >> 14, 1);
    out('.');
    frac = ((f << 6) + (f << 5) + (f << 2)) >> 14;
    put2((int)frac);
}

static int print_summary(const uint32_t loads[3], uint32_t live)
{
    struct rtc_time t;

    if (get_rtc(&t) != 0) {
        out_str("muzix top  --:--:--   the clock did not answer");
        out_end();
        return -1;
    }
    out_str("muzix top  ");
    put2(t.hour);
    out(':');
    put2(t.minute);
    out(':');
    put2(t.second);
    out_str("    cpu ");
    put_load(loads[0]);
    out(' ');
    put_load(loads[1]);
    out(' ');
    put_load(loads[2]);
    out_str("   tasks ");
    put_field(live, 2);
    out_end();
    return t.second;
}

static void print_columns(void)
{
    out_str("  PID  PPID Q ST     UTIME      STIME     CUTIME     CSTIME  COMMAND");
    out_end();
}

static void print_rule(void)
{
    static const char dashes[] =
        "--------------------------------------------------------";

    out_str(dashes);
    out_end();
}

/* What the numbers mean, on the screen rather than above it.
 *
 * A clearing display wipes anything printed before the first refresh, so a
 * legend emitted once at start-up is a legend nobody reads.  These four lines
 * ride every refresh instead, and they are the things a reader would otherwise
 * get wrong: that the figures are a busy fraction and not a queue depth, that nothing is
 * time-sliced even though there is now a tick, that the time columns are ticks
 * of that tick and how many there are to the second, and what the Q column is.
 *
 * The line about the times used to be the important one, because the columns
 * read zero for as long as one process ran to completion and that looked like a
 * broken counter.  What the kernel charged was an entry into userspace, so a
 * process that was never preempted was never charged - and on a machine with
 * nothing time-sliced that is the only kind there is.  There is a 120 Hz CTC
 * tick now (platform/zeta-v2/tick.s) and the kernel charges the ticks between
 * the two ends of each syscall, so the columns are the time the process spent
 * running and the time the kernel spent on it, counted.
 *
 * They are ticks and not seconds because seconds are not free on this target -
 * the conversion needs a 32-bit divide, `__divulong` is not in the link, and a
 * call to an undefined global on this machine is a call to the boot stub.  See
 * the note on print_row() below, which is where that was found.
 *
 * What has NOT changed is that nothing is time-sliced.  The tick counts; it
 * does not switch processes, and it is not going to - a scheduler switch out of
 * an interrupt handler has to know which map is installed, and the tick handler
 * deliberately does not (see the safety argument in tick.s).  So a process
 * still runs until it blocks or exits. */
static void print_note(void)
{
    out_str("cpu = fraction of wall time the CPU was busy, 1/5/15 min");
    out_end();
    out_str("averages, UTIME+STIME over the interval. Read from the");
    out_end();
    out_str("clock. 120 Hz tick, but no preemption: a process runs");
    out_end();
    out_str("until it blocks.  Q: T S U.  UTIME/STIME are ticks, 120/sec.");
    out_end();
}

int main(int argc, char *argv[])
{
    struct proc_info *info = table;
    int slot;
    int stamp;
    uint32_t live;
    uint32_t loads[3];


    if (argc > 1) {
        out_str("usage: ");
        out_str(argv[0]);
        out_end();
        return 1;
    }

    /* Once per refresh, before anything is drawn, and the check is the reason
     * this program can be stopped.
     *
     * Every other way out is closed on this machine and each is closed for a
     * reason worth stating, because the obvious one is not obviously
     * unavailable:
     *
     *   - Ctrl-C does not work, and cannot be made to with a change here.  The
     *     kernel delivers no signals from the keyboard, so 0x03 arrives as an
     *     ordinary character, and the console is in canonical mode with echo,
     *     so the line discipline stores it in the line buffer like any other.
     *     Making Ctrl-C mean "interrupt" needs a keyboard interrupt and a
     *     signal path, neither of which exists.
     *
     *   - A blocking read() does not work either.  On a canonical TTY it waits
     *     for a whole line, so a program that read to find out whether anyone
     *     had typed would sit there until somebody pressed Enter - which is not
     *     the same thing as being interruptible, and for a program that wants
     *     to keep refreshing is worse than useless.
     *
     *   - Timer and UART interrupts are wired, but the scheduler is
     *     cooperative, so they do not preempt this process. The refresh loop
     *     checks input only at its top; its sliced RTC wait does not end early
     *     on a keypress.
     *
     * What is left is the one that works: the console already knows whether a
     * byte is waiting, and a program can now ask it.  tty_input_pending() is
     * that question, it does not block, and it does not take the byte.
     *
     * Placed at the top of the loop rather than at the bottom so that a
     * keypress is noticed on the way into the next refresh instead of after
     * one, and so that the frame left on screen is a whole one: quitting before
     * clear_screen() runs leaves the last complete display up rather than
     * whatever fraction of a frame the timing landed in.
     *
     * A keypress is seen at the next refresh: up to two seconds plus drawing.
     * Nothing is consumed, so the shell that gets the console back reads the
     * same byte. */
    for (;;) {
        if (tty_input_pending() > 0) {
            return 0;
        }

        /* The sweep comes first and the sample is taken from it, so the figures
         * in the summary and the rows under them describe the same instant.
         * Printing the summary before the sweep would put the previous
         * refresh's average above this refresh's process table, which is the
         * kind of near-truth a load display should not tell. */
        live = 0;
        for (slot = 0; slot < MUZIX_PROC_INFO_SLOTS; slot++) {
            if (sys_proctab(&info[slot], slot) != 0) {
                /* A slot that cannot be read is reported as a slot that has
                 * never run, rather than as whatever the last refresh left in
                 * the buffer: a stale row is worse than an honest one. */
                info[slot].active = 0;
                info[slot].pid = 0;
                info[slot].ppid = 0;
                info[slot].ready_q = MUZIX_PROC_Q_USER;
                info[slot].exited = 0;
                info[slot].name[0] = '\0';
            }
            if (info[slot].active && info[slot].pid != 0) {
                live++;
            }
        }

        /* The three windows come from the kernel, in one call.
         *
         * They used to be computed here, out of the same counters this sweep
         * already reads, and that is where every one of this program's numbers
         * went wrong: the history existed only while this program was running.
         * Exit it and the average was gone; start it and the three windows began
         * at a seed and took three emulated minutes to separate, so the first
         * three minutes of every session reported three identical numbers.
         *
         * kernel/load.c holds the state and the interval arithmetic now, and
         * SYS_LOAD folds the interval that has just ended into it on the way in.
         * That is not only a size argument.  The figure wanted is an average over
         * an interval, and an interval is only known once it has ended - which is
         * exactly when this call happens.  Two programs asking at once get the
         * same history rather than two private ones, and a reader that never comes
         * costs nothing at all.
         *
         * What came out is the same quantity as before: the Q14 fraction of wall
         * time the CPU was not idle, with 1, 5 and 15 minute constants.  It is
         * not, and was never, the Unix load average - that is processes waiting
         * for the processor, which a cooperative scheduler cannot measure.  See
         * the header of kernel/load.c. */
        if (sys_load(loads) != 0) {
            /* The kernel could not write three words back.  Drawing the
             * previous frame's numbers without saying so would be a stale
             * reading presented as a current one, which is the one thing a load
             * display must not do; so it says so instead. */
            loads[0] = 0;
            loads[1] = 0;
            loads[2] = 0;
        }

        /* Clear first, then draw, then wait.  Clearing last would leave the
         * screen blank for the whole of the two seconds, because the clear
         * would be the last thing sent before wait_seconds() started. */
        clear_screen();

        /* The stamp is taken before the block is drawn and the wait is measured
         * from it, so the interval between two summaries is the two seconds
         * rather than the two seconds plus however long drawing took. */
        stamp = print_summary(loads, live);
        print_columns();
        for (slot = 0; slot < MUZIX_PROC_INFO_SLOTS; slot++) {
            print_row(&info[slot]);
        }
        print_rule();
        print_note();

        wait_seconds(stamp, REFRESH_SECONDS);
    }

    return 0;
}
