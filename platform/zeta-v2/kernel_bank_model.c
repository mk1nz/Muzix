#include "test_kernel/bank_model.h"
#include "bank_io.h"
#include "z80_io.h"
#include "context_switch.h"
#include "trace.h"
#include "bitmap_ops.h"

#include <string.h>

/* The page bitmap, and it is bytes rather than the uint32_t it used to be.
 *
 * The 32-bit mask read better in C and cost ten times as much on the Z80:
 * `(g_used_pages & ((uint32_t)1u << index))` made SDCC shift a 32-bit one by
 * a variable amount on every one of the 32 steps, and the whole scan measured
 * ~3.5k T-states against ~470T for the assembly walk in bitmap_ops.s - see
 * that file's header for the numbers.  Four bytes are also the same memory.
 */
static uint8_t g_used_pages[(ZETA_RAM_PAGES + 7u) / 8u];

/* REMOVED 2026-10-02: the six staging words below.
 *
 * They were the inputs of muzix_zeta_copy_window3() - the "copy to a user page
 * while window 3 is hidden" path - which had no caller anywhere in the tree.  The
 * path was replaced (context_switch.c:224 says so where the replacement lives)
 * and the storage outlived it, which is the worst way for dead code to sit: it
 * is a public-looking set of globals in a .c file, so it reads as a live
 * handshake between two modules.
 */

static void zeta_memory_mark(uint8_t page)
{
    uint16_t index;

    if (page >= ZETA_RAM_PAGE_FIRST && page <= ZETA_RAM_PAGE_LAST) {
        index = (uint16_t)(page - ZETA_RAM_PAGE_FIRST);
        g_used_pages[index >> 3] |= (uint8_t)(1u << (index & 0x07u));
    }
}

void zeta_memory_init(const process_map_t *kernel_map)
{
    uint8_t region;

    memset(g_used_pages, 0, sizeof(g_used_pages));
    if (!kernel_map) {
        return;
    }
    for (region = 0; region < 4; region++) {
        zeta_memory_mark(kernel_map->pages[region]);
    }
    /* Page 0x3f is the non-allocatable copy scratch page. */
    zeta_memory_mark(ZETA_RAM_PAGE_LAST);
}

int zeta_memory_alloc_page(uint8_t *page)
{
    int16_t index;

    if (!page) {
        return -1;
    }
    /* One call: scan, set and report.  bitmap_ops.s holds the walk; the
     * 32-bit mask loop this replaces is measured at ~3.5k T-states against
     * ~470T over the same four bytes. */
    index = muzix_bitmap_alloc(g_used_pages, (uint16_t)sizeof(g_used_pages));
    if (index < 0) {
        return -1;
    }
    *page = (uint8_t)(ZETA_RAM_PAGE_FIRST + index);
    return 0;
}

void zeta_memory_release_page(uint8_t page)
{
    if (page >= ZETA_RAM_PAGE_FIRST && page <= ZETA_RAM_PAGE_LAST) {
        muzix_bitmap_release(g_used_pages,
                             (uint16_t)(page - ZETA_RAM_PAGE_FIRST));
    }
}

void zeta_init(zeta_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->bank_reg[0] = MUZIX_ZETA_KERNEL_BANK_0;
    state->bank_reg[1] = MUZIX_ZETA_KERNEL_BANK_1;
    state->bank_reg[2] = MUZIX_ZETA_KERNEL_BANK_2;
    state->bank_reg[3] = MUZIX_ZETA_KERNEL_BANK_3;
    state->paging_enabled = true;
    z80_outb(MUZIX_ZETA_BANK_PORT_0, state->bank_reg[0]);
    z80_outb(MUZIX_ZETA_BANK_PORT_1, state->bank_reg[1]);
    z80_outb(MUZIX_ZETA_BANK_PORT_2, state->bank_reg[2]);
    z80_outb(MUZIX_ZETA_BANK_PORT_3, state->bank_reg[3]);
}

void zeta_enable_paging(zeta_state_t *state, bool enable)
{
    if (state) {
        state->paging_enabled = enable;
    }
}

void zeta_refresh_visible(zeta_state_t *state)
{
    if (!state || !state->paging_enabled) {
        return;
    }
    z80_outb(0x78, state->bank_reg[0]);
    z80_outb(0x79, state->bank_reg[1]);
    z80_outb(0x7a, state->bank_reg[2]);
    z80_outb(0x7b, state->bank_reg[3]);
}

void zeta_set_bank(zeta_state_t *state, int region, uint8_t page)
{
    if (!state || region < 0 || region > 3) {
        return;
    }
    state->bank_reg[region] = page;
    zeta_refresh_visible(state);
}

void zeta_map_kernel(zeta_state_t *state, uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3)
{
    if (!state) {
        return;
    }
    state->bank_reg[0] = p0;
    state->bank_reg[1] = p1;
    state->bank_reg[2] = p2;
    state->bank_reg[3] = p3;
    zeta_refresh_visible(state);
}

void zeta_map_process(zeta_state_t *state, const process_map_t *map)
{
    if (!state || !map) {
        return;
    }
    state->bank_reg[0] = map->pages[0];
    state->bank_reg[1] = map->pages[1];
    state->bank_reg[2] = map->pages[2];
    state->bank_reg[3] = map->pages[3];
    zeta_refresh_visible(state);
}

uint8_t zeta_read_visible(zeta_state_t *state, uint16_t addr)
{
    (void)state;
    return *((volatile uint8_t *)addr);
}

void zeta_write_visible(zeta_state_t *state, uint16_t addr, uint8_t value)
{
    (void)state;
    *((volatile uint8_t *)addr) = value;
}