#ifndef MUZIX_BITMAP_OPS_H
#define MUZIX_BITMAP_OPS_H

#include <stdint.h>

/* The page allocator's bitmap scan and clear, in platform/zeta-v2/bitmap_ops.s.
 *
 * The scan-and-set is one operation because the caller has nothing to do
 * between finding a free bit and taking it - the C it replaces read the mask,
 * masked it and wrote it back as three separate statements, and the middle one
 * cannot be interrupted by anything that matters, but stating it as one call
 * makes that a property of the API rather than of the call site.
 *
 * `byte_count` is the map's size in bytes and the allocable indices are
 * 0 .. byte_count*8 - 1: a partial last byte is not masked off here, so the
 * caller must size the map to the bit count.  The tree's is four bytes for
 * ZETA_RAM_PAGES bits.
 *
 * Returns the index of the bit it set, or -1 when the map is full.  Plain
 * -mz80: 16-bit return in DE (AGENTS.md's convention). */
int16_t muzix_bitmap_alloc(void *map, uint16_t byte_count);

/* Clear the bit at `index`.  An index past the map is not refused - the mask
 * walk would produce a zero mask and the clear a no-op - so the caller's own
 * range check stays where it is. */
void muzix_bitmap_release(void *map, uint16_t index);

#endif
