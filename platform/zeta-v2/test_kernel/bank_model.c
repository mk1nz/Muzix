#include "bank_model.h"

#include <string.h>

static uint8_t zeta_ram_storage[ZETA_RAM_STORAGE_SIZE];

/* Translate a physical page ID into the double's storage.
 *
 * Returns NULL for a page with no storage, so every caller can skip the access
 * instead of indexing past the array.  The previous version returned
 * &state->ram[page * 0x4000], which is only a valid offset into the array for
 * page IDs below 4; the kernel's own pages start at 0x20, so every real
 * mapping read half a megabyte past the end of a 16 KiB array.
 *
 * Page IDs inside the RAM range alias onto the storage pool, because the pool
 * is far smaller than the 32 pages the hardware has.  That is a deliberate
 * reduction in fidelity, not a bounds hole: the index can never leave the
 * array. */
static uint8_t *zeta_page_base(const zeta_state_t *state, uint8_t page)
{
    uint8_t slot;

    if (!state || !state->ram) {
        return 0;
    }
    if (page < ZETA_RAM_PAGE_FIRST || page > ZETA_RAM_PAGE_LAST) {
        return 0; /* ROM page or out of range: no storage here */
    }
    slot = (uint8_t)((page - ZETA_RAM_PAGE_FIRST) % ZETA_STORAGE_PAGES);
    return &state->ram[(uint16_t)slot * ZETA_BANK_SIZE];
}

/* There is no cached copy of the visible window: the address is translated
 * through the bank register on every access, so a 64 KiB memcpy into a 16 KiB
 * buffer - and a rejection of every address above 0x4000 - are both gone. */
void zeta_refresh_visible(zeta_state_t *state)
{
    (void)state;
}

void zeta_init(zeta_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->ram = zeta_ram_storage;
    for (int i = 0; i < (int)ZETA_REGIONS; i++) {
        state->bank_reg[i] = 0;
    }
    zeta_enable_paging(state, true);
}

void zeta_enable_paging(zeta_state_t *state, bool enable)
{
    if (!state) {
        return;
    }
    state->paging_enabled = enable;
    if (enable) {
        zeta_refresh_visible(state);
    }
}

void zeta_set_bank(zeta_state_t *state, int region, uint8_t page)
{
    if (!state || region < 0 || region >= (int)ZETA_REGIONS) {
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

    for (int region = 0; region < (int)ZETA_REGIONS; region++) {
        state->bank_reg[region] = map->pages[region];
    }
    zeta_refresh_visible(state);
}

void zeta_save_kernel_map(zeta_state_t *state, process_map_t *out)
{
    if (!state || !out) {
        return;
    }

    for (int region = 0; region < (int)ZETA_REGIONS; region++) {
        out->pages[region] = state->bank_reg[region];
    }
}

void zeta_restore_map(zeta_state_t *state, const process_map_t *saved)
{
    if (!state || !saved) {
        return;
    }

    zeta_map_process(state, saved);
}

uint8_t zeta_read_visible(zeta_state_t *state, uint16_t addr)
{
    uint8_t *page;

    if (!state || !state->paging_enabled || addr >= ZETA_VISIBLE_SIZE) {
        return 0;
    }
    /* addr is 16 bits, so addr >> 14 is always 0..3: every window is
     * reachable, which is what the old ZETA_VISIBLE_SIZE == 0x4000 test
     * wrongly prevented. */
    page = zeta_page_base(state, state->bank_reg[addr >> 14]);
    if (!page) {
        return 0;
    }
    return page[addr & (ZETA_BANK_SIZE - 1)];
}

void zeta_write_visible(zeta_state_t *state, uint16_t addr, uint8_t value)
{
    uint8_t *page;

    if (!state || !state->paging_enabled || addr >= ZETA_VISIBLE_SIZE) {
        return;
    }
    page = zeta_page_base(state, state->bank_reg[addr >> 14]);
    if (!page) {
        return;
    }
    page[addr & (ZETA_BANK_SIZE - 1)] = value;
}

/* True when [user_addr, user_addr+len) stays inside one 16 KiB window. */
static int copy_fits(uint16_t user_addr, size_t len)
{
    return len <= (size_t)(ZETA_BANK_SIZE - (user_addr & (ZETA_BANK_SIZE - 1)));
}

/* Both of these returned NOTHING on either path - a bare `return;` on the guard
 * and a fall off the end on the copy - while mm/mm_copy.c does
 * `return zeta_copy_user_to_kernel(...)`, and kernel/test_syscalls.c counts a
 * copy as succeeding when the result is 0.  So the caller was reading whatever
 * happened to be in the return register.  On the Z80 that is HL, and on the
 * build the values below were meant to be correct; what makes it visible is that
 * clang refuses the file outright, which is why bank_model.c never reached the
 * host pool and why platform/zeta-v2/test_kernel/context_switch.c would not
 * link there for want of these two symbols.
 *
 * -1 on refusal and 0 on success is the convention the rest of this file already
 * uses - see zeta_copy_page_to_kernel() below. */
int zeta_copy_user_to_kernel(zeta_state_t *state,
                             const process_map_t *src_map,
                             uint16_t user_addr,
                             uint8_t *dst,
                             size_t len)
{
    process_map_t saved;

    if (!state || !src_map || !dst || !copy_fits(user_addr, len)) {
        return -1;
    }
    zeta_save_kernel_map(state, &saved);
    zeta_map_process(state, src_map);

    for (size_t i = 0; i < len; i++) {
        dst[i] = zeta_read_visible(state, (uint16_t)(user_addr + i));
    }

    zeta_restore_map(state, &saved);
    return 0;
}

int zeta_copy_kernel_to_user(zeta_state_t *state,
                             const process_map_t *dst_map,
                             uint16_t user_addr,
                             const uint8_t *src,
                             size_t len)
{
    process_map_t saved;

    if (!state || !dst_map || !src || !copy_fits(user_addr, len)) {
        return -1;
    }
    zeta_save_kernel_map(state, &saved);
    zeta_map_process(state, dst_map);

    for (size_t i = 0; i < len; i++) {
        zeta_write_visible(state, (uint16_t)(user_addr + i), src[i]);
    }

    zeta_restore_map(state, &saved);
    return 0;
}

int zeta_copy_page_to_kernel(zeta_state_t *state,
                             uint8_t region,
                             uint8_t page,
                             uint16_t offset,
                             uint8_t *dst,
                             size_t len)
{
    uint8_t saved_page;

    if (!state || !dst || region >= ZETA_REGIONS) {
        return -1;
    }
    if ((uint32_t)offset + (uint32_t)len > (uint32_t)ZETA_BANK_SIZE) {
        return -1;
    }
    if (!zeta_page_base(state, page)) {
        return -1;
    }

    saved_page = state->bank_reg[region];
    state->bank_reg[region] = page;
    for (size_t i = 0; i < len; i++) {
        dst[i] = zeta_read_visible(state,
                                   (uint16_t)(region * ZETA_BANK_SIZE + offset + i));
    }
    state->bank_reg[region] = saved_page;
    return 0;
}
