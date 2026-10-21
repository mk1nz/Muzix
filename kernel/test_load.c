/*
 * test_load - the kernel's busy-fraction windows
 *
 * Drives muzix_load_sample() directly with a clock it controls and the halt
 * brackets it invents, and checks the arithmetic against values worked out by
 * hand.  Both inputs are parameters rather than globals on purpose: the whole
 * reason this can be tested is that it is written so.
 *
 * That is not a style preference.  platform/zeta-v2/test_tick.c tests
 * muzix_host_tick, a host uint16_t standing in for the tick counter, and so it
 * had no multi-byte object, no register file and no byte order to get wrong -
 * which is how three separate real defects in the real routine passed through
 * it.  A test that cannot fail is not a test, and this one is checked both ways:
 * every case below has been run against a deliberately broken version of the
 * arithmetic and observed to fail.
 *
 * WHAT MOVED HERE, AND WHY THE FIGURE IS NOW DRIVEN BY HALTS
 * ---------------------------------------------------------
 * The windows used to be the sum of every process's user and system time over the
 * interval, so the test filled a table with a charged total and let the module
 * read it back.  That is not what the figure is now: it is the fraction of wall
 * time the PROCESSOR was executing, which is the interval minus the part it spent
 * in HALT.  So the driver here is muzix_load_note_halted(), and an interval is
 * described by its LENGTH and how busy it was - the module works out its own tick
 * stamp from the running clock below.
 *
 * The table is still handed to the module and is still not read by it, which is
 * deliberate and is what case 5 asserts.
 *
 * The reason for the change is in muzix_load_note_halted(): the kernel's own work
 * belonged to no process and so fell out of the sum entirely, which is how an idle
 * machine reported 0.02 while being busy about 9% of the time.
 */

#include <stdint.h>
#include <stdio.h>

#include "proc_table.h"
#include "load.h"

#define CHECK(cond, which)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("test_load: case %d failed (%s:%d)\n", (which),          \
                   #cond, __LINE__);                                        \
            return (which);                                                 \
        }                                                                   \
    } while (0)

/* The test's clock.  Every muzix_load_init() below sets it to the same instant
 * the module is given, so an interval is written as a LENGTH and the two never
 * drift apart - which is the mistake case 5's first version made, and the reason
 * the two clocks live next to each other. */
static uint16_t g_tick;
/* A valid table.  The figure does not come out of it any more - see the header -
 * but the module still refuses a NULL one, so the test has to hand it something. */
static void table_ready(muzix_kernel_proc_table_t *t)
{
    int i;

    for (i = 0; i < MUZIX_PROC_TABLE_MAX; i++) {
        t->slots[i].user_time = 0;
        t->slots[i].sys_time = 0;
        t->slots[i].active = 0;
    }
    t->slots[1].active = 1;
    t->slots[1].pid = 1;
    t->current = 1;
}

/* The same table with nothing in it - the state after every process has exited. */
static void table_emptied(muzix_kernel_proc_table_t *t)
{
    int i;

    for (i = 0; i < MUZIX_PROC_TABLE_MAX; i++) {
        t->slots[i].user_time = 0;
        t->slots[i].sys_time = 0;
        t->slots[i].active = 0;
    }
    t->current = -1;
}

/* An interval of `span` ticks, in the units the module divides by: counts of CTC
 * channel 3. At 4 MHz, startup calibration measures about 130 counts per tick.
 * The host can multiply, so this test uses the shared default calibration value.
 *
 * 64-bit intermediates are fine here and only here: the no-multiply rule is a
 * constraint on the kernel image, not on a test compiled by gcc. */
static uint32_t span_counts(uint16_t span)
{
    return (uint32_t)span * MUZIX_LOAD_CTC3_COUNTS_PER_TICK;
}

/* ---- the multiply ---------------------------------------------------------
 *
 * WHAT THIS COVERS AND WHY IT IS NOT THE SAME AS NOTHING
 * -----------------------------------------------------
 * AGENTS.md has claimed since it was written that this file "checks the result
 * against the host compiler over 256 x 256 operand pairs".  It did not, and
 * could not: load_mul() was file static in load.c, so no test could call it and
 * the only thing standing behind the claim was the prose.  The routine is now
 * exported as muzix_load_mul() precisely so that the claim is a test.
 *
 * The full 65536 x 65536 domain is 4.3 billion pairs, which is a few minutes of
 * host CPU and a lot of output; what is checked instead is a DENSE set chosen to
 * put every byte of both operands on a boundary that matters to the carry
 * arithmetic, followed by a deterministic random sweep.  Every pair is compared
 * against the host compiler's own 32-bit product, so the routine is never asked
 * to agree with a hand-worked constant - only with the machine that has a real
 * multiply.
 *
 * The boundaries are not arbitrary: 0, 1, 2, 15, 16, 127, 128, 255 and 256 are
 * where a byte turns over, 8191/8192 and 16383/16384/16385 straddle the Q14
 * operand limit the production callers stay under, and 32767/32768, 65471,
 * 65534 and 65535 are where the top byte of the operand - and therefore p3 and
 * the high half of the product - is largest.  The two carry weights that
 * AGENTS.md records as having been got wrong both only appear above Q14, so a
 * test that stayed inside Q14 could not have caught either of them.
 */

/* The operands the grid is built from.  See the comment above: each one is a
 * boundary for some byte of a or b, or for the Q14 range the callers use. */
static const uint16_t g_mul_ops[] = {
    0u, 1u, 2u, 15u, 16u, 127u, 128u, 255u, 256u, 8191u, 8192u, 16383u,
    16384u, 16385u, 32767u, 32768u, 65471u, 65534u, 65535u
};
#define MUL_OP_COUNT (sizeof(g_mul_ops) / sizeof(g_mul_ops[0]))

/* How many random pairs the sweep adds to the grid.  4096 is small on the host
 * and is not the point: it is here to catch an operand pair the grid's
 * boundaries do not produce, and one bad pair in 4096 random ones is enough to
 * fail.  The generator is a fixed 32-bit LCG so the pairs are the same on every
 * run - a test whose inputs change is a test that can pass by luck. */
#define MUL_RANDOM_PAIRS 4096u

/* (a * b) >> 14 as the host compiler computes it: a 32-bit product shifted
 * down, which is the definition this module has to match.  32-bit arithmetic is
 * fine here - the no-multiply rule constrains the kernel image, and this file
 * is compiled by gcc with all of libc. */
static uint32_t mul_reference(uint16_t a, uint16_t b)
{
    return ((uint32_t)a * (uint32_t)b) >> 14;
}

/* One pair, checked.  Returns 1 when it agrees and 0 when it does not, and
 * counts the pairs whose true value does not fit the 16-bit return.
 *
 * The return of muzix_load_mul() is a uint16_t, and (a*b) >> 14 reaches 262136
 * for a = b = 65535 - it does not fit.  Above Q14 the documented behaviour is
 * the low 16 bits of the true value, so that is what is compared against, and
 * the count of such pairs is reported rather than hidden: a routine that were
 * only ever checked in its top 16 bits would look correct for a reason that
 * stops being true the moment a caller needs the full product. */
static int mul_pair_ok(uint16_t a, uint16_t b, unsigned long *too_wide)
{
    uint32_t want = mul_reference(a, b);
    uint32_t got = muzix_load_mul(a, b);

    if (want > 0xFFFFu) {
        (*too_wide)++;
        want &= 0xFFFFu;
    }
    if (got != want) {
        printf("test_load: mul a=%u b=%u got=%lu want=%lu\n", a, b,
               (unsigned long)got, (unsigned long)want);
        return 0;
    }
    return 1;
}

/* The multiply, against the host compiler.  Returns 0, or the case number. */
static int check_load_mul(void)
{
    unsigned long too_wide = 0;
    unsigned long pairs = 0;
    uint32_t state = 0x13572468u;
    uint32_t k;

    /* 1. The dense grid: every ordered pair of the boundary operands. */
    for (k = 0; k < MUL_OP_COUNT; k++) {
        uint32_t j;

        for (j = 0; j < MUL_OP_COUNT; j++) {
            if (!mul_pair_ok(g_mul_ops[k], g_mul_ops[j], &too_wide)) {
                return 8;
            }
            pairs++;
        }
    }

    /* 2. The random sweep, same LCG on every run. */
    for (k = 0; k < MUL_RANDOM_PAIRS; k++) {
        uint16_t a;
        uint16_t b;

        state = state * 1664525u + 1013904223u;
        a = (uint16_t)(state >> 16);
        state = state * 1664525u + 1013904223u;
        b = (uint16_t)(state >> 16);
        if (!mul_pair_ok(a, b, &too_wide)) {
            return 8;
        }
        pairs++;
    }

    /* 3. The grid must actually contain pairs above Q14, or claim 1 proves
     *    nothing about the carry weights that only exist there. */
    if (too_wide == 0) {
        printf("test_load: no operand pair exceeded 16 bits\n");
        return 8;
    }

    /* 4. The production range is exact, not merely truncated: a Q14 operand
     *    times a Q14 operand shifted back down is always inside 16 bits. */
    CHECK(muzix_load_mul(16384u, 16384u) == LOAD_Q14_SCALE, 8);
    CHECK(muzix_load_mul(16384u, 1u) == 1u, 8);
    CHECK(mul_reference(16384u, 16384u) <= 0xFFFFu, 8);
    CHECK(mul_reference(16384u, 16385u) <= 0xFFFFu, 8);

    printf("test_load: mul %lu pairs, %lu above 16 bits, ok\n", pairs,
           too_wide);
    return 0;
}
/* The busy counts for an interval of `span` at `busy_q14` - the part of it the
 * scheduler was awake for, in the units muzix_load_note_halted() is given.  32 bits
 * because an interval of this size does not fit the argument the module takes;
 * interval() below is what breaks it into pieces that do. */
static uint32_t busy_counts_for(uint16_t span, uint16_t busy_q14)
{
    unsigned long long cap = (unsigned long long)span_counts(span);

    return (uint32_t)((cap * (unsigned long long)busy_q14) >> 14);
}

/* One interval of `span` ticks at `busy_q14`: report its awake counts, sample.
 *
 * Reported in chunks, and that is not tidiness.  muzix_load_note_halted() takes ONE
 * gap, which on the machine is a few thousand cycles and fits a uint16_t easily.
 * A whole minute at full load is 3600 ticks * 130 counts = 468 000 counts, which
 * does not, and passing it in one call silently truncates to 12032 - a figure of
 * 0.014 for an interval that was fully busy.  The first version of this test did
 * exactly that and case 5 caught it.
 *
 * So the interval is handed over in pieces no larger than one tick's worth, which
 * is also the largest the real caller ever produces.  The SAMPLE is still taken
 * once, at the end, so the window arithmetic the cases below assert on is the
 * same one-sample-per-interval shape they were written for. */
static void interval(muzix_kernel_proc_table_t *t, uint16_t span,
                     uint16_t busy_q14)
{
    uint32_t remaining = span_counts(span) - busy_counts_for(span, busy_q14);

    while (remaining > 0u) {
        uint16_t chunk = (remaining > MUZIX_LOAD_CTC3_COUNTS_PER_TICK)
                             ? MUZIX_LOAD_CTC3_COUNTS_PER_TICK
                             : (uint16_t)remaining;

        muzix_load_note_halted(chunk);
        remaining -= (uint32_t)chunk;
    }
    g_tick = (uint16_t)(g_tick + span);
    muzix_load_sample(t, g_tick);
}

/* Start the module and the test clock at the same instant. */
static void restart(muzix_kernel_proc_table_t *t, int emptied)
{
    muzix_load_init(0, MUZIX_LOAD_CTC3_COUNTS_PER_TICK);
    g_tick = 0;
    if (emptied) {
        table_emptied(t);
    } else {
        table_ready(t);
    }
}

int main(void)
{
    muzix_kernel_proc_table_t table;
    uint32_t before0;
    uint32_t before2;
    uint16_t busy = LOAD_Q14_SCALE;        /* 100% */
    int i;

    /* 0. The multiply, first, because every figure below is made of it.  A
     *    module whose Q14 multiply is wrong produces windows that are wrong in
     *    a way no window-shaped assertion can diagnose. */
    {
        int r = check_load_mul();

        if (r != 0) {
            return r;
        }
    }

    /* 1. A first sample is seeded from boot, not from zero.
     *
     * That is the behaviour apps/top.c had to fake with uptime() and the boot
     * clock, and the reason the figures took three emulated minutes to become
     * true: muzix_load_init() records the boot tick, so the first read has the
     * one interval that actually exists.  Since boot nothing has been halted yet -
     * the machine has not reached its first idle loop - so the first reading is
     * 1.00, and that is arithmetic rather than a seeding artefact. */
    restart(&table, 0);
    interval(&table, 120, busy);
    CHECK(muzix_load_window(1) == LOAD_Q14_SCALE, 1);

    /* 2. All three windows answer, and they are equal when the load never
     *    changed - which is the cheapest check that the three-time-constant
     *    table is indexed the way the window accessor thinks. */
    for (i = 0; i < 3; i++) {
        CHECK(muzix_load_window(i) == LOAD_Q14_SCALE, 2);
    }
    /* Out of range is 0, not whatever happens to be next in the array. */
    CHECK(muzix_load_window(-1) == 0, 2);
    CHECK(muzix_load_window(3) == 0, 2);

    /* 3. The SHORT window follows a change and the long one barely moves.
     *
     * Two MINUTES of zero load, which is two constants of the short window and
     * two tenths of one of the long one.  Analytically, from 1.00:
     *
     *   window 0   exp(-120/60)  = 0.135  ->  2217 of 16384
     *   window 1   exp(-120/300) = 0.670  -> 10982
     *   window 2   exp(-120/900) = 0.875  -> 14339
     *
     * So the short window must end up far BELOW the long one.  It was the other
     * way round for a while, and the cause was not the constants: `acc +=
     * (busy - acc) * alpha` is acc*(1-alpha) + busy*alpha, which puts the
     * retention on the NEW sample and collapses the long window instead of the
     * short one.  The ordering is the whole point of this case, so it is checked
     * as an inequality rather than as a number.
     *
     * Two minutes, not one: the separation is exp(-2) against exp(-2/15), which
     * is a factor of six and half.  Over a single second the three windows sit
     * within 2% of each other and a test could not tell a correct module from an
     * inverted one.
     *
     * Zero load is the WHOLE interval reported as stopped, which is what a parked
     * machine looks like.  The version before that zeroed the process counters
     * instead, which was a different statement: it said a process charged nothing,
     * not that the processor ran nothing, and that distinction is the whole
     * change. */
    interval(&table, 0, busy);                 /* span 0: a no-op */
    before0 = muzix_load_window(0);
    before2 = muzix_load_window(2);
    interval(&table, (uint16_t)(120 * 120), 0); /* 120 s with no awake time */
    CHECK(muzix_load_window(0) < before0, 3);         /* short fell */
    CHECK(muzix_load_window(2) < before2, 3);         /* long fell too */
    CHECK(muzix_load_window(0) < muzix_load_window(2), 3); /* and less than long */
    CHECK(muzix_load_window(0) > 2000u && muzix_load_window(0) < 2450u, 3);
    CHECK(muzix_load_window(2) > 14000u && muzix_load_window(2) < 14700u, 3);

    /* 4. A zero-length interval does nothing at all.
     *
     * Not "does something small": dt == 0 is what two reads in the same tick
     * produce, and a window that moved on them would report a rate the caller
     * never measured. */
    {
        uint32_t before = muzix_load_window(0);

        muzix_load_sample(&table, g_tick);
        CHECK(muzix_load_window(0) == before, 4);
    }

    /* 5. Nothing outside the kernel can move the figure.
     *
     * This replaced the old "a total that FALLS does not count as idle" case,
     * which guarded a real underflow: the sum of process counters is cumulative
     * and a process EXIT takes its counters out of the table with it, so the
     * unsigned difference read as an enormous number.  That hazard is gone rather
     * than fixed - there is no longer a total for a process to take away, because
     * the halted accumulator belongs to the scheduler and the scheduler is the
     * only writer.
     *
     * So the property is stronger than the one it replaces and is asserted
     * directly: the same clock, the same brackets and the same window history,
     * with an emptied table and with a populated one, must produce the same
     * figure.  That is the whole claim - the table is an argument the module
     * accepts and does not use - and it is the case that fails if anyone wires
     * the counters back in. */
    {
        uint32_t with_empty_table;
        uint32_t with_full_table;

        restart(&table, 1);
        interval(&table, 120, 0);                 /* identical history, idle */
        interval(&table, (uint16_t)(60 * 120), busy);   /* then a minute at full */
        with_empty_table = muzix_load_window(0);

        restart(&table, 0);
        interval(&table, 120, 0);                 /* identical history, idle */
        interval(&table, (uint16_t)(60 * 120), busy);   /* and the same interval */
        with_full_table = muzix_load_window(0);

        CHECK(with_empty_table == with_full_table, 5);
        /* And the pair is not trivially equal because nothing happened: one idle
         * minute then one busy minute must leave the short window well above zero
         * and below the maximum. */
        CHECK(with_full_table > 2000u && with_full_table < LOAD_Q14_SCALE, 5);
    }

    /* 5b. The three constants still behave differently over one step.
     *
     * Kept separate from the equality above because that one needs identical
     * inputs, and this one needs the opposite: from a known start, one minute back
     * at full load, the short window must move far and the long one barely.  From
     * 0 and exp(-1):
     *
     *   window 0   16384 * (1 - exp(-1))  = 11008,  a move of 11008
     *   window 1   16384 * (1 - exp(-1/5))=  3116
     *   window 2   16384 * (1 - exp(-1/15))=  1050
     *
     * What distinguishes the constants is how far each MOVED, so that is what is
     * compared - not the levels, which are all near their own targets after one
     * step and say nothing. */
    {
        uint32_t w0, w1, w2;

        restart(&table, 0);
        interval(&table, 120, 0);                       /* idle, from zero */
        CHECK(muzix_load_window(0) == 0, 5);
        interval(&table, (uint16_t)(60 * 120), busy);   /* one minute at full */
        w0 = muzix_load_window(0);
        w1 = muzix_load_window(1);
        w2 = muzix_load_window(2);
        CHECK(w0 > 10000u && w0 < 12000u, 5);
        CHECK(w1 > 2600u && w1 < 3600u, 5);
        CHECK(w2 > 800u && w2 < 1400u, 5);
        CHECK(w0 > w1 && w1 > w2, 5);
    }

    /* 6. Convergence: a constant load drives every window to that load.
     *
     * Five minutes of half load, sampled every second, must bring all three
     * close to 8192 - and the short window must be closer to it than the long
     * one is not yet.  This is the end-to-end statement that the recurrence is
     * an average and not a copy: with a per-TICK decay factor the 15-minute
     * window would be stuck at 16384 forever, which is what exp(-1/108000)
     * rounds to in Q14, and this case cannot pass if that mistake is made. */
    {
        uint16_t half = LOAD_Q14_SCALE / 2;
        int step;

        /* The system starts IDLE, then the load appears and stays.  A load that
         * is already at the target when the first sample lands proves nothing:
         * the first sample ASSIGNS the windows, so they would all read 8192 and
         * stay there whether or not the recurrence works. */
        restart(&table, 0);
        interval(&table, 120, 0);                        /* one idle second */
        CHECK(muzix_load_window(0) == 0, 6);

        for (step = 1; step <= 300; step++) {
            interval(&table, 120, half);                 /* half of it executing */
        }
        /* 300 s is five short constants, one long one and a third of a
         * fifteen-minute one, so the three windows must be widely spread rather
         * than merely near the target:
         *
         *   window 0   8192 * (1 - exp(-5))    = 8136
         *   window 1   8192 * (1 - exp(-1))    = 5178
         *   window 2   8192 * (1 - exp(-1/3))  = 2322
         *
         * Generous bounds, because the point is the ordering and the
         * neighbourhood, not a decimal. */
        CHECK(muzix_load_window(0) > 7800u && muzix_load_window(0) < 8600u, 6);
        CHECK(muzix_load_window(1) > 4400u && muzix_load_window(1) < 5900u, 6);
        CHECK(muzix_load_window(2) > 1600u && muzix_load_window(2) < 3000u, 6);
        CHECK(muzix_load_window(0) > muzix_load_window(1), 6);
        CHECK(muzix_load_window(1) > muzix_load_window(2), 6);
    }

    /* 7. The fallback scale is calibrated for the installed 4 MHz oscillator.
     *
     * Half of one tick's 130-count capacity halted means half busy. With the
     * former 240-count assumption this would incorrectly report about 73% busy. */
    restart(&table, 0);
    muzix_load_note_halted(65u);
    g_tick = 1;
    muzix_load_sample(&table, g_tick);
    CHECK(muzix_load_window(0) > 8170u &&
          muzix_load_window(0) < 8210u, 7);

    /* The measured scale is passed into load accounting, not just reported. */
    muzix_load_init(0, 240u);
    muzix_load_note_halted(120u);
    g_tick = 1;
    muzix_load_sample(&table, g_tick);
    CHECK(muzix_load_window(0) == LOAD_Q14_SCALE / 2u, 9);

    printf("test_load: ok\n");
    return 0;
}