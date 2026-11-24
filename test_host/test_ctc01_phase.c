/*
 * test_host/test_ctc01_phase.c - Host test for CTC0/CTC1 phase calculation.
 *
 * Tests the phase formula, modulo subtraction, and overhead conversion for
 * the CTC0/CTC1 HALT accounting.
 *
 * The phase formula: phase = (29 - c1) * 256 + (255 - c0)
 * produces a value in 0..7679 representing the phase within the 120 Hz tick.
 * Subtraction is modulo 7680 using the wrap-safe pattern:
 *   elapsed = (after >= before) ? after - before : 7680 - before + after;
 *
 * Overhead conversion: ceil(T * 921600 / cpu_hz) converts CPU T-states to
 * reference-clock counts for delay compensation.
 *
 * Coherent-read model for the ISR snapshot path:
 *   c0a = read CTC0; c1 = read CTC1; c0b = read CTC0 again.
 *   CTC0 is a down-counter: c0b > c0a means a wrap (reload) occurred between
 *   the two CTC0 reads, so (c0a, c1) spans a period boundary and is incoherent.
 *   Retry up to 4 times; on success encode wake_valid = 1 + retries*4
 *   (bit 0 = valid, bits 1-2 = failed attempts 0..3).
 *   On 4 failed attempts write wake_valid = 2 (bit 0 clear, non-zero):
 *   fallback marker meaning UART woke CPU but snapshot unavailable.
 *   _muzix_halt_end() distinguishes valid snapshot (bit 0 set) from fallback
 *   (bit 0 clear, non-zero) and calls _muzix_ctc01_read_phase_locked() for
 *   the fallback path.
 */

#include "ztypes.h"
#include <stdio.h>

/* Production formula from platform/zeta-v2/tick.s:_muzix_ctc01_read_phase */
static uint16_t
phase_from_counters(uint8_t c0, uint8_t c1)
{
    return (uint16_t)(29u - c1) * 256u + (uint16_t)(255u - c0);
}

/* Wrap-safe phase difference modulo 7680, matching production kernel_loop.c. */
static uint16_t
phase_delta(uint16_t before, uint16_t after)
{
    if (after >= before) {
        return (uint16_t)(after - before);
    }
    return (uint16_t)(7680u - before + after);
}

/* ceil(ticks * 921600 / cpu_hz) - matches production overhead conversion.
 * Computed with repeated subtraction to avoid host-only 32-bit divide. */
static uint16_t
ticks_to_counts(uint16_t ticks, uint32_t cpu_hz)
{
    uint32_t num = (uint32_t)ticks * 921600u + cpu_hz - 1u;
    uint32_t result = 0;
    while (num >= cpu_hz) {
        num -= cpu_hz;
        result++;
    }
    return result;
}

/* Simulate the bounded coherent-read sequence from _muzix_uart_rx_isr.
 *
 * reads[] supplies successive CTC0 values; the function consumes three per
 * attempt (c0a, c1 is skipped, c0b).  Returns 1 if a coherent pair was found
 * within 4 attempts, 0 otherwise.  Sets *out_coherent_c0a to the accepted c0a.
 *
 * Coherence rule for a down-counter: c0b > c0a means wrap between reads.
 */
static int
coherent_read_sim(const uint8_t *reads, size_t nreads, uint8_t *out_coherent_c0a)
{
    size_t idx = 0;
    int attempt;

    for (attempt = 0; attempt < 4; attempt++) {
        if (idx + 2 > nreads) {
            return 0;           /* ran out of synthetic reads */
        }
        uint8_t c0a = reads[idx++];
        /* c1 read is between - we don't consume a CTC0 value for it */
        (void)reads[idx++];     /* placeholder for c1, not a CTC0 read */
        uint8_t c0b = reads[idx++];

        if (c0b > c0a) {
            continue;           /* wrap between reads: incoherent, retry */
        }
        *out_coherent_c0a = c0a;
        return 1;               /* coherent */
    }
    return 0;                   /* max attempts exhausted */
}

/* Encode wake_valid from retries, matching production ISR:
 *   valid snapshot: 1 + retries*4  (retries 0..3 => 1, 5, 9, 13)
 *   fallback marker: 2             (bit 0 clear, non-zero)
 *   no wake:         0
 */
static uint8_t
wake_valid_encode(int retries, int fallback)
{
    if (fallback) {
        return 2u;
    }
    return (uint8_t)(1u + (uint8_t)retries * 4u);
}

/* Decode wake_valid: returns 0 (no wake), 1 (valid snapshot), 2 (fallback). */
static int
wake_valid_classify(uint8_t wv)
{
    if (wv == 0u) {
        return 0;
    }
    if ((wv & 1u) != 0u) {
        return 1;               /* valid snapshot */
    }
    return 2;                   /* fallback marker */
}

/* Fallback overhead T-states beyond WAKE_OVERHEAD_T (44 T).
 * See production kernel/load.c:muzix_load_uart_fallback_overhead_counts().
 * Fixed total: 537 T (ld b,#4 + 4 failed retries + ISR exit + return +
 * muzix_halt_end checks + call + read_phase_locked to first IN).
 */
#define FALLBACK_OVERHEAD_T 537u

static uint16_t
fallback_overhead_counts(uint32_t cpu_hz)
{
    return ticks_to_counts(FALLBACK_OVERHEAD_T, cpu_hz);
}

static int failures;

static void
check(int ok, int line, const char *what)
{
    if (!ok) {
        printf("FAIL line %d: %s\n", line, what);
        failures++;
    }
}

int
main(void)
{
    /* ---- Phase formula -------------------------------------------------- */

    /* Both counters just reloaded: phase 0 */
    check(phase_from_counters(255u, 29u) == 0u, __LINE__, "phase 0 at reset");

    /* CTC0 at terminal count, CTC1 just reloaded: phase 255 */
    check(phase_from_counters(0u, 29u) == 255u, __LINE__, "phase 255 at c0=0");

    /* CTC0 just reloaded, CTC1 at terminal count: phase 7424 */
    check(phase_from_counters(255u, 0u) == 7424u, __LINE__, "phase 7424 at c1=0");

    /* Both at terminal count: last count before wrap, phase 7679 */
    check(phase_from_counters(0u, 0u) == 7679u, __LINE__, "phase 7679 at both=0");

    /* Arbitrary values */
    check(phase_from_counters(100u, 15u) == (29u - 15u) * 256u + (255u - 100u),
          __LINE__, "arbitrary c0/c1");

    /* ---- Modulo subtraction for elapsed time ---------------------------- */

    /* No wrap: simple subtraction */
    check(phase_delta(100u, 500u) == 400u, __LINE__, "delta no wrap");

    /* Wrap forward: after < before, mod 7680 */
    check(phase_delta(7679u, 0u) == 1u, __LINE__, "delta wraps forward by 1");
    check(phase_delta(7679u, 100u) == 101u, __LINE__, "delta wraps forward by 101");
    check(phase_delta(7500u, 100u) == 280u, __LINE__, "delta wraps forward by 280");

    /* Full period: 7680 mod 7680 = 0 (same phase) */
    check(phase_delta(100u, 100u) == 0u, __LINE__, "delta same phase is zero");

    /* ---- Overhead conversion ------------------------------------------ */

    /* ceil(T * 921600 / cpu_hz) at each supported frequency.
     * T values match production START_OVERHEAD_T=124 and WAKE_OVERHEAD_T=44. */
    check(ticks_to_counts(124u, 2000000u) == 58u, __LINE__, "2 MHz start overhead (124 T)");
    check(ticks_to_counts(44u, 2000000u) == 21u, __LINE__, "2 MHz wake overhead (44 T)");
    check(ticks_to_counts(124u, 4000000u) == 29u, __LINE__, "4 MHz start overhead (124 T)");
    check(ticks_to_counts(44u, 4000000u) == 11u, __LINE__, "4 MHz wake overhead (44 T)");
    check(ticks_to_counts(124u, 6000000u) == 20u, __LINE__, "6 MHz start overhead (124 T)");
    check(ticks_to_counts(44u, 6000000u) == 7u,  __LINE__, "6 MHz wake overhead (44 T)");
    check(ticks_to_counts(124u, 8000000u) == 15u, __LINE__, "8 MHz start overhead (124 T)");
    check(ticks_to_counts(44u, 8000000u) == 6u,  __LINE__, "8 MHz wake overhead (44 T)");
    check(ticks_to_counts(124u, 10000000u) == 12u, __LINE__, "10 MHz start overhead (124 T)");
    check(ticks_to_counts(44u, 10000000u) == 5u,  __LINE__, "10 MHz wake overhead (44 T)");
    check(ticks_to_counts(124u, 20000000u) == 6u,  __LINE__, "20 MHz start overhead (124 T)");
    check(ticks_to_counts(44u, 20000000u) == 3u,  __LINE__, "20 MHz wake overhead (44 T)");

    /* ---- Overhead total at 4 MHz -------------------------------------- */

    /* Total overhead at 4 MHz = start(29) + wake(11) = 40 counts */
    check(ticks_to_counts(124u, 4000000u) + ticks_to_counts(44u, 4000000u) == 40u,
          __LINE__, "4 MHz total overhead = 40 counts");

    /* ---- Coherent-read model (bounded retry) --------------------------- */

    /* No wrap: c0b < c0a on first attempt */
    {
        uint8_t reads[] = { 100u, 0u, 50u };   /* c0a=100, c1=skip, c0b=50 */
        uint8_t got = 0;
        check(coherent_read_sim(reads, 3, &got) == 1, __LINE__, "coherent first attempt");
        check(got == 100u, __LINE__, "coherent c0a returned");
    }

    /* Wrap between c0a and c0b: c0b > c0a, retry succeeds on attempt 2 */
    {
        uint8_t reads[] = { 5u, 0u, 250u,   /* attempt 1: wrap (250 > 5) */
                            200u, 0u, 10u }; /* attempt 2: coherent (10 < 200) */
        uint8_t got = 0;
        check(coherent_read_sim(reads, 6, &got) == 1, __LINE__, "coherent on retry 2");
        check(got == 200u, __LINE__, "coherent c0a from retry 2");
    }

    /* Boundary: c0a = 0, c0b = 255 means wrap */
    {
        uint8_t reads[] = { 0u, 0u, 255u,   /* attempt 1: wrap (255 > 0) */
                            100u, 0u, 99u }; /* attempt 2: coherent (99 < 100) */
        uint8_t got = 0;
        check(coherent_read_sim(reads, 6, &got) == 1, __LINE__, "coherent after boundary wrap");
        check(got == 100u, __LINE__, "coherent c0a after boundary wrap");
    }

    /* Boundary: c0a = 255, c0b = 0 means normal decrement (no wrap) */
    {
        uint8_t reads[] = { 255u, 0u, 0u };   /* c0a=255, c0b=0: normal decrement */
        uint8_t got = 0;
        check(coherent_read_sim(reads, 3, &got) == 1, __LINE__, "coherent at c0a=255");
        check(got == 255u, __LINE__, "coherent c0a=255 returned");
    }

    /* c0b == c0a: no change between reads, coherent */
    {
        uint8_t reads[] = { 42u, 0u, 42u };
        uint8_t got = 0;
        check(coherent_read_sim(reads, 3, &got) == 1, __LINE__, "coherent when c0b == c0a");
        check(got == 42u, __LINE__, "coherent c0a=42 returned");
    }

    /* Max attempts exhausted: 4 wraps in a row, then fail */
    {
        uint8_t reads[] = { 10u, 0u, 200u,   /* attempt 1: wrap */
                            20u, 0u, 210u,   /* attempt 2: wrap */
                            30u, 0u, 220u,   /* attempt 3: wrap */
                            40u, 0u, 230u }; /* attempt 4: wrap */
        uint8_t got = 0;
        check(coherent_read_sim(reads, 12, &got) == 0, __LINE__, "max attempts exhausted");
    }

    /* Not enough reads for 4 attempts */
    {
        uint8_t reads[] = { 10u, 0u, 200u, 20u, 0u, 210u }; /* only 2 attempts */
        uint8_t got = 0;
        check(coherent_read_sim(reads, 6, &got) == 0, __LINE__, "insufficient reads");
    }

    /* ---- wake_valid encoding/classification ----------------------------- */

    /* No wake: wake_valid == 0 */
    check(wake_valid_classify(0u) == 0, __LINE__, "wake_valid 0 = no wake");

    /* Valid snapshot with 0 retries: 1 + 0*4 = 1 */
    check(wake_valid_classify(1u) == 1, __LINE__, "wake_valid 1 = valid snapshot (0 retries)");
    check(wake_valid_encode(0, 0) == 1u, __LINE__, "encode 0 retries = 1");

    /* Valid snapshot with 1 retry: 1 + 1*4 = 5 */
    check(wake_valid_classify(5u) == 1, __LINE__, "wake_valid 5 = valid snapshot (1 retry)");
    check(wake_valid_encode(1, 0) == 5u, __LINE__, "encode 1 retry = 5");

    /* Valid snapshot with 2 retries: 1 + 2*4 = 9 */
    check(wake_valid_classify(9u) == 1, __LINE__, "wake_valid 9 = valid snapshot (2 retries)");
    check(wake_valid_encode(2, 0) == 9u, __LINE__, "encode 2 retries = 9");

    /* Valid snapshot with 3 retries: 1 + 3*4 = 13 */
    check(wake_valid_classify(13u) == 1, __LINE__, "wake_valid 13 = valid snapshot (3 retries)");
    check(wake_valid_encode(3, 0) == 13u, __LINE__, "encode 3 retries = 13");

    /* Fallback marker: 2 (bit 0 clear, non-zero) */
    check(wake_valid_classify(2u) == 2, __LINE__, "wake_valid 2 = fallback marker");
    check(wake_valid_encode(0, 1) == 2u, __LINE__, "encode fallback = 2");

    /* Fallback marker distinct from all valid snapshot values */
    check(wake_valid_encode(0, 0) != wake_valid_encode(0, 1), __LINE__,
          "valid(0 retries) != fallback");
    check(wake_valid_encode(1, 0) != wake_valid_encode(0, 1), __LINE__,
          "valid(1 retry) != fallback");
    check(wake_valid_encode(2, 0) != wake_valid_encode(0, 1), __LINE__,
          "valid(2 retries) != fallback");
    check(wake_valid_encode(3, 0) != wake_valid_encode(0, 1), __LINE__,
          "valid(3 retries) != fallback");

    /* ---- Fallback overhead counts --------------------------------------- */

    /* 537 T at each supported frequency */
    check(fallback_overhead_counts(2000000u) == 248u, __LINE__,
          "2 MHz fallback overhead (537 T)");
    check(fallback_overhead_counts(4000000u) == 124u, __LINE__,
          "4 MHz fallback overhead (537 T)");
    check(fallback_overhead_counts(6000000u) == 83u, __LINE__,
          "6 MHz fallback overhead (537 T)");
    check(fallback_overhead_counts(8000000u) == 62u, __LINE__,
          "8 MHz fallback overhead (537 T)");
    check(fallback_overhead_counts(10000000u) == 50u, __LINE__,
          "10 MHz fallback overhead (537 T)");
    check(fallback_overhead_counts(20000000u) == 25u, __LINE__,
          "20 MHz fallback overhead (537 T)");

    if (failures) {
        printf("test_ctc01_phase: %d failures\n", failures);
        return 1;
    }
    printf("test_ctc01_phase: all pass\n");
    return 0;
}
