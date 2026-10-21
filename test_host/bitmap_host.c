/*
 * Host stand-ins for platform/zeta-v2/bitmap_ops.s.
 *
 * WHY THESE EXIST
 * ---------------
 * kernel_bank_model.c's allocator walks the page bitmap through the assembly
 * module, and kernel/test_syscalls.c drives that allocator from a host test
 * binary.  The host pool is C sources only - the .s modules are not compiled
 * there - so without these two the test binary would not link, and a gate that
 * cannot link is a gate that cannot run.
 *
 * WHAT IS HERE IS THE REFERENCE, NOT A FAKE
 * -----------------------------------------
 * Both are direct transcriptions of the assembly, written so that a host test
 * can check the walk against an independent implementation: same invariants
 * (a byte_count*8 index space, -1 for full), same walk (bytes then bit
 * positions from bit 0), same set/clear.  What is not modelled is anything
 * about speed: on the host the C is faster than the Z80, and what the target
 * measurements in bitmap_ops.s's header are about is the Z80's C code, not this.
 */

#include "bitmap_ops.h"

#include <stdint.h>

int16_t muzix_bitmap_alloc(void *map, uint16_t byte_count)
{
    uint8_t *bytes = (uint8_t *)map;
    uint16_t index = 0;
    uint16_t byte_index;

    if (!map) {
        return -1;
    }
    for (byte_index = 0; byte_index < byte_count; byte_index++) {
        uint8_t free_bits = (uint8_t)~bytes[byte_index];
        uint8_t bit;

        for (bit = 0; bit < 8u; bit++) {
            if (free_bits & (uint8_t)(1u << bit)) {
                bytes[byte_index] |= (uint8_t)(1u << bit);
                return (int16_t)(byte_index * 8u + bit);
            }
        }
        index += 8u;
    }
    return -1;
}

void muzix_bitmap_release(void *map, uint16_t index)
{
    uint8_t *bytes = (uint8_t *)map;

    if (!map) {
        return;
    }
    bytes[index >> 3] &= (uint8_t)~(uint8_t)(1u << (index & 0x07u));
}
