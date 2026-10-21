/*
 * The kernel's MM service - and the reason this file exists at all.
 *
 * THREE files in this tree are named mm_service.c, and which one the kernel
 * links is decided by vpath order, not by anything visible at the call site:
 * the Makefile's KERNEL_C_MODULES lists the bare stem `mm_service` and the
 * vpath line puts kernel/ ahead of mm/ and servers/mm/, so THIS file is the
 * kernel's implementation and mm/mm_service.c - the message-routed dispatcher
 * - is not in the kernel image at all.  That split is deliberate and it is
 * written down here because the alternative reading (that the kernel links the
 * dispatcher, or that editing mm/mm_service.c changes the kernel) is exactly
 * what the vpath shading invites, and it is wrong.
 *
 * What the kernel actually needs from the MM service is what this file
 * implements for real:
 *
 *   - muzix_mm_alloc_page / muzix_mm_free_page / muzix_mm_memory_init, the
 *     physical page allocator over zeta_memory_*;
 *   - muzix_mm_copy_rom_page_to_kernel, which rom_read.c's block device goes
 *     through (kernel/rom_read.c; AGENTS.md records why the kernel may not
 *     touch a bank register itself);
 *   - muzix_mm_copy_kernel_to_user / muzix_mm_copy_user_to_kernel, which
 *     kernel/syscalls.c's muzix_copy_{user_from,user_to}_current,
 *     muzix_copy_from_proc and the syscall layer's IPC copies all reach -
 *     every one of them a direct call into the platform's zeta_copy_*
 *     primitives with the destination slot's map.
 *
 * The remaining entry points are deliberate no-ops that report refusal:
 * muzix_mm_newmap, muzix_mm_copy, muzix_mm_load_map,
 * muzix_mm_load_kernel_map, muzix_mm_cross_bank_call,
 * muzix_mm_copy_page_to_kernel and muzix_mm_refresh_visible.  They are not
 * unimplemented; they are unrouted, and the difference matters:
 *
 *   - No kernel caller needs them.  The only kernel call sites of the two
 *     map-loading entry points are kernel/runtime_state.c, reached from
 *     kernel/bootstrap.c with mm == NULL, so the guard skips them entirely.
 *   - Routing them would mean linking mm/mm_service.c, whose methods build a
 *     muzix_system_message_t and call muzix_system_task_handle_message().
 *     That is the right shape for a userspace server (servers/mm links its
 *     own copy of the dispatcher and its own MUZIX_SVC_* handlers, in
 *     MM_SERVER_C_MODULES) and the wrong shape for the kernel's live copy
 *     paths: it would insert an unexercised dispatcher between exec_loader,
 *     rom_read and the syscall layer on one side and the platform copy
 *     primitives on the other, on a kernel stack with about 1.2 KiB of
 *     headroom, on the path that loads the shell.
 *   - The kernel's copy of the MUZIX_SVC_* handlers (kernel/system_service.c,
 *     reached only through muzix_system_task_handle_message()) therefore has
 *     no caller in the image and is dead code there.  It is not accidentally
 *     dead: it is kept because the system task is the documented home of
 *     those services, the host tests drive it directly
 *     (kernel/test_system_service.c), and deleting it would delete the
 *     dispatcher the servers are modelled on.  AGENTS.md records it under the
 *     audit's known-gaps section rather than pretending it is wired.
 *
 * If a kernel caller ever does need one of the no-op methods, the answer is
 * to implement it HERE - as the copy primitives above are implemented, as a
 * direct call into platform/zeta-v2/context_switch.c - and not to swap the
 * link over to the dispatcher without re-validating the whole boot path.
 */
#include "../mm/mm_service.h"
#include "../platform/zeta-v2/context_switch.h"
#include "../platform/zeta-v2/test_kernel/bank_model.h"
#include "kernel_loop.h"
#include "../platform/zeta-v2/trace.h"

extern muzix_kernel_loop_t *g_loop_ptr;

void muzix_mm_service_init(muzix_mm_service_t *mm,
                           struct muzix_system_task_t *system_task,
                           uint8_t source)
{
    if (!mm) return;
    mm->system_task = system_task;
    mm->source = source;
    /* Initialize embedded zeta_state_t with kernel bank registers for ROM reads */
    zeta_init(&mm->zeta);
}

int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page)
{
    (void)mm;
    if (!page) return -1;
    return zeta_memory_alloc_page(page);
}

void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page)
{
    (void)mm;
    zeta_memory_release_page(page);
}

void muzix_mm_memory_init(muzix_mm_service_t *mm, const process_map_t *kernel_map)
{
    (void)mm;
    zeta_memory_init(kernel_map);
}

int muzix_mm_newmap(muzix_mm_service_t *mm, uint8_t proc_slot, uint16_t map_ptr)
{
    (void)mm;
    (void)proc_slot;
    (void)map_ptr;
    return -1;
}

int muzix_mm_copy(muzix_mm_service_t *mm,
                  uint8_t src_proc,
                  uint8_t src_seg,
                  uint16_t src_vir,
                  uint8_t dst_proc,
                  uint8_t dst_seg,
                  uint16_t dst_vir,
                  uint16_t byte_count)
{
    (void)mm;
    (void)src_proc;
    (void)src_seg;
    (void)src_vir;
    (void)dst_proc;
    (void)dst_seg;
    (void)dst_vir;
    (void)byte_count;
    return -1;
}

int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm,
                                  uint8_t dst_proc,
                                  uint16_t dst_vir,
                                  const void *kernel_buf,
                                  uint16_t byte_count)
{
    if (!mm || !kernel_buf || byte_count == 0) return -1;

    /* Use the process map of the destination process.  The slot number is a
     * uint8_t and a bad one is an index past the end of the table, which is a
     * read of somebody else's slot - so it is checked here, the way
     * mm/mm_copy.c's own handlers check src_proc/dst_proc before they index
     * slots[]. */
    if (!g_loop_ptr) return -1;
    if (dst_proc >= MUZIX_PROC_TABLE_MAX) return -1;

    muzix_proc_slot_t *slot = &g_loop_ptr->system.startup.proc_table.slots[dst_proc];
    if (!slot->active) return -1;

    return zeta_copy_kernel_to_user(&mm->zeta, &slot->map, dst_vir, kernel_buf, (size_t)byte_count);
}

int muzix_mm_load_map(muzix_mm_service_t *mm,
                      uint8_t proc,
                      const process_map_t *map)
{
    (void)mm;
    (void)proc;
    (void)map;
    return -1;
}

int muzix_mm_load_kernel_map(muzix_mm_service_t *mm,
                             const process_map_t *map)
{
    (void)mm;
    (void)map;
    return -1;
}

int muzix_mm_cross_bank_call(muzix_mm_service_t *mm,
                             const process_map_t *target_map,
                             int (*func)(void *),
                             void *arg)
{
    (void)mm;
    (void)target_map;
    (void)func;
    (void)arg;
    return -1;
}

int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t page,
                                  uint16_t offset,
                                  void *dst,
                                  uint16_t len)
{
    (void)mm;
    (void)page;
    (void)offset;
    (void)dst;
    (void)len;
    return -1;
}

int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm,
                                   uint8_t src_proc,
                                   uint16_t src_vir,
                                   void *kernel_buf,
                                   uint16_t len)
{
    if (!mm || !kernel_buf || len == 0) return -1;

    if (!g_loop_ptr) return -1;

    /* The same bounds check as the kernel-to-user entry above, and for the same
     * reason: src_proc is a uint8_t and an out-of-range one indexes past the
     * table. */
    if (src_proc >= MUZIX_PROC_TABLE_MAX) return -1;

    muzix_proc_slot_t *slot = &g_loop_ptr->system.startup.proc_table.slots[src_proc];
    if (!slot->active) return -1;

    return zeta_copy_user_to_kernel(&mm->zeta, &slot->map, src_vir, kernel_buf, len);
}

int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm,
                                        uint8_t page,
                                        uint16_t offset,
                                        void *dst,
                                        uint16_t len)
{
    /* The inverted order this used to be - `zeta_state_t *state = &mm->zeta;
     * if (!state || !mm)` - tested mm after dereferencing it, so a NULL mm
     * crashed on the line that was supposed to reject it.  &mm->zeta is a
     * constant offset from mm and is never NULL while mm is, which is exactly
     * why the test has to be on mm itself: mm/mm_copy.c's copies spell the same
     * guard out for the same reason. */
    if (!mm) return -1;
    return zeta_copy_rom_page_to_kernel(&mm->zeta, page, offset, (uint8_t *)dst, (size_t)len);
}

void muzix_mm_refresh_visible(muzix_mm_service_t *mm)
{
    (void)mm;
}

void *muzix_mm_service_create(void)
{
    return 0;
}

void muzix_mm_service_destroy(void *mm)
{
    (void)mm;
}