#include "test_kernel/bank_model.h"
#include "bank_io.h"
#include "z80_io.h"
#include "context_switch.h"
#include "trace.h"

#include <string.h>

static uint32_t g_used_pages;

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
    if (page >= ZETA_RAM_PAGE_FIRST && page <= ZETA_RAM_PAGE_LAST) {
        g_used_pages |= (uint32_t)1u << (page - ZETA_RAM_PAGE_FIRST);
    }
}

void zeta_memory_init(const process_map_t *kernel_map)
{
    uint8_t region;

    g_used_pages = 0;
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
    uint8_t index;

    if (!page) {
        return -1;
    }
    for (index = 0; index < ZETA_RAM_PAGES; index++) {
        if ((g_used_pages & ((uint32_t)1u << index)) == 0) {
            g_used_pages |= (uint32_t)1u << index;
            *page = (uint8_t)(ZETA_RAM_PAGE_FIRST + index);
            return 0;
        }
    }
    return -1;
}

void zeta_memory_release_page(uint8_t page)
{
    if (page >= ZETA_RAM_PAGE_FIRST && page <= ZETA_RAM_PAGE_LAST) {
        g_used_pages &= ~((uint32_t)1u << (page - ZETA_RAM_PAGE_FIRST));
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