/*
 * test_load_freq - CPU-frequency detection, CTC3 conversion, HALT-overhead
 * scaling, and frequency-independence of the loadavg windows.
 *
 * WHY THIS EXISTS
 * ---------------
 * The loadavg figure is built from two clocks: the 120 Hz system tick (which
 * sets the EMA exponent) and the CTC0/CTC1 reference clock at 921600 Hz (which
 * measures the halted fraction).  Neither depends on the CPU clock, so the
 * result must be identical at every supported CPU frequency.  The one place the
 * CPU clock legitimately enters is the HALT measurement overhead: the fixed
 * instruction path in muzix_halt_begin()/the ISRs is measured in T-states, and
 * converting T-states to CTC0/CTC1 counts needs the detected frequency.
 *
 * The emulator runs at one fixed clock, so the frequency matrix cannot be run
 * against the board model.  What can be tested - and is what this file does -
 * is the arithmetic that turns a measured CTC3 count into a frequency, the
 * arithmetic that turns a frequency into a HALT overhead in reference counts,
 * and the fact that the loadavg windows themselves are unchanged by the
 * frequency.  All of it runs against the production functions, not a copy.
 *
 * The host provides muzix_tick_ctc3_counts_per_tick (test_host/zeta_host.c),
 * which is what the calibration would have measured, and tick_helpers.c turns
 * it into a frequency exactly as the kernel does.
 */

#include "ztypes.h"
#include "libc.h"

#include "proc_table.h"
#include "load.h"

/* The production frequency conversion.  tick_helpers.c reads the calibrated
 * CTC3 count from this global. */
extern uint8_t muzix_tick_get_cpu_mhz_x10(void);
extern uint16_t muzix_tick_ctc3_counts_per_tick;

#define CHECK(cond, which)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("test_load_freq: case %d failed (%s:%d)\n", (which),     \
                   #cond, __LINE__);                                        \
            return (which);                                                 \
        }                                                                   \
    } while (0)

/* CTC0/CTC1 reference counts per 120 Hz tick, from load.h.  The halted
 * accumulator and muzix_load_sample()'s capacity both use these. */
#define COUNTS_PER_TICK 7680u

/* The eight supported frequencies, in MHz, and the CTC3 counts per tick each
 * produces.  N = f_Hz / (256 * 120) = f_Hz / 30720, rounded to nearest.
 *
 *   2 MHz  ->  65.104  ->  65
 *   4 MHz  -> 130.208  -> 130
 *   6 MHz  -> 195.313  -> 195
 *   8 MHz  -> 260.417  -> 260
 *  10 MHz  -> 325.521  -> 326
 *  12 MHz  -> 390.625  -> 391
 *  16 MHz  -> 520.833  -> 521
 *  20 MHz  -> 651.042  -> 651
 */
static const uint16_t g_mhz[8] = { 2u, 4u, 6u, 8u, 10u, 12u, 16u, 20u };
static const uint16_t g_counts[8] = { 65u, 130u, 195u, 260u,
                                      326u, 391u, 521u, 651u };

/* ceil(T * 921600 / (mhz_x10 * 100000)) computed with the host's 32-bit
 * multiply - the reference the production shift/subtract routine must match. */
static uint16_t ref_overhead(uint16_t t, uint16_t mhz_x10)
{
    uint32_t num = (uint32_t)t * 921600u;
    uint32_t den = (uint32_t)mhz_x10 * 100000u;

    return (uint16_t)((num + den - 1u) / den);
}

/* The production overhead pair, as the kernel computes it in muzix_load_init:
 * START_OVERHEAD_T + WAKE_OVERHEAD_T.  Kept in step with load.c's constants. */
#define START_OVERHEAD_T 118u
#define WAKE_OVERHEAD_T  44u

static uint16_t ref_total_overhead(uint16_t mhz_x10)
{
    return (uint16_t)(ref_overhead(START_OVERHEAD_T, mhz_x10) +
                      ref_overhead(WAKE_OVERHEAD_T, mhz_x10));
}

/* The test's clock and table, as in test_load.c. */
static uint16_t g_tick;
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

/* Hand the module `span` ticks' worth of halted counts, in chunks no larger
 * than one tick (the largest the real caller ever produces), then sample. */
static void interval(muzix_kernel_proc_table_t *t, uint16_t span,
                     uint16_t busy_q14)
{
    uint32_t total = (uint32_t)span * COUNTS_PER_TICK;
    /* 64-bit intermediate: total * (16384 - busy) reaches 7.5e9 at one tick
     * of 921600 counts, past what uint32_t holds. */
    uint32_t halted = (uint32_t)(((uint64_t)total *
                                  (uint64_t)(LOAD_Q14_SCALE - busy_q14)) >> 14);

    while (halted > 0u) {
        uint16_t chunk = (halted > COUNTS_PER_TICK)
                             ? (uint16_t)COUNTS_PER_TICK
                             : (uint16_t)halted;

        muzix_load_note_halted(chunk);
        halted -= chunk;
    }
    g_tick = (uint16_t)(g_tick + span);
    muzix_load_sample(t, g_tick);
}

static void restart(muzix_kernel_proc_table_t *t, uint16_t counts)
{
    muzix_load_init(0, counts);
    g_tick = 0;
    table_ready(t);
}

int
main(void) {
    muzix_kernel_proc_table_t table;
    int f;

    /* 1. CPU frequency detection, at every supported frequency.
     *
     *    CPU_MHz_x10 = N * 3072 / 10000, where N is the CTC3 count per tick.
     *    The test feeds the exact N the calibration would measure and checks
     *    the reported frequency. */
    for (f = 0; f < 8; f++) {
        uint8_t got;

        muzix_tick_ctc3_counts_per_tick = g_counts[f];
        got = muzix_tick_get_cpu_mhz_x10();
        if (got != (uint8_t)(g_mhz[f] * 10u)) {
            printf("test_load_freq: case 1 f=%d MHz counts=%u got=%u want=%u\n",
                   g_mhz[f], g_counts[f], got, (uint8_t)(g_mhz[f] * 10u));
        }
        CHECK(got == (uint8_t)(g_mhz[f] * 10u), 1);
    }

    /* 2. The measured N must be recoverable from the frequency, and the two
     *    ends of the range must not overflow or collapse to zero. */
    for (f = 0; f < 8; f++) {
        uint32_t hz = (uint32_t)g_mhz[f] * 1000000u;
        uint16_t expect = (uint16_t)((hz + 15360u) / 30720u); /* round */

        CHECK(expect == g_counts[f], 2);
    }
    CHECK(g_counts[0] > 0u, 2);          /* 2 MHz does not read as zero */
    CHECK(g_counts[7] > 255u, 2);        /* 20 MHz needs the wide path */

    /* 3. HALT-overhead scaling, at every frequency.
     *
     *    overhead_counts(T, mhz_x10) = ceil(T * 921600 / (mhz_x10 * 100000)).
     *    The production routine must match the host reference for both the
     *    start and wake paths, and the total must be what muzix_load_init
     *    stores. */
    for (f = 0; f < 8; f++) {
        uint16_t mhz_x10 = (uint16_t)(g_mhz[f] * 10u);

        muzix_tick_ctc3_counts_per_tick = g_counts[f];
        muzix_load_init(0, g_counts[f]);
        CHECK(muzix_load_halt_overhead_counts() == ref_total_overhead(mhz_x10),
              3);
    }

    /* 4. The loadavg windows are frequency-independent.
     *
     *    The same real-time workload - 60 s at 50% busy, sampled once - must
     *    produce the same Q14 windows at every frequency, because the busy
     *    fraction is a ratio of CTC0/CTC1 reference counts and the EMA
     *    exponent is in ticks.  If any part of the path secretly used the
     *    CPU clock, this would diverge. */
    {
        uint32_t w0 = 0, w1 = 0, w2 = 0;

        for (f = 0; f < 8; f++) {
            restart(&table, g_counts[f]);
            /* One idle second from zero, then 60 s at half load. */
            interval(&table, 120, 0);
            interval(&table, (uint16_t)(60u * 120u), LOAD_Q14_SCALE / 2u);
            if (f == 0) {
                w0 = muzix_load_window(0);
                w1 = muzix_load_window(1);
                w2 = muzix_load_window(2);
            } else {
                CHECK(muzix_load_window(0) == w0, 4);
                CHECK(muzix_load_window(1) == w1, 4);
                CHECK(muzix_load_window(2) == w2, 4);
            }
        }
        /* Sanity: the common result is between zero and full, and ordered. */
        CHECK(w0 > 0u && w0 < LOAD_Q14_SCALE, 4);
        CHECK(w0 > w1 && w1 > w2, 4);
    }

    /* 5. The busy fraction itself is frequency-independent.
     *
     *    A 25/50/75% busy workload, expressed as CTC0/CTC1 reference counts,
     *    must read back the same fraction at every frequency.  This is the
     *    measurement the EMA consumes; if it moved with frequency, no amount
     *    of EMA correctness would save the figure. */
    {
        uint16_t busy_pct[3] = { LOAD_Q14_SCALE / 4u,
                                 LOAD_Q14_SCALE / 2u,
                                 (LOAD_Q14_SCALE / 4u) * 3u };
        int p;

        for (p = 0; p < 3; p++) {
            uint32_t first = 0;

            for (f = 0; f < 8; f++) {
                restart(&table, g_counts[f]);
                /* A single one-second interval at the target fraction: the
                 * first sample ASSIGNS, so the window is the measured busy
                 * fraction directly. */
                interval(&table, 120, busy_pct[p]);
                if (f == 0) {
                    first = muzix_load_window(0);
                } else {
                    CHECK(muzix_load_window(0) == first, 5);
                }
            }
            /* The measured fraction must be within one Q14 LSB of the target
             * (the ratio is a division, so allow the rounding). */
            if ((first >= busy_pct[p] ? first - busy_pct[p]
                                      : busy_pct[p] - first) > 1u) {
                printf("test_load_freq: case 5 p=%d target=%u got=%u\n",
                       p, busy_pct[p], first);
            }
            CHECK((first >= busy_pct[p] ? first - busy_pct[p]
                                        : busy_pct[p] - first) <= 1u, 5);
        }
    }

    /* 6. The CTC3 count does not leak into the busy fraction.
     *
     *    The same wall-time busy fraction, fed with two very different CTC3
     *    counts (2 MHz and 20 MHz), must give the same window.  The CTC3
     *    count is only used for the frequency display and the overhead; the
     *    halted accumulator is in CTC0/CTC1 counts. */
    {
        uint32_t slow, fast;

        restart(&table, g_counts[0]);            /* 2 MHz */
        interval(&table, (uint16_t)(30u * 120u), LOAD_Q14_SCALE / 2u);
        slow = muzix_load_window(0);

        restart(&table, g_counts[7]);            /* 20 MHz */
        interval(&table, (uint16_t)(30u * 120u), LOAD_Q14_SCALE / 2u);
        fast = muzix_load_window(0);

        CHECK(slow == fast, 6);
    }

    printf("test_load_freq: ok\n");
    printf("test_load_freq: MHz  CTC3/tick  mhz_x10  halt_overhead\n");
    for (f = 0; f < 8; f++) {
        muzix_tick_ctc3_counts_per_tick = g_counts[f];
        muzix_load_init(0, g_counts[f]);
        printf("test_load_freq: %3u  %9u  %7u  %14u\n",
               g_mhz[f], g_counts[f], muzix_tick_get_cpu_mhz_x10(),
               muzix_load_halt_overhead_counts());
    }
    return 0;
}