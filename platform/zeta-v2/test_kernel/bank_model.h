#ifndef MUZIX_BANK_MODEL_H
#define MUZIX_BANK_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Small prototype sizing for Z80/SDCC. The logic stays the same as the real
 * Zeta V2 16 KiB banking model, but the test memory footprint is reduced so it
 * remains buildable under the target cross-toolchain.
 *
 * Note what the reduction costs.  The hardware has 32 RAM pages of 16 KiB; a
 * double that kept all of them, plus a 64 KiB cache of the visible window,
 * would need 576 KiB of .bsd and could not be linked into a 64 KiB Z80 image
 * at all.  So this model keeps a small pool of physical pages and derives the
 * visible window from the bank registers on every access, the way
 * kernel_bank_model.c does on real hardware - there is no cached copy of the
 * 64 KiB window to keep in sync, and therefore none to overflow.
 */
#define ZETA_BANK_SIZE        0x4000u
#define ZETA_RAM_PAGE_FIRST  0x20u
#define ZETA_RAM_PAGE_LAST   0x3fu
#define ZETA_RAM_PAGES        32u

/* Pages the double actually has storage for.  A page ID outside the RAM range
 * is unmapped; one inside it aliases onto this pool.  Two is what fits
 * alongside code in a 64 KiB image. */
#define ZETA_STORAGE_PAGES   2u
#define ZETA_RAM_STORAGE_SIZE (ZETA_STORAGE_PAGES * ZETA_BANK_SIZE)

/* The full 64 KiB address space, not just window 0.  Reading or writing an
 * address is translated through the bank register for the window it falls in,
 * so regions 1-3 are as reachable as region 0. */
#define ZETA_VISIBLE_SIZE     0x10000u

#define ZETA_REGION_0         0
#define ZETA_REGION_1         1
#define ZETA_REGION_2         2
#define ZETA_REGION_3         3

/* Number of 16 KiB windows. */
#define ZETA_REGIONS          4u

typedef struct {
    uint8_t pages[4];
} process_map_t;

typedef struct {
    uint8_t bank_reg[ZETA_REGIONS];
    bool paging_enabled;
    uint8_t *ram;
} zeta_state_t;

void zeta_init(zeta_state_t *state);
void zeta_enable_paging(zeta_state_t *state, bool enable);
void zeta_set_bank(zeta_state_t *state, int region, uint8_t page);
void zeta_map_kernel(zeta_state_t *state, uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3);
void zeta_map_process(zeta_state_t *state, const process_map_t *map);
void zeta_save_kernel_map(zeta_state_t *state, process_map_t *out);
void zeta_restore_map(zeta_state_t *state, const process_map_t *saved);
void zeta_refresh_visible(zeta_state_t *state);
uint8_t zeta_read_visible(zeta_state_t *state, uint16_t addr);
void zeta_write_visible(zeta_state_t *state, uint16_t addr, uint8_t value);
void zeta_memory_init(const process_map_t *kernel_map);
int zeta_memory_alloc_page(uint8_t *page);
void zeta_memory_release_page(uint8_t page);
int zeta_copy_user_to_kernel(zeta_state_t *state,
                             const process_map_t *src_map,
                             uint16_t user_addr,
                             uint8_t *dst,
                             size_t len);
int zeta_copy_kernel_to_user(zeta_state_t *state,
                             const process_map_t *dst_map,
                             uint16_t user_addr,
                             const uint8_t *src,
                             size_t len);
int zeta_copy_page_to_kernel(zeta_state_t *state,
                             uint8_t region,
                             uint8_t page,
                             uint16_t offset,
                             uint8_t *dst,
                             size_t len);

#endif
