/*
 * muzix_load - CPU busy fraction, three windows, kept by the kernel
 *
 * WHY THIS IS HERE AND NOT IN apps/top.c
 * --------------------------------------
 * apps/top.c keeps its averages in its own memory, so the history exists only
 * while top runs: exit it and there is none, start it and the figures climb
 * from a seed for three emulated minutes.  A load figure that only exists while
 * you are looking at it is not a load figure.  The kernel is the only thing here
 * that lives continuously, so the state lives here and every program reads the
 * same three numbers.
 *
 * IT IS NOT THE UNIX LOAD AVERAGE, and the name should not pretend otherwise.
 * That is the number of processes WAITING for the processor, and it cannot be
 * measured on this machine: the scheduler is cooperative, a running process owns
 * the CPU and is not waiting, and a process parked in the console's bounded poll
 * is not a dispatch candidate at all - which is why it never runs.  What can be
 * measured here is TIME, and time is what the kernel already accounts:
 * muzix_proc_table_charge() charges a process's two counters at both ends of
 * every system call, so between two charges they advance at exactly the CTC's
 * rate.
 *
 * WHY IT IS UPDATED AT READ TIME
 * ------------------------------
 * The obvious place to accumulate this is the 120 Hz handler, and it is the one
 * place it must not go.  tick.s argues at length that the handler may address
 * only windows 0 and 3 and must never grow a tail call, because it can be taken
 * at any instant including between the `out (0x79)` and the `out (0x7a)` of
 * write_all_banks() with the map half installed.  So the handler is not touched
 * at all: nothing here runs in interrupt context.
 *
 * Instead the state carries the stamp of the last sample and the update happens
 * when someone asks.  That is correct rather than merely cheaper, because the
 * figure wanted is the average over an interval, and an interval is only known
 * once it has ended - which is what the reader supplies.  A reader that never
 * comes costs nothing; one that comes ten times a second gets the same answer
 * ten times a second.
 *
 * NO LIBRARY, NO HELPER, NO 32-BIT C
 * ------------------------------------
 * Three things this needs do not exist on this target: a 32-bit multiply, a
 * divide, and an exponential.  None is imported.
 *
 * The multiply is Q14 x Q14 shifted back down by 14, which fits in a uint16_t,
 * so the 28-bit product in between never has to be formed: four 8x8 partial
 * products, every one of them inside 16 bits, recombined with carries that are
 * also 16.  The halves are uint8_t on purpose - SDCC emits a call to __mulint
 * for a 16x16 and a shift-add for an 8x8, and __mulint is a helper this kernel
 * does not carry.  An earlier version widened the halves to uint16_t and the
 * link said so.  A later version used 32-bit products and measured 724 bytes for
 * the multiply alone.
 *
 * The divide is repeated subtraction, bounded because the numerator is clamped
 * first.
 *
 * The exponential is exp(-dt/tau) as a per-SECOND Q14 factor raised to a power,
 * by squaring, which needs the same multiply and nothing else.  Per second
 * rather than per tick: exp(-1/108000) in Q14 is 16384, exactly 1.0, so a
 * per-tick factor would leave every window permanent.  The three windows share
 * one walk over the exponent's bits, because three walks are three copies of
 * the same loop and this is _CODE the kernel does not have.
 *
 * WHAT THE THREE NUMBERS ARE
 * --------------------------
 * Q14 fractions of wall time that the CPU was not idle, exponentially damped
 * with 1, 5 and 15 minute constants - the textbook recurrence, fed with a
 * quantity this machine can actually measure.  The first reading is seeded from
 * since boot rather than from zero.
 *
 * ONE KNOWN LIMIT
 * ----------------
 * The decay exponent is whole seconds rounded to nearest, so a reader asking
 * more often than every half second gets no decay at all and the windows stop
 * moving.  Carrying the remainder would fix that and costs a counter the kernel
 * has no byte for.  The readers here are seconds to minutes apart, and asking
 * ten times a second is answered identically ten times anyway.
 */

#include "load.h"

extern uint8_t muzix_tick_get_cpu_mhz_x10(void);

#include "../platform/zeta-v2/tick.h"

/* Q14: 1.0 == 16384. */
#define LOAD_SCALE 16384u

/* The CTC tick, which is what g_load_tick counts.  kernel/proc_table.c says 120
 * ticks make a second exactly, and the decay factors below are per SECOND, so
 * this is the conversion the exponent walk needs. */
#define LOAD_TICK_HZ 120u

/* exp(-1/tau) in Q14, for one SECOND.  The same values apps/top.c carried as
 * LOAD_TAU_1/5/15 - 16113, 16329, 16366 - so the windows keep their meaning and
 * only stop being lost when the program exits. */
static const uint16_t g_load_factor[3] = { 16113u, 16329u, 16366u };

/* The state.  Twelve bytes of accumulators, a tick stamp, an awake-count
 * accumulator, a boot tick and a ready flag - in _DATA, which is why the figures
 * survive every process exit. */
static uint32_t g_load[3];
static uint16_t g_load_tick;
static uint32_t g_load_halted;
static uint16_t g_load_boot_tick;
static uint8_t  g_load_ready;

/* Sub-second decay, applied to every actual elapsed interval so that a change
 * in the busy fraction between two sub-second samples is not lost.
 *
 * The EMA over a span of `whole` seconds and `frac` ticks (0..119) is
 *
 *   L(t+dt) = L(t) * exp(-dt/tau) + B * (1 - exp(-dt/tau))
 *
 * with dt = whole + frac/120.  exp(-dt/tau) = f^whole * f^(frac/120), where
 * f = g_load_factor[i] = exp(-1/tau) per second.  f^(frac/120) is 1 - frac/(120*tau)
 * to first order, and the coefficient frac/(120*tau) is exact to the Q14 LSB
 * for frac >= 12 ticks (100 ms); below that it rounds to 1 LSB.
 *
 * g_load_frac_k3[i] = round(2^22 / (120*tau[i])) << 6, so that
 * muzix_load_mul(frac, g_load_frac_k3[i]) = (frac * K) >> 8 = frac * 16384 / (120*tau).
 * For the three windows: 583, 117, 39 (before the << 6), i.e. 37312, 7488, 2496. */
static const uint16_t g_load_frac_k3[3] = { 37312u, 7488u, 2496u };

/* HALT measurement overhead in CTC0/CTC1 reference counts (one count =
 * 1/921600 s).  Computed in muzix_load_init() from the detected CPU frequency
 * and the fixed instruction paths in muzix_halt_begin() and the ISRs.
 *
 * START_OVERHEAD_T: T-states from the start-phase timestamp (start of first
 *   IN in muzix_halt_begin) to the HALT instruction, including the first IN
 *   itself (the timestamp is at its start) and the HALT.
 *   Path: in CTC0(12) + ld b(4) + in CTC1(12) + ld c(4) + in CTC0(12) +
 *   cp b(4) + jr z taken(12) + ld a,#29(7) + sub c(4) + ld d(4) +
 *   ld a,#255(7) + sub b(4) + ld e(4) + ld hl,#wake_valid(10) +
 *   ld (hl),#0(10) + ei(4) + halt(4) = 118 T (coherent snapshot path)
 * WAKE_OVERHEAD_T: T-states from interrupt entry to the wake-phase timestamp
 *   (first IN in the ISR), including IM2 dispatch.
 *   IM2 acknowledge(19) + CALL(17) + ex af,af'(4) + exx(4) = 44 T
 *   Same for both tick and UART ISRs.
 */
#define START_OVERHEAD_T 118u
#define WAKE_OVERHEAD_T  44u

/* CTC0/CTC1 single-snapshot error bound:
 *
 * The phase is read as c0a → c1 → c0b over 32 CPU T-states.  CTC0 changes at
 * UART_CLK/2 = 921600 Hz, independent of CPU clock, so it decrements D =
 * 32 * 921600 / CPU_Hz times between the two CTC0 reads.  D is 15 at 2 MHz,
 * 8 at 4 MHz, 1 or 2 at 20 MHz (never 0: 32 CPU cycles > CTC0 period).
 *
 * If CTC0 wraps between c0a and c0b but CTC1 has not yet seen the ZC/TO
 * when c1 is read, the computed phase lags the true phase at the first IN
 * by 256 - D.  D >= 1, so the strict upper bound is 255 phase units
 * (3.3% of one 7680-count tick).  If CTC1 saw the wrap, the error is D
 * (<= 20 units).  The error is bounded, mostly positive, and averaged out
 * by the EMA over multiple samples. */

static uint16_t g_halt_overhead_counts;

/* Forward declaration: defined below at overhead_counts(). */
static uint16_t overhead_counts(uint8_t t, uint8_t mhz_x10);

/* Return the computed halt overhead in CTC0/CTC1 reference counts. */
uint16_t
muzix_load_halt_overhead_counts(void) {
    return g_halt_overhead_counts;
}

/* Record `counts` of the time the processor has just spent stopped.
 *
 * `counts` is CTC channel 3, so one count is MUZIX_CTC_CYCLE_DIV (256) cycles.
 * The figure the windows hold is the fraction of the interval the machine was NOT
 * stopped, so this is what gets subtracted from the interval's own length.
 *
 * CALL IT WITH THE HALT, NOT WITH THE LOOP AROUND IT
 * --------------------------------------------------
 * The version before this was handed the scheduler's awake gap instead, and it
 * measured only the kernel's own housekeeping.  That is a true fact about a true
 * thing and it is the wrong fact: a machine running a process at full tilt never
 * reaches the idle loop, so nothing was reported and the figure read 0.00 on a
 * machine that was 100% busy.  Measured with `top` running, 0.05 reported against
 * 24.75% actually busy.  A statistic that falls to zero when the machine gets
 * busy is not a statistic about the machine.
 *
 * The gap is also not measurable by the tick, which is why this is a CTC count
 * and not a tick difference: a halt is ENDED BY THE TICK, so bracketing one reads
 * 1 however long it was, and the figure comes out as 2b - 1 - zero for anything
 * under half.  Tried, and measured at 0.01 against 7.63% true.
 *
 * WHY THIS IS MEASURED AT ALL: THE KERNEL'S OWN WORK USED TO BE INVISIBLE
 * ------------------------------------------------------------------------
 * The figure used to be the sum of every process's user and system time over the
 * interval.  A parked process is not running, so nothing was charged for it, and
 * that part was right - but the loop that decides whether to park belongs to the
 * kernel, and with no process current there was nothing to charge it to either.
 * So the kernel's own work fell out of the arithmetic completely.  Measured on the
 * emulator against an independent count of cycles actually spent halted, a machine
 * that was idle and reported 0.02 was busy about 9% of the time, and the gap did
 * not shrink as the idle interval grew: it was a standing cost of roughly 5500
 * cycles per 120 Hz wakeup - the IM2 acknowledge, the handler's seven register
 * pairs, and muzix_proc_table_should_idle()'s walk of the table.
 *
 * "Fraction of wall time some process was on the CPU" is a legitimate statistic,
 * but on a machine where the scheduler's own loop is a measurable share of the
 * total it is the wrong one to print next to a word like cpu, and nothing in the
 * system could see the cost it was not counting.
 *
 * Per-process UTIME and STIME are untouched and still mean what they say: one
 * process's own running time.  Those are a process's accounting.  This is the
 * machine's.
 *
 * WHY NOT THE OTHER WAY ROUND, WHICH WAS TRIED FIRST AND WAS WRONG
 * ---------------------------------------------------------------
 * Bracketing the halt and subtracting was the first version, and it does not
 * work, for a reason that is not obvious enough to skip.  A halt is ended BY the
 * tick, so a tick bracketing it reads exactly 1 however long the halt was, while
 * the true length is uniform in (0, 1 tick) because the processor enters a halt
 * at a random phase in the tick period.  Each halt is therefore recorded as about
 * twice its real length, which turns a true busy fraction b into 2b - 1 - and for
 * anything under a half that is negative, i.e. a machine that is 8% busy reports
 * zero.  Measured: 0.01 reported against 0.077 true.
 *
 * A halt is at most one tick - 61440 cycles, about 130 counts at 4 MHz - and the counter is 8
 * bits, so it holds one with 16 counts to spare.  The only halt it cannot hold is
 * one longer than a tick, which means the tick was missed.
 *
 * The kernel holds no 32-bit multiply and cannot link one, which is why the
 * arithmetic below converts ticks to counts by shifting rather than multiplying.
 * At the board's 4 MHz CPU clock, 61440 cycles per tick / 256 cycles per CTC3
 * count is 130.208 counts per tick; load_span_counts() uses the nearest integer. */
void
muzix_load_note_halted(uint16_t counts) {
    g_load_halted += counts;
}

/* (a * b) >> 14, in Q14, with no 32-bit arithmetic anywhere.
 *
 * RE-DERIVED FROM THE FOUR PARTIAL PRODUCTS, FROM SCRATCH
 * ------------------------------------------------------
 * Cut both operands into bytes:  a = ah*256 + al,  b = bh*256 + bl.  Then
 *
 *     p0 = al*bl   weight 2^0
 *     p1 = ah*bl   weight 2^8
 *     p2 = al*bh   weight 2^8
 *     p3 = ah*bh   weight 2^16
 *
 *     a*b = p0 + (p1 + p2)*2^8 + p3*2^16
 *
 * Every p is at most 255*255 = 65025, so each fits in 16 bits and SDCC expands
 * it inline as a shift-add; a uint16_t*uint16_t instead emits a call to
 * __mulint, which this kernel cannot link (AGENTS.md, "The kernel holds no
 * 32-bit multiply").  The product itself is 32 bits and never exists as one:
 * it is carried as the two 16-bit halves `low` and `high` below.
 *
 * THE 17-BIT MIDDLE SUM, AND WHY ITS CARRY WEIGHS 256
 * ---------------------------------------------------
 * p1 + p2 reaches 65025 + 65025 = 130050, which does not fit in 16 bits, so
 * split it into a 16-bit part and a carry-out:
 *
 *     p1 + p2 = 65536*C + S,   S = (p1 + p2) & 0xFFFF,   C = carry out (0 or 1)
 *
 * Substituting, and grouping by what lands in the low 16 bits of the product
 * and what lands above them:
 *
 *     a*b = p0 + S*2^8 + C*2^24 + p3*2^16
 *         = (p0 + (S & 0xFF)*2^8)  +  ((S >> 8) + C*256 + p3)*2^16
 *
 * Two things fall out of that grouping, and both were got wrong here before
 * they were caught:
 *
 *   1. The carry C out of the 17-bit sum enters the HIGH half with weight
 *      2^24 / 2^16 = 256.  Writing `+ 1` there is a 256-fold error, and it is
 *      the mistake AGENTS.md records (7 of 225 operand pairs, then 93 of 225).
 *      `carry << 8` IS that 256, and it is the whole reason the shift is there.
 *   2. The high half can itself overflow: (S >> 8) + C*256 + p3 reaches
 *      255 + 256 + 65025 = 65536.  It cannot in fact, because the whole
 *      product is at most 65535*65535 = 0xFFFE0001, so a*b >> 16 is at most
 *      0xFFFE = 65534 and the sum above is bounded by the product's own top
 *      half.  The bound is worth stating rather than trusting: it is what
 *      makes the additions below 16-bit instead of needing a 17-bit piece.
 *
 * THE CARRY OUT OF THE LOW HALF
 * -----------------------------
 * p0 + (S & 0xFF)*2^8 can itself exceed 65535, and that carry is detected the
 * only way it can be without a wider add: `t < p0` after `t = low + p0`, which
 * is true exactly when the addition wrapped.  Note it is compared against p0
 * and not against `low` - `low + p0 >= 65536` implies `t = low + p0 - 65536 <
 * p0`, and the converse holds too, so the comparison is exact.
 *
 * WHAT THE 16-BIT RETURN CAN AND CANNOT HOLD
 * ------------------------------------------
 * The result is bits 14 and above of the 32-bit product: `(high << 2) |
 * (low >> 14)`.  For Q14 operands - the production range, where a, b <= 16384
 * and (a*b) >> 14 <= 16384 - that is exact and complete.  Above Q14 the true
 * value needs 18 bits (65535*65535 >> 14 = 262136) and the cast to uint16_t
 * keeps its low 16; that is the documented behaviour of the return type rather
 * than a second defect, and kernel/test_load.c asserts it exactly that way.
 */
uint16_t
muzix_load_mul(uint16_t a, uint16_t b) {
    uint8_t al = (uint8_t)(a & 0xFFu);
    uint8_t ah = (uint8_t)(a >> 8);
    uint8_t bl = (uint8_t)(b & 0xFFu);
    uint8_t bh = (uint8_t)(b >> 8);
    uint16_t p0 = (uint16_t)(al * bl);
    uint16_t p1 = (uint16_t)(ah * bl);
    uint16_t p2 = (uint16_t)(al * bh);
    uint16_t p3 = (uint16_t)(ah * bh);
    /* The 17-bit middle sum, as a 16-bit low part plus its carry. */
    uint16_t mid = (uint16_t)(p1 + p2);
    uint16_t carry = (mid < p1) ? 1u : 0u;
    /* bits 0..15 of the product, and bits 16..31 of it. */
    uint16_t low = (uint16_t)(mid << 8);
    uint16_t high = (uint16_t)((mid >> 8) + (uint16_t)(carry << 8));
    uint16_t t;

    t = (uint16_t)(low + p0);
    high = (uint16_t)(high + ((t < p0) ? 1u : 0u));
    low = t;
    high = (uint16_t)(high + p3);

    return (uint16_t)((high << 2) | (low >> 14));
}

/* (spent << 14) / capacity, by subtraction.  The caller clamps, so the quotient
 * is at most LOAD_SCALE and the loop is bounded. */
static uint32_t
load_div(uint32_t spent, uint32_t cycles) {
    /* cycles = span * 7680 fits in uint32_t (max 65535 * 7680 = 503,316,480).
     * spent <= cycles, so spent * LOAD_Q14_SCALE fits in uint64_t.
     */
    if (cycles == 0) return LOAD_Q14_SCALE;
    if (spent >= cycles) return LOAD_Q14_SCALE;
    uint32_t result = 0;
    uint32_t remainder = spent;
    int i;

    for (i = 0; i < 14; i++) {
        remainder = remainder * 2;
        if (remainder >= cycles) {
            result = (result << 1) | 1u;
            remainder -= cycles;
        } else {
            result = result << 1;
        }
    }
    return result;
}
/* (spent << 14) / capacity, for arguments too large to scale.
 *
 * load_div() computes the quotient by scaling the numerator left by 14 and
 * subtracting, which is exact and needs no divide - but it is only exact while
 * `spent << 14` fits in 32 bits, i.e. while spent is below 2^18 = 262 143.  The
 * busy accumulator is in CTC counts and a 15-minute interval is 25 920 000 of
 * them, so the obvious call overflows: the shifted value truncates to 32 bits and
 * comes back as a plausible-looking fraction for an interval that was completely
 * idle, which is worse than useless because it looks like an answer.  Measured
 * here before this existed: a window of 1 728 000 counts came back as 930 of
 * 16384, a fraction of 0.06, for an interval that was fully busy.
 *
 * So both arguments are shifted right together until the denominator fits.  The
 * ratio is preserved up to the truncation, and the worst case is a 15-minute
 * window shifted 7 bits, which costs 1/128 of the fraction - about 0.4% at 0.5 and
 * nothing at all at 0.03.
 *
 * Shifting BOTH is what keeps this a ratio: shifting only the numerator would
 * divide the true value by up to 128, and shifting only the denominator would
 * multiply it.  load_div() itself is unchanged and is still what the window
 * arithmetic uses, where the arguments are Q14 fractions and 2^18 is never
 * approached. */
static uint32_t
load_ratio(uint32_t spent, uint32_t capacity) {
    /* 0x3FFFF, not 0x3FFFFF.  The limit is where `spent << 14` still fits in 32
     * bits, and 2^32 / 2^14 is 2^18 - so the threshold is 18 bits of mask, and
     * writing seven hex digits here puts it four bits too high, which lets
     * anything up to 4 194 303 through to the overflow.  Measured with the extra
     * four bits: a 1 728 000-count interval, fully busy, came back as 1470 of
     * 16384.  It is the kind of constant that looks right and is not. */
    while (capacity > 0x3FFFFu) {
        spent >>= 1;
        capacity >>= 1;
    }
    return load_div(spent, capacity);
}

/* The interval expressed in calibrated CTC3 counts. Multiply using shift-and-add
 * to avoid an SDCC 32-bit multiply helper that the kernel cannot link. */
static uint32_t
load_span_counts(uint16_t span) {
    /* span * 7680 = span * (4096 + 2048 + 1024 + 512)
     * All constant shifts on uint32_t, no multiply helper needed. */
    uint32_t s = (uint32_t)span;
    return (s << 12) + (s << 11) + (s << 10) + (s << 9);
}

/* Compute ceil(T * 921600 / CPU_Hz) in CTC0/CTC1 reference counts,
 * without any 32-bit multiply or divide helper.
 *
 * T is the instruction path length in T-states (<= 118).
 * CPU_Hz = mhz_x10 * 100000 (2 MHz .. 20 MHz).
 * Result fits in uint16_t (<= 58 at 2 MHz).
 *
 * num = T * 921600 computed as shift-and-add on uint32_t:
 *   T * 9216 = (T << 13) + (T << 10)      [T * 8192 + T * 1024]
 *   T * 921600 = (T * 9216) * 100 = ((T * 9216) << 6) + ((T * 9216) << 5) + ((T * 9216) << 2)
 * denom = CPU_Hz = mhz_x10 * 100000 computed as shift-and-add:
 *   mhz_x10 * 1000 = (mhz_x10 << 10) - ((mhz_x10 << 4) + (mhz_x10 << 3))
 *   CPU_Hz = (mhz_x10 * 1000) * 100 = << 6 + << 5 + << 2
 * Ceil division by repeated subtraction (bounded: result <= 58). */
static uint16_t
overhead_counts(uint8_t t, uint8_t mhz_x10) {
    uint32_t t32 = (uint32_t)t;
    uint32_t num = (t32 << 13) + (t32 << 10);          /* T * 9216 */
    num = (num << 6) + (num << 5) + (num << 2);        /* T * 921600 */

    uint32_t mx32 = (uint32_t)mhz_x10;
    uint32_t mx1000 = (mx32 << 10) - ((mx32 << 4) + (mx32 << 3));  /* mhz_x10 * 1000 */
    uint32_t denom = (mx1000 << 6) + (mx1000 << 5) + (mx1000 << 2); /* mhz_x10 * 100000 */

    /* Ceil: (num + denom - 1) / denom by repeated subtraction. */
    num += denom - 1u;
    uint16_t counts = 0;
    while (num >= denom) {
        num -= denom;
        counts++;
    }
    return counts;
}

void
muzix_load_init(uint16_t now_ticks, uint16_t counts_per_tick) {
    int i;

    (void)counts_per_tick; /* Fixed at 7680 counts per tick (CTC0/CTC1 cascade) */

    for (i = 0; i < 3; i++) {
        g_load[i] = 0;
    }
    g_load_tick = now_ticks;
    /* Zero, and that is a fact rather than a placeholder: no gap has been measured
     * yet at boot, so nothing is known to have been awake and the first interval
     * is reported as fully busy.  That is what the first reading of 1.00 is. */
    g_load_halted = 0;
    g_load_boot_tick = now_ticks;
    g_load_ready = 0;

    /* Compute halt overhead in CTC0/CTC1 reference counts from the detected
     * CPU frequency.  Overhead in counts = ceil(T * 921600 / CPU_Hz) where
     * CPU_Hz = mhz_x10 * 100000.  T is the fixed instruction path length
     * (START_OVERHEAD_T from phase-timestamp to HALT, WAKE_OVERHEAD_T from
     * ISR entry to wake timestamp).
     *
     * All arithmetic uses shifts and repeated subtraction only - no 32-bit
     * multiply or divide helpers, which the kernel cannot link.  Results are
     * small (<= 58 counts at 2 MHz), so the subtraction loop is bounded. */
    {
    uint8_t mhz_x10 = muzix_tick_get_cpu_mhz_x10();
    g_halt_overhead_counts = (uint16_t)(
        overhead_counts(START_OVERHEAD_T, mhz_x10) +
        overhead_counts(WAKE_OVERHEAD_T,  mhz_x10));
    }
}

void
muzix_load_sample(muzix_kernel_proc_table_t *table, uint16_t now_ticks) {
    uint32_t span;
    uint32_t capacity;
    uint32_t halted;
    uint32_t busy;
    uint32_t d[3];
    uint32_t f[3];
    uint16_t rest;
    uint16_t whole;
    uint16_t frac;
    int i;

    if (!table) return;
    /* The table is still required - it is what makes this routine reachable from a
     * test with a clock the test controls - but the figure no longer comes out of
     * it.  What the processor did is measured from the CTC cycle counter now. */
    halted = g_load_halted;

    if (g_load_ready == 0) {
        /* FIRST SAMPLE: SET, DO NOT FOLD.
         *
         * There is no previous interval, but there IS the one interval that
         * exists - since muzix_load_init().  Folding it into an average that
         * started at zero would be the same as starting from zero with extra
         * steps: after 120 seconds of 100% load the one-minute window reads 0.14,
         * because exp(-2) is 0.135.  So the first sample ASSIGNS, and the windows
         * are true from the first read.
         *
         * kernel/test_load.c case 1 is what found this: it asserted the window
         * equalled 1.00 after one interval of full load, and the code produced
         * 0.14, because the comment here claimed a seeding that the code below
         * was not doing. */
        span = (uint16_t)(now_ticks - g_load_boot_tick);
        if (span == 0) return;
        capacity = load_span_counts(span);
        if (halted > capacity) {
            halted = capacity;
        }
        busy = load_ratio(capacity - halted, capacity);
        if (busy > LOAD_SCALE) {
            busy = LOAD_SCALE;
        }
        for (i = 0; i < 3; i++) {
            g_load[i] = busy;
        }
        g_load_halted = 0;
        g_load_tick = now_ticks;
        g_load_ready = 1;
        return;
    }
    span = (uint16_t)(now_ticks - g_load_tick);

    if (span == 0) return;

    /* Occupancy: the interval's own length minus the part of it the processor was
     * stopped for.  That is the whole of it, and it is a partition - every cycle
     * is either halted or executing - so there is nothing unattributed.
     *
     * Clamped both ways, because the halted figure is a sum of 8-bit subtractions
     * and one of them can land on the wrong side of a wrap.  A figure above 1.0 is
     * not a thing, and neither is a negative one; a miss would otherwise show as
     * either. */
    capacity = load_span_counts(span);
    if (halted > capacity) {
        halted = capacity;
    }
    busy = load_ratio(capacity - halted, capacity);
    if (busy > LOAD_SCALE) {
        busy = LOAD_SCALE;
    }
    g_load_halted = 0;

    /* exp(-seconds/tau) for all three windows in ONE walk over the exponent's
     * bits.
     *
     * SECONDS, not ticks.  g_load_factor[] holds exp(-1/tau) for ONE SECOND, so
     * raising it to the power of a TICK count raises it 120 times too often and
     * the windows decay to nothing in a couple of seconds - the 15-minute window
     * reaches its final value inside 30 seconds.  kernel/test_load.c case 6 is
     * what found it: the run converged to 8192 after one step and all three
     * windows came out indistinguishable.
     *
     * Split the span into whole seconds and a sub-second remainder.  The
     * whole-second part uses g_load_factor[i] exactly; the remainder is applied
     * below with g_load_frac_k3[i], so a span that changes the busy fraction
     * between sub-second samples still contributes its own B. */
    rest = (uint16_t)span;
    whole = 0;
    while (rest >= LOAD_TICK_HZ) {
        rest = (uint16_t)(rest - LOAD_TICK_HZ);
        whole++;
    }
    frac = rest;
    for (i = 0; i < 3; i++) {
        d[i] = LOAD_SCALE;
        f[i] = g_load_factor[i];
    }
    while (whole != 0) {
        if ((whole & 1u) != 0) {
            for (i = 0; i < 3; i++) {
                d[i] = muzix_load_mul((uint16_t)d[i], (uint16_t)f[i]);
            }
        }
        whole = (uint16_t)(whole >> 1);
        if (whole != 0) {              /* the last squaring feeds nothing */
            for (i = 0; i < 3; i++) {
                f[i] = muzix_load_mul((uint16_t)f[i], (uint16_t)f[i]);
            }
        }
    }

    /* Sub-second decay: d *= exp(-frac/(120*tau)) ≈ 1 - frac/(120*tau).
     * This is what makes a 250 ms busy + 750 ms idle pair different from a
     * single 1 s sample: the busy 250 ms contributes its own B, then the idle
     * 750 ms decays it, rather than the last sample's B being applied to the
     * whole second. */
    if (frac != 0u) {
        for (i = 0; i < 3; i++) {
            uint16_t decay = muzix_load_mul((uint16_t)frac, g_load_frac_k3[i]);
            uint16_t df = (uint16_t)(LOAD_SCALE - decay);

            d[i] = muzix_load_mul((uint16_t)d[i], df);
        }
    }

    /* acc = acc*alpha + busy*(1 - alpha), where d IS alpha: exp(-dt/tau), the
     * fraction of the old value that SURVIVES the interval.
     *
     * This is two multiplies, and the one-multiply version is wrong.  The
     * tempting `acc += (busy - acc) * alpha` expands to acc*(1-alpha) +
     * busy*alpha - the retention on the NEW sample and the complement on the
     * old one, which is the recurrence backwards.  With a short interval that
     * is a difference nobody would see: a step of 1 second over a 60-second
     * constant moves the value by 1.6% either way.  Over a 120-second interval
     * the short window must drop to alpha = 0.135 and the long one must stay
     * near 0.875, and the inverted form does precisely the opposite: the long
     * window collapses and the short one barely moves, so a stale reading looks
     * like a fresh one.
     *
     * kernel/test_load.c case 3 is what caught it, by checking that the short
     * window ends up BELOW the long one after an idle interval. */
    for (i = 0; i < 3; i++) {
        uint32_t keep = muzix_load_mul((uint16_t)g_load[i], (uint16_t)d[i]);
        uint32_t take = muzix_load_mul((uint16_t)busy,
                                  (uint16_t)(LOAD_SCALE - d[i]));

        g_load[i] = keep + take;
    }

    g_load_tick = now_ticks;
}

uint32_t
muzix_load_window(int index) {
    if (index < 0 || index > 2) return 0;
    return g_load[index];
}

/* The fractional-decay coefficient for a window, exposed so the host test can
 * derive round(2^22 / (120*tau)) << 6 independently and compare. */
uint16_t
muzix_load_frac_coef(int index) {
    if (index < 0 || index > 2) return 0;
    return g_load_frac_k3[index];
}
