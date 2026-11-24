#include "ztypes.h"
#include "../platform/zeta-v2/tick.h"

uint8_t
muzix_tick_get_cpu_mhz_x10(void) {
    uint16_t counts = muzix_tick_ctc3_counts_per_tick;
    uint16_t value_100;
    uint16_t counts72;
    uint8_t result = 0;

    if (counts == 0) return 40;  /* default 4 MHz */
    /* CPU_MHz_x10 = counts * 3072 / 10000, evaluated as
     *   (counts * 30 + counts * 72 / 100 + 50) / 100
     * so every intermediate is a 16-bit quantity.
     *
     * The counts * 72 / 100 term is added straight into the running 16-bit
     * value rather than held in a uint8_t.  The quotient reaches 468 at
     * 20 MHz (651 * 72 / 100); a byte held 468 as 212 and made every
     * frequency from 12 MHz up read low - 391 counts reported 11.8 MHz
     * instead of 12.0, 521 reported 15.7, 651 reported 19.7. */
    value_100 = counts + counts + counts;            /* counts * 3 */
    value_100 = (value_100 << 3) + (value_100 << 1); /* counts * 30 */
    counts72 = (counts << 6) + (counts << 3);        /* counts * 72 */
    while (counts72 >= 100) {
        counts72 -= 100;
        value_100++;
    }
    value_100 += 50;
    while (value_100 >= 100) {
        value_100 -= 100;
        result++;
    }
    return result;
}