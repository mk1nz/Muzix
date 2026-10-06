#ifndef MUZIX_ZETA_CONTEXT_SWITCH_H
#define MUZIX_ZETA_CONTEXT_SWITCH_H

#include <stdint.h>
#include "test_kernel/bank_model.h"

/* Opaque saved context for kernel entry/exit */
typedef struct {
    uint8_t pages[4];
} zeta_saved_context_t;

/*
 * WINDOW CONTRACT FOR EVERY MAP-INSTALLING ENTRY POINT BELOW
 * -------------------------------------------------------
 * Window 0 ($0000-$3FFF) is the only window that always maps kernel page 0x20,
 * so it holds the boot stub, the installed syscall vector at 0x0030 and the
 * start of _CODE.  Windows 1-3 are handed to processes.
 *
 * A map install rewrites ports $79-$7B and then returns, and the return
 * address is fetched from whichever window the caller was executing in.  A
 * caller that lives above $3FFF therefore resumes in the page it just mapped.
 * That was true of kernel/system_service.c's load-map handler, which is linked
 * at $75xx, and it worked only because the map it installed happened to keep a
 * usable page in window 1.
 *
 * Rules for callers:
 *   - Call these only from code linked below $3FFF.  Each public entry point
 *     verifies this itself, through muzix_zeta_caller_in_window0(), in its own
 *     stack frame, and refuses the switch instead of corrupting the caller.
 *     A refusal leaves the previous map installed and traces "zeta: win0 map".
 *   - The check samples one address, so a caller whose body straddles $3FFF can
 *     still be caught mid-window.  Keep these entry points out of anything
 *     that does.
 *   - Interrupts must already be disabled.  These functions do not change IFF:
 *     the old DI/EI wrapper could not save IFF (DI clears IFF2, so nothing was
 *     left to restore) and its unconditional EI silently ended the caller's
 *     critical section.
 */

/* Switch from current context (kernel or user) to target process map.
 * Saves current bank registers to 'saved', loads target map.
 * Must be called with interrupts disabled. */

/* Restore previously saved context (for syscall/interrupt return).
 * Must be called with interrupts disabled. */

/* Enter kernel mode: save current user map, load kernel map.
 * Returns saved user context for later restore.
 * Must be called with interrupts disabled. */
void zeta_kernel_enter(zeta_state_t *state,
                       zeta_saved_context_t *saved_user_context);

/* Exit kernel mode: restore user context.
 * Must be called with interrupts disabled. */
void zeta_enter_process(zeta_state_t *state,
                        const process_map_t *target_map);
void zeta_kernel_exit(zeta_state_t *state,
                      const zeta_saved_context_t *saved_user_context);

/* Get the fixed kernel process map (pages 0x20, 0x21, 0x22, 0x23) */

/* Load a process map directly (for kernel map loading).
 * Must be called from window 0 with interrupts disabled.
 * Returns 0 on success, -1 if the switch was refused (wrong window, or a NULL
 * argument) - callers must not report success unconditionally. */
int zeta_load_map(zeta_state_t *state, const process_map_t *map);


/* Cross-bank call: atomically switch to target_map, call func(arg), restore.
 * Returns func's return value, or -1 if the switch was refused.
 *
 * A map whose pages[3] differs from the installed one is refused: window 3
 * holds the kernel stack, every automatic variable and every static in _DATA,
 * so switching it hides the return address of the running code.  This makes
 * the primitive unusable for maps that own window 3 - which is every process
 * map - and a real cross-bank call needs a mechanism that does not run kernel C
 * under a foreign window 3. */
/* Which of the internal temp-map callbacks to run.  An identity rather than a
 * pointer: see map_call_impl in context_switch.c. */
typedef enum {
    ZETA_CB_NONE = 0,
    ZETA_CB_COPY_PAGE_TO_KERNEL,
    ZETA_CB_COPY_USER_TO_KERNEL,
    ZETA_CB_COPY_KERNEL_TO_USER,
    ZETA_CB_COPY_ROM_TO_KERNEL
} zeta_callback_id_t;

int zeta_cross_bank_call_impl(zeta_state_t *state,
                         const process_map_t *target_map,
                         zeta_callback_id_t cb,
                         void *arg);

/* Execute with temporary bank mapping: save current, apply temp, call func,
 * restore.  'temp' is a complete process_map_t with the desired temporary
 * mapping.  Same window-3 refusal as zeta_cross_bank_call_impl(). */
int zeta_with_temp_map_impl(zeta_state_t *state,
                        const process_map_t *temp_map,
                        zeta_callback_id_t cb,
                        void *arg);

/* Resolve a function pointer to one of the callbacks above.  Messages that
 * carry a callback do so as an address; this turns it back into an identity so
 * that nothing on the path ever calls through a pointer.  Returns
 * ZETA_CB_NONE for an address that is not one of the known callbacks. */
zeta_callback_id_t zeta_callback_from_fn(int (*func)(void *));

/* Reset the temp-map nesting depth.  Idempotent, and safe to call with
 * operations still in flight: a leaked slot (a callback that never returned)
 * is reclaimed rather than wedging every later call. */
void zeta_context_stack_init(void);

/* Helper: create temp map with single region swapped (for copy operations).
 * Does nothing if any argument is NULL or region > 3. */

/* Copy operations using callback mechanism (execute in region 0).
 *
 * These run the copy from a C callback, so the kernel stack, the caller's
 * frame and the staging buffer must all stay reachable: none of them may run
 * with window 3 mapped to a process page.  Region-3 requests are therefore
 * refused and traced; callers that need them (userspace's stack is at 0xFFF0,
 * so it always is region 3) have to use a primitive that does not return while
 * the kernel stack is hidden. */

/* zeta_copy_kernel_to_user() and zeta_copy_user_to_kernel() stage through a
 * buffer of this size and silently drop anything larger, so callers that
 * cannot report a failure must reject oversized requests themselves. */
#define MUZIX_ZETA_COPY_STAGING 256u

int zeta_copy_page_to_kernel(zeta_state_t *state,
                              uint8_t region,
                              uint8_t page,
                              uint16_t offset,
                              uint8_t *dst,
                              size_t len);

/* Read a read-only bank page (ROM) into a kernel buffer.
 *
 * This is how the MM reads ROMFS blocks. It replaces an ad-hoc
 * mm/mm_service.c: the MM did its own single-region bank swap with a hardcoded
 * window, which is exactly the manual bank switching AGENTS.md forbids. Here the
 * swap goes through the same temp-map machinery as every other copy, so the
 * complete installed map is saved and restored and window 3 stays the kernel's.
 *
 * `page` is a physical page ID (0x00-0x1F is ROM), not a process page.
 * `offset` and `len` are bounded to one window. Returns 0 or -1. */
int zeta_copy_rom_page_to_kernel(zeta_state_t *state,
                                 uint8_t page,
                                 uint16_t offset,
                                 uint8_t *dst,
                                 size_t len);

int zeta_copy_kernel_to_user(zeta_state_t *state,
                              const process_map_t *dst_map,
                              uint16_t user_addr,
                              const uint8_t *src,
                              size_t len);

int zeta_copy_user_to_kernel(zeta_state_t *state,
                              const process_map_t *src_map,
                              uint16_t user_addr,
                              uint8_t *dst,
                              size_t len);

/* Kernel map for direct access (for temp map creation) */
extern const process_map_t g_kernel_map;

/* Single-region atomic swap for ROM reads (region 1, 2, or 3) */

#endif