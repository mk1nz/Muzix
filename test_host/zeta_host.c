/*
 * Host stand-ins for the banked-memory primitives in platform/zeta-v2/
 * context_switch.c.
 *
 * WHY THESE EXIST
 * ---------------
 * The copy primitives - zeta_copy_user_to_kernel, zeta_copy_kernel_to_user,
 * zeta_copy_page_to_kernel, zeta_copy_rom_page_to_kernel - are the one place
 * where the C in this tree stops being portable: they are reached through a C
 * callback with the bank registers programmed, which is inline Z80 assembly and
 * has no meaning off the board.  They are the MM's alone (tools/
 * check_memory_manager.rb enforces it), and every module that transitively
 * reaches mm/mm_copy.c - which is most of the kernel - goes undefined without
 * them.  So a host test of, say, the SYS_SETRTC handler could not be linked at
 * all, and no amount of arranging the test would change that: the handler's own
 * copy of the caller's struct is the thing under test, and it goes through these
 * two functions.
 *
 * WHAT IS REAL AND WHAT IS NOT
 * ----------------------------
 * Real: the two user-facing copy primitives move bytes, and they keep the two
 * refusals the real ones make - a request larger than the staging buffer is
 * refused rather than truncated, and windows 0 and 3 are refused because they
 * are the kernel's.  The reason is not fidelity for its own sake: window 3 is
 * where userspace's own stack lives at 0xFFF0, so a fake that allowed it would
 * quietly let a copy into kernel memory that the real driver refuses, and a test
 * would pass on behaviour the target cannot do.
 *
 * Not real: the address space.  There is one flat 64 KB array standing in for
 * every process's four windows, because a banked address space needs banks.  So
 * nothing here tests which page a virtual address resolves to, and nothing here
 * claims to.  What it does test is everything above this file: the syscall
 * handlers, the memory manager's mediation, the drivers - all of which treat the
 * copy primitives as a boundary and must not care what is behind it.
 *
 * Nothing in this file is in KERNEL_C_MODULES, so SDCC never sees it and it
 * cannot reach the ROM.  tools/host_tests.rb links it into every host test
 * binary, which is why a test that reaches the copy primitives needs no
 * arrangement of its own.
 */

#include <stdint.h>
#include <string.h>

#include "../platform/zeta-v2/context_switch.h"

/* The stand-in address space.  64 KB, because a process's four 16 KB windows
 * are 64 KB and the caller passes a window-relative address. */
#define HOST_USER_SPACE_SIZE 0x10000u

static uint8_t g_host_user_space[HOST_USER_SPACE_SIZE];

/* Stand-ins for the other platforms' registers, referenced by the state and the
 * map the primitives are handed.  They are ignored: see the note above. */
zeta_state_t *g_zeta_state_ptr;

void muzix_host_user_space_clear(void);
uint8_t *muzix_host_user_space(void);

void muzix_host_user_space_clear(void)
{
    memset(g_host_user_space, 0, sizeof(g_host_user_space));
}

uint8_t *muzix_host_user_space(void)
{
    return g_host_user_space;
}

/* The refusals the real primitives make before touching anything.  Both return
 * 0 for "refused", as the real ones do, so a caller that ignores the answer
 * still gets no transfer. */
static int host_copy_refused(uint16_t user_addr, size_t len)
{
    if (len > MUZIX_ZETA_COPY_STAGING) {
        return 1;
    }
    /* Window 0 is pinned to the boot stub and _CODE; window 3 is _DATA and the
     * C stack.  See mm/mm_copy.c for why this is checked at both ends. */
    if ((user_addr >> 14) == 0 || (user_addr >> 14) == 3) {
        return 1;
    }
    if ((size_t)user_addr + len > HOST_USER_SPACE_SIZE) {
        return 1;
    }
    return 0;
}

int zeta_copy_user_to_kernel(zeta_state_t *state,
                              const process_map_t *src_map,
                              uint16_t user_addr,
                              uint8_t *dst,
                              size_t len)
{
    (void)state;
    (void)src_map;
    if (dst == 0 || host_copy_refused(user_addr, len)) {
        return -1;
    }
    memcpy(dst, &g_host_user_space[user_addr], len);
    return 0;
}

int zeta_copy_kernel_to_user(zeta_state_t *state,
                              const process_map_t *dst_map,
                              uint16_t user_addr,
                              const uint8_t *src,
                              size_t len)
{
    (void)state;
    (void)dst_map;
    if (src == 0 || host_copy_refused(user_addr, len)) {
        return -1;
    }
    memcpy(&g_host_user_space[user_addr], src, len);
    return 0;
}

/* The other two are not on any path a host test reaches - a page read is the
 * memory manager loading a whole 16 KB page, and a ROM page read is it loading
 * ROMFS - and both are refused rather than faked.  A test that needs one will
 * fail with EFAULT, which says "not covered here" and does not say "worked". */
int zeta_copy_page_to_kernel(zeta_state_t *state,
                              uint8_t region,
                              uint8_t page,
                              uint16_t offset,
                              uint8_t *dst,
                              size_t len)
{
    (void)state; (void)region; (void)page; (void)offset; (void)dst; (void)len;
    return -1;
}

int zeta_copy_rom_page_to_kernel(zeta_state_t *state,
                                 uint8_t page,
                                 uint16_t offset,
                                 uint8_t *dst,
                                 size_t len)
{
    (void)state; (void)page; (void)offset; (void)dst; (void)len;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Reached only through linkage.  None of these is on a path any host test
 * takes, and each is here because some module in the closure references it by
 * name; a stub that returned 0 silently would be worse than one that refuses,
 * so each refuses.
 * --------------------------------------------------------------------------- */

void zeta_kernel_enter(zeta_state_t *state,
                       zeta_saved_context_t *saved_user_context)
{
    (void)state; (void)saved_user_context;
}

void zeta_kernel_exit(zeta_state_t *state,
                      const zeta_saved_context_t *saved_user_context)
{
    (void)state; (void)saved_user_context;
}

int zeta_load_map(zeta_state_t *state, const process_map_t *map)
{
    (void)state; (void)map;
    return -1;
}

int zeta_cross_bank_call_impl(zeta_state_t *state,
                              const process_map_t *target_map,
                              zeta_callback_id_t cb, void *arg)
{
    (void)state; (void)target_map; (void)cb; (void)arg;
    return -1;
}

int zeta_with_temp_map_impl(zeta_state_t *state,
                            const process_map_t *temp_map,
                            zeta_callback_id_t cb, void *arg)
{
    (void)state; (void)temp_map; (void)cb; (void)arg;
    return -1;
}

zeta_callback_id_t zeta_callback_from_fn(int (*func)(void *))
{
    (void)func;
    return ZETA_CB_NONE;
}

void zeta_context_stack_init(void)
{
}

uint8_t zeta_swap_region(zeta_state_t *state, uint8_t region, uint8_t new_page)
{
    (void)state; (void)region; (void)new_page;
    return 0;
}

void zeta_restore_region(zeta_state_t *state, uint8_t region, uint8_t old_page)
{
    (void)state; (void)region; (void)old_page;
}
