/*
 * test_tick.c - the CTC tick, checked against the numbers rather than the
 * comment.
 *
 * The arithmetic in platform/zeta-v2/tick.s says 120 Hz, and this test re-derives
 * 120 from the assembler's own `.equ` values rather than believing the prose
 * next to them.  A comment that says 120 and a time constant that produces 121
 * is not a comment anybody reads again; this is the build failing instead, which
 * is the only version of that check that survives a Tuesday.
 *
 * Three things are checked: the divisor arithmetic and the control byte, the
 * deadline comparison including the 16-bit wrap, and the process accounting
 * including the carry into the high half of a 32-bit counter.
 *
 * Every case here runs on the host, and deliberately so: the whole of the tick
 * is a port write, an arithmetic identity and two comparisons, and none of the
 * three needs a Z80 to be wrong.  What this file deliberately does NOT fake is
 * the part that does need the board or the emulator - that the interrupt is
 * taken at all, that the CTC really divides by 256, and the console.  Those are
 * verified by running build/muzix-zeta-v2.rom and reading the counters back
 * through top, and emulator/z80pack/z80pack.c is where the emulator's half of
 * the contract lives.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tick.h"

/* The test's own tick control, through the same host seam every other host test
 * uses (test_host/z80_io_host.c).  The stub's arithmetic is a C restatement of
 * the assembly in tick.s, and the two are meant to be the same function - a
 * bounded wait and a process clock are both just "subtract, then add", and if
 * the C and the Z80 ever disagree then one of them is wrong and nothing in this
 * tree would have said so. */
extern uint16_t muzix_host_tick;
extern void muzix_host_tick_set(uint16_t ticks);
extern void muzix_host_tick_advance(uint16_t ticks);

#define Z80PACK_CPU_HZ 7372800u
/* The board's tick chain, from the Zeta SBC V2 README: channel 0's CLK/TG input
 * is UART_CLK/2, channel 1's is channel 0's ZC/TO. This input is independent of
 * the board's 4 MHz U17 oscillator. In the emulator profile, UART_CLK/2 is
 * Z80PACK_CPU_HZ divided by 8. */
#define ZETA_UART_CLK_2 921600u

static int failures;

static void check(int ok, int id, const char *what)
{
    if (!ok) {
        printf("FAIL %d: %s\n", id, what);
        failures++;
    }
}

/* ---- the control word, read out of the assembler's source ----------------
 *
 * The byte is the interface with hardware the host does not have, and it has to
 * satisfy two independent readings of the CTC at once:
 *
 *   - the real Zilog part, where bit 2 means "the next byte is a time constant
 *     and reset the counter" and bit 1 means "stay enabled afterwards"; and
 *   - the emulator's model in emulator/z80pack/z80pack.c, which reads
 *     `control & 0x04` as the flag that arms the time constant and
 *     `control & 0x20` to choose a /256 rather than /16 timer prescaler, and
 *     `control & 0x40` to distinguish counter mode from timer mode.
 *
 * A byte that satisfies the first and not the second would work on the board and
 * produce no tick in the emulator, which is the sort of difference that gets
 * discovered by "it was working yesterday".  So the bits are checked by name
 * rather than the whole byte compared against a literal - the literal would pass
 * for the wrong reason if somebody reordered the fields. */
/* Read a `.equ` value by name.
 *
 * The name has to be matched as a WHOLE identifier, not as a prefix.  It used to
 * be a bare strstr(), which means MUZIX_CTC_TC matched the first occurrence of
 * that text in the file - and after the board's two-channel tick was documented,
 * that is MUZIX_CTC_TC_PRESCALE, so the tick's own time constant silently read
 * as the prescaler's.  The two are a factor of 8.5 apart and the resulting
 * failure looked like arithmetic rather than like a lookup bug, which is the
 * worst kind.
 *
 * So: the name, then whitespace, then the directive.
 */
static unsigned long read_equ(const char *text, const char *name, int *found)
{
    size_t len = strlen(name);
    const char *p = text;

    *found = 0;
    while ((p = strstr(p, name)) != NULL) {
        const char *q = p + len;

        if (*q == ' ' || *q == '\t') {
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            if (strncmp(q, ".equ", 4) == 0) {
                q += 4;
                *found = 1;
                while (*q == ' ' || *q == '\t') {
                    q++;
                }
                if (*q == '#' || *q == '$') {
                    q++;
                }
                return strtoul(q, NULL, 0);
            }
        }
        p += len;
    }
    return 0;
}


static char *slurp(const char *path)
{
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static void test_divisor_arithmetic(void)
{
    const char *path = "platform/zeta-v2/tick.s";
    char *text = slurp(path);
    int have_tc_tick = 0;
    int have_ctrl_tick = 0;
    int have_tc_ch0 = 0;
    int have_ctrl_ch0 = 0;
    int have_uart_ctrl = 0;
    int have_uart_tc = 0;
    int have_cycle_tc = 0;
    int have_cycle_ctrl = 0;
    unsigned long tc_tick;
    unsigned long ctrl_tick;
    unsigned long tc_ch0;
    unsigned long ctrl_ch0;
    unsigned long uart_ctrl;
    unsigned long uart_tc;
    unsigned long cycle_tc;
    unsigned long cycle_ctrl;
    unsigned long ctc_rate;
    unsigned long emulator_cycles_per_tick;
    unsigned long cycle_period;
    const char *init;
    const char *tick_vector;
    const char *im2;
    const char *uart_irq;
    const char *tick_irq;
    const char *tick_isr;
    const char *uart_isr;
    const char *tick_ei;
    const char *uart_ei;
    const char *tick_reti;
    const char *uart_reti;

    if (!text) {
        check(0, 1, "platform/zeta-v2/tick.s is readable from the tree root");
        return;
    }
    check(1, 1, "platform/zeta-v2/tick.s is readable from the tree root");

    tc_tick = read_equ(text, "MUZIX_CTC_TC_TICK", &have_tc_tick);
    ctrl_tick = read_equ(text, "MUZIX_CTC_CTRL_TICK_IRQ", &have_ctrl_tick);
    tc_ch0 = read_equ(text, "MUZIX_CTC_TC_CH0", &have_tc_ch0);
    ctrl_ch0 = read_equ(text, "MUZIX_CTC_CTRL_CH0", &have_ctrl_ch0);
    uart_ctrl = read_equ(text, "MUZIX_CTC_CTRL_UART_IRQ", &have_uart_ctrl);
    uart_tc = read_equ(text, "MUZIX_CTC_UART_TC", &have_uart_tc);
    cycle_tc = read_equ(text, "MUZIX_CTC_CYCLE_TC", &have_cycle_tc);
    cycle_ctrl = read_equ(text, "MUZIX_CTC_CTRL_CYCLE", &have_cycle_ctrl);

    check(have_tc_tick, 2, "tick.s declares MUZIX_CTC_TC_TICK");
    check(have_ctrl_tick, 3, "tick.s declares MUZIX_CTC_CTRL_TICK_IRQ");
    check(have_tc_ch0, 17,
          "tick.s declares MUZIX_CTC_TC_CH0 - without a channel 0 there is "
          "nothing clocking channel 1, and the tick never interrupts");
    check(have_ctrl_ch0, 18,
          "tick.s declares MUZIX_CTC_CTRL_CH0");
    check(have_cycle_tc && have_cycle_ctrl, 19,
          "tick.s declares the channel 3 cycle-counter control and time constant");
    check(have_uart_ctrl && have_uart_tc, 35,
          "tick.s declares the channel 2 UART interrupt control and time constant");
    init = strstr(text, "_muzix_tick_init:");
    tick_vector = init ? strstr(init, "ld\t(MUZIX_IM2_TICK),hl") : NULL;
    im2 = init ? strstr(init, "\tim\t2") : NULL;
    uart_irq = init ? strstr(init, "ld\ta,#MUZIX_CTC_CTRL_UART_IRQ") : NULL;
    tick_irq = init ? strstr(init, "ld\ta,#MUZIX_CTC_CTRL_TICK_IRQ") : NULL;
    check(tick_vector && im2 && uart_irq && tick_irq &&
              tick_vector < im2 && im2 < uart_irq && uart_irq < tick_irq,
          43, "IM2 table is installed before UART and tick IRQ sources are armed");
    tick_isr = strstr(text, "\n_muzix_tick_isr:");
    uart_isr = strstr(text, "\n_muzix_uart_rx_isr:");
    tick_ei = tick_isr ? strstr(tick_isr, "\tei") : NULL;
    uart_ei = uart_isr ? strstr(uart_isr, "\tei") : NULL;
    tick_reti = tick_isr ? strstr(tick_isr, "\treti") : NULL;
    uart_reti = uart_isr ? strstr(uart_isr, "\treti") : NULL;
    check(tick_isr && tick_ei && tick_reti && tick_ei < tick_reti,
          44, "tick ISR reenables maskable interrupts before RETI");
    check(uart_isr && uart_ei && uart_reti && uart_ei < uart_reti,
          45, "UART ISR reenables maskable interrupts before RETI");
    if (!have_tc_tick || !have_ctrl_tick || !have_tc_ch0 || !have_ctrl_ch0 ||
        !have_cycle_tc || !have_cycle_ctrl || !have_uart_ctrl || !have_uart_tc) {
        return;
    }
    /* A written 0 is 256 to the CTC. Both channel constants use that encoding. */
    if (tc_ch0 == 0u) {
        tc_ch0 = 256u;
    }
    if (tc_tick == 0u) {
        tc_tick = 256u;
    }

    /* THE RATE IS THE BOARD'S CTC CHAIN, NOT A CPU DIVIDE.
     *
     * The Zeta SBC V2 README, "Interrupts", says:
     *
     *   "Channel's 0 CLK/TG input is connected to UART_CLK/2 (921.6 kHz)"
     *   "Channel's 1 CLK/TG input is connected to channel's 0 ZC/TO output"
     *
     * So the tick is
     *
     *   921 600 Hz -> ch0 / TC_CH0 -> ch1 / TC_TICK -> tick rate
     *
     * The CTC rate is independent of the CPU frequency. The cycle interval
     * below is only the corresponding value for this emulator profile, where
     * UART_CLK/2 happens to be CPU/8.
     */
    ctc_rate = ZETA_UART_CLK_2 / tc_ch0 / tc_tick;
    check(ctc_rate == MUZIX_PROC_TICK_HZ, 4,
          "UART_CLK/2 divided by both channel constants gives 120 Hz");
    check(ZETA_UART_CLK_2 % (tc_ch0 * tc_tick) == 0u, 13,
          "the board's chained CTC divisor gives an exact 120 Hz rate");
    emulator_cycles_per_tick =
        (Z80PACK_CPU_HZ / ZETA_UART_CLK_2) * tc_ch0 * tc_tick;
    check(emulator_cycles_per_tick * MUZIX_PROC_TICK_HZ == Z80PACK_CPU_HZ, 41,
          "the emulator profile maps each 120 Hz period to exact CPU cycles");
    check(Z80PACK_CPU_HZ % ZETA_UART_CLK_2 == 0u, 42,
          "UART_CLK/2 is CPU/8 in this emulator profile");
    check(tc_tick >= 1 && tc_tick <= 256, 5,
          "the time constant fits a CTC time-constant byte (0 means 256)");
    check(tc_ch0 >= 1 && tc_ch0 <= 256, 14,
          "channel 0's time constant fits a CTC time-constant byte");
    /* D6 is "timer/counter" and 1 selects COUNTER, which clocks the channel from
     * the system clock.  Both tick channels must be COUNTERS, because their clocks
     * come from the board: channel 0 from UART_CLK/2, channel 1 from channel 0's
     * ZC/TO.  A timer mode here would be clocked from the CPU and would ignore
     * the wiring entirely.
     *
     * This assertion has been inverted twice in this file's history and was wrong
     * both times, once in the comment and once in the code.  The board's own
     * documentation settles it: it specifies `01000111` for channel 0 and
     * `11000111` for channel 1, and both have bit 6 set.
     */
    check((ctrl_tick & 0x40) != 0, 6,
          "control bit 6 set: COUNTER mode, clocked from the board's wiring");
    check((ctrl_ch0 & 0x40) != 0, 15,
          "channel 0 control bit 6 set: COUNTER mode, clocked from UART_CLK/2");
    check((ctrl_ch0 & 0x80) == 0, 16,
          "channel 0 control bit 7 clear: it must not itself "
          "interrupt - channel 1 raises the interrupt");
    check((ctrl_tick & 0x80) != 0, 7, "control bit 7 set: interrupts enabled");
    /* D2 set: the next byte is the time constant. */
    check((ctrl_tick & 0x04) != 0, 8,
          "control bit 2 set: the next byte is a time constant");
    check((ctrl_tick & 0x02) != 0, 9,
          "control bit 1 set: the channel stays enabled after loading");
    check((ctrl_tick & 0x01) != 0, 10,
          "control bit 0 set: this is a control word");

    /* Channel 3 measures time in the kernel's halt loop. D6 clear selects
     * TIMER mode; D5 set selects the hardware's /256 prescaler. */
    check((cycle_ctrl & 0x40) == 0, 32,
          "channel 3 control bit 6 clear: TIMER mode uses the system clock");
    check((cycle_ctrl & 0x20) != 0, 33,
          "channel 3 control bit 5 set: hardware /256 prescaler");
    cycle_period = ((cycle_ctrl & 0x20) != 0 ? 256u : 16u) *
                   (cycle_tc == 0u ? 256u : cycle_tc);
    check(cycle_period == 65536u, 34,
          "channel 3 counter wraps after 65536 CPU cycles, not 4096");

    /* The Zeta V2 UART interrupt output asserts high. Channel 2 must count its
     * rising edge, as FUZIX does with 0xD7, or a parked reader sleeps until its
     * two-second syscall deadline instead of waking for input. */
    check((uart_ctrl & 0x10) != 0, 36,
          "channel 2 control bit 4 set: UART interrupt's rising edge");
    check((uart_ctrl & 0x40) != 0, 37,
          "channel 2 control bit 6 set: COUNTER mode follows the UART pin");
    check((uart_ctrl & 0x80) != 0 && uart_tc == 1u, 38,
          "channel 2 interrupt enabled with time constant 1");
    check(uart_ctrl == MUZIX_CTC_UART_IRQ_CONTROL, 39,
          "the C configuration and assembly control word agree");
    {
        char *kernel_text = slurp("kernel/kernel_main.c");
        check(kernel_text != NULL &&
              strstr(kernel_text,
                     "z80_outb(0x22u, MUZIX_CTC_UART_IRQ_CONTROL)") != NULL,
              40, "the boot probe restores channel 2 with its configured edge");
    }

    /* And the rate is the one everything else in the tree assumes.  120 is what
     * makes "a value of 120 is one second" in lib/proc_info.h true, and a
     * millisecond not being a whole number of ticks is why every budget in the
     * tree is in seconds or in whole ticks. */
    check(MUZIX_PROC_TICK_HZ == 120, 11, "the published rate is 120 Hz");
    check(emulator_cycles_per_tick != 0u &&
          Z80PACK_CPU_HZ % emulator_cycles_per_tick == 0u, 12,
          "the emulator's modeled tick period has no fractional CPU cycle");
}

/* ---- the deadline comparison ---------------------------------------------
 *
 * muzix_tick_expired() is `now - deadline >= 0x8000` as a 16-bit unsigned
 * subtraction, which is the only form that survives the counter wrapping: the
 * tick counter is 16 bits at 120 Hz, so it wraps every 546 seconds and a
 * deadline set just before that has to still read as "not yet".
 *
 * The stub is the C restatement of tick.s, so these cases are testing the rule
 * the assembly implements.  The assembly itself is exercised by running the ROM
 * and reading the counters back through top. */
static void test_deadline(void)
{
    uint16_t deadline;

    muzix_host_tick_set(0);
    deadline = MUZIX_TICK_DEADLINE(5);
    check(deadline == 600, 20,
          "a five second deadline is 600 ticks at 120 Hz");
    check(muzix_tick_expired(deadline) == 0, 21,
          "a deadline is not expired the moment it is set");

    muzix_host_tick_advance(599);
    check(muzix_tick_expired(deadline) == 0, 22,
          "one tick short of the deadline has not expired");

    muzix_host_tick_advance(1);
    check(muzix_tick_expired(deadline) == 1, 23,
          "the deadline expires on the tick it is reached");

    muzix_host_tick_advance(1000);
    check(muzix_tick_expired(deadline) == 1, 24,
          "well past the deadline is still expired");

    /* The wrap.  A deadline of 0xFFF0 is nine ticks from the top of the counter,
     * so it must read as "not yet" for those nine ticks and as expired after -
     * a signed comparison would have it backwards across the whole window. */
    muzix_host_tick_set(0xFFE0);
    deadline = (uint16_t)(0xFFE0 + 16);
    check(deadline == 0xFFF0, 25, "the wrap deadline is the value expected");
    check(muzix_tick_expired(deadline) == 0, 26,
          "a deadline just below the counter wrap is not expired");
    muzix_host_tick_set(0xFFF0);
    check(muzix_tick_expired(deadline) == 1, 27,
          "it expires exactly at the value the addition produced");
    muzix_host_tick_set(0x0004);   /* the counter has wrapped past 65535 */
    check(muzix_tick_expired(deadline) == 1, 28,
          "still expired after the 16-bit counter wraps");

    /* A deadline more than half the range ahead is ambiguous by construction -
     * 0x8000 ticks is 273 seconds and the half-range is 32767 ticks.  The one
     * budget anywhere near that is the fallback shell's 2-tick re-arm, and this
     * pins the boundary so a future "just make it 300 seconds" fails here
     * rather than in a five minute wait on the board. */
    muzix_host_tick_set(0);
    check(muzix_tick_expired((uint16_t)(0 + 32767)) == 0, 29,
          "32767 ticks ahead - the largest budget the half-range rule can see - "
          "reads as not yet");
    check(muzix_tick_expired(0) == 1, 30,
          "a deadline equal to now has just been reached and is expired");
    check(muzix_tick_expired((uint16_t)(0 + 0x8000)) == 0, 31,
          "32768 ticks ahead is past the half-range and reads as not yet, "
          "which is why no budget in the tree is that large");
}

/* ---- the process accounting ---------------------------------------------
 *
 * muzix_tick_add_process_time() is four `add`/`adc` and four stores in
 * tick.s, replacing a 247-byte C function.  What can be wrong with it is
 * arithmetic rather than codegen, and arithmetic is checkable here: the
 * 16-bit difference is added to the LOW half of a 32-bit counter and the carry
 * has to reach the high half, or a process's times run to 65535 and stop.
 *
 * The carry case is the one that a test which only adds small numbers would
 * miss entirely, and it is the one that happens in the field. */
static void test_process_accounting(void)
{
    uint32_t user = 0;
    uint32_t system = 0;
    uint16_t stamp;

    muzix_host_tick_set(1000);
    muzix_tick_stamp_store(&stamp);
    check(stamp == 1000, 40, "a new process's accounting starts at now");

    /* No time, no charge: two syscalls inside one tick is the common case and
     * must not drift the stamp, or a process doing nothing but syscalls would
     * accrue a fraction of a tick per syscall forever. */
    muzix_tick_add_process_time(&user, &stamp);
    check(user == 0, 41, "a charge of zero ticks adds nothing");
    check(stamp == 1000, 42, "and does not move the stamp either");

    muzix_host_tick_advance(250);
    muzix_tick_add_process_time(&user, &stamp);
    check(user == 250, 43, "250 ticks of user time are charged");
    check(stamp == 1250, 44, "the stamp follows the charge");

    muzix_host_tick_advance(5);
    muzix_tick_add_process_time(&system, &stamp);
    check(system == 5, 45, "5 ticks of system time are charged to the other "
          "counter");
    check(user == 250, 46, "and not to user time");
    check(stamp == 1255, 47, "one stamp serves both counters");

    /* The carry into the high half.  A slot's counter is uint32_t because
     * struct proc_info says so; the addend is 16 bits, so the high half is
     * reached only by a carry out of the low half - which is 65 535 ticks of
     * runtime, about nine minutes, and is exactly the case a test that adds
     * 250 at a time would never run into. */
    {
        uint32_t big = 0x0000FFFFu;
        uint16_t big_stamp;

        muzix_host_tick_set(0);
        muzix_tick_stamp_store(&big_stamp);
        big_stamp = 0;                   /* pretend a full low half elapsed */
        muzix_host_tick_set(0xFFFF);
        muzix_tick_add_process_time(&big, &big_stamp);
        check(big == 0x0001FFFEu, 48,
              "a 16-bit addend carries into the high half of the 32-bit counter");

        big = 0x00010000u;
        muzix_host_tick_set(0x0000);
        big_stamp = 0x0001;
        muzix_tick_add_process_time(&big, &big_stamp);
        check(big == 0x0001FFFFu, 49,
              "a 65535-tick charge across the counter's own 16-bit wrap is "
              "0x1FFFF, and reading it as 0xFFFF would silently halve it");
    }
}

/* ---- the real assembly, run --------------------------------------------
 *
 * Everything above this tests a C restatement of tick.s, through the host seam
 * in test_host/z80_io_host.c.  That is worth having and it is not sufficient,
 * and the reason is worth writing down because it is not a general truth about
 * testing: the restatement has ONE byte order, no register file and no
 * multi-byte object, so the three faults tick.s actually carried for a month
 * were invisible to it.  Each of the readers took the low byte of the counter
 * first, so the second half of the value came from `(low << 8) | 1` - window
 * zero, which on this machine holds the boot stub - and a charge of three ticks
 * landed as 0x0400 because the difference had its halves the wrong way round.
 * Not one of those is expressible in C, and every one of them passed here.
 *
 * So the assembly is also assembled, linked and executed, by tools/tick_z80.rb,
 * and this test fails if it does not pass.  That is also why the rig is a tool
 * and not a Makefile target: it needs the assembler and the emulator core, it
 * belongs to no module, and it has to run through host_tests because that is
 * the only runner in this tree that actually executes anything.
 *
 * The repository root comes out of argv[0].  tools/host_tests.rb runs the test
 * binary as build/hosttests/a.out, so the root is the part in front of that,
 * and the test does not depend on the working directory it was started in.
 *
 * A missing toolchain is a failure and not a pass.  A test that reports
 * success because the thing it wanted to check did not run is the failure mode
 * this tree's own test history is full of - thirty-odd test_*.c files that were
 * linked into .ihx, reported as BUILT, and never started once. */
static int test_the_real_assembly(const char *argv0)
{
    char root[512];
    char cmd[700];
    const char *tail = "/build/hosttests/a.out";
    size_t cut;
    int rc;

    if (!argv0 || !*argv0) {
        printf("FAIL z80: no argv[0], so the repository root cannot be found "
               "and the real tick.s was not run\n");
        failures++;
        return 0;
    }
    cut = strlen(argv0);
    if (cut <= strlen(tail) || strcmp(argv0 + cut - strlen(tail), tail) != 0) {
        printf("FAIL z80: this test was started as '%s', not as build/hosttests/"
               "/a.out, so the repository root cannot be found and the real "
               "tick.s was not run\n", argv0);
        failures++;
        return 0;
    }
    cut -= strlen(tail);
    if (cut >= sizeof root) {
        printf("FAIL z80: the repository root does not fit in %u bytes\n",
               (unsigned)sizeof root);
        failures++;
        return 0;
    }
    memcpy(root, argv0, cut);
    root[cut] = '\0';

    snprintf(cmd, sizeof cmd, "ruby tools/tick_z80.rb '%s'", root);
    rc = system(cmd);
    if (rc == -1) {
        printf("FAIL z80: could not run '%s'\n", cmd);
        failures++;
        return 0;
    }
    /* system() returns a wait status, and the shell's is 256 * the exit code.
     * Anything else - a signal, a shell error - is not a pass either. */
    if ((rc & 0x7F) != 0 || ((rc >> 8) & 0xFF) != 0) {
        printf("FAIL z80: tools/tick_z80.rb did not pass (status 0x%X)\n", (unsigned)rc);
        failures++;
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    test_divisor_arithmetic();
    test_deadline();
    test_process_accounting();
    test_the_real_assembly(argv[0]);

    if (failures) {
        printf("test_tick: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_tick: all cases pass\n");
    return 0;
}
