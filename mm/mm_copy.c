#include "mm_copy.h"
#include "../kernel/kernel_loop.h"
#include "../kernel/proc_table.h"
#include "../platform/zeta-v2/context_switch.h"

/*
 * The kernel's live loop, published by kernel_main.c as `muzix_kernel_loop_t
 * *g_loop_ptr = &g_loop;`.  kernel/mm_service.c reaches the same symbol with
 * the same extern for its slot lookup, and the two must agree: a private
 * pointer to a copy of the table would hand out maps for processes that are
 * not running.
 */
extern muzix_kernel_loop_t *g_loop_ptr;
extern zeta_state_t g_zeta_state;


int muzix_mm_copy_maps(muzix_mm_service_t *mm,
                       const muzix_mproc_t *src_mp,
                       int src_seg,
                       uint16_t src_vir,
                       const muzix_mproc_t *dst_mp,
                       int dst_seg,
                       uint16_t dst_vir,
                       size_t bytes)
{
    uint8_t tmp[256];
    process_map_t src_map = {{0, 0, 0, 0}};
    process_map_t dst_map = {{0, 0, 0, 0}};

    if (!mm || !src_mp || !dst_mp ||
        !muzix_mproc_range_valid(src_mp, src_seg, src_vir, bytes) ||
        !muzix_mproc_range_valid(dst_mp, dst_seg, dst_vir, bytes)) {
        return MUZIX_COPY_EFAULT;
    }

    if (bytes > sizeof(tmp)) {
        return MUZIX_COPY_EFAULT;
    }

    /* Both maps are derived from the mprocs, so the window each virtual address
     * resolves to is the one the owning process actually runs on. Reject the
     * kernel's own windows before touching them: window 0 is pinned to page
     * 0x20 (the boot stub, the syscall vector at 0x0030 and _CODE from 0x0098)
     * and window 3 is the kernel's _DATA and C stack, so a copy aimed at either
     * is a copy against kernel memory rather than the source process's. The
     * primitives refuse both too, but the failure has to be an EFAULT here
     * rather than a silent zero-byte transfer. */
    if ((src_vir >> 14) == 0 || (src_vir >> 14) == 3 ||
        (dst_vir >> 14) == 0 || (dst_vir >> 14) == 3) {
        return MUZIX_COPY_EFAULT;
    }

    muzix_mproc_to_zeta_map(src_mp, &src_map);
    muzix_mproc_to_zeta_map(dst_mp, &dst_map);

    /* The staging copy goes through the MM's window-aware primitives rather
     * than muzix_mm_copy_page_to_kernel()/muzix_mm_copy_kernel_to_user().
     *
     * The MM entry points take a process SLOT, and this used to pass a
     * physical page where a slot is required: src_map.pages[0] is always the
     * kernel page 0x20, and 0x20 >= MUZIX_PROC_TABLE_MAX, so the re-dispatched
     * handler rejected the request outright. SYS_COPY therefore never copied a
     * byte, and src_seg/dst_seg were validated and then never used at all.
     *
     * muzix_mproc_umap() is the check that makes the segment arguments
     * meaningful: it resolves (seg, vir) through the segment table, so an
     * address that belongs to a segment other than the one named is refused
     * instead of being copied through whatever page happened to be installed.
     */
    if (muzix_mproc_umap(src_mp, src_seg, src_vir, (uint16_t)bytes) == 0 ||
        muzix_mproc_umap(dst_mp, dst_seg, dst_vir, (uint16_t)bytes) == 0) {
        return MUZIX_COPY_EFAULT;
    }

    if (zeta_copy_user_to_kernel(&mm->zeta, &src_map, src_vir,
                                 tmp, bytes) != 0) {
        return MUZIX_COPY_EFAULT;
    }

    if (muzix_mm_copy_kernel_to_map(mm, &dst_map, dst_vir, tmp,
                                    (uint16_t)bytes) != 0) {
        return MUZIX_COPY_EFAULT;
    }

    return MUZIX_COPY_OK;
}

/*
 * Map-to-map copy by SLOT NUMBER rather than by mproc pointer.
 *
 * muzix_mm_copy_maps() above is the primitive that actually works - it stages
 * through a kernel buffer, so neither process is asked to be the other's
 * window - but it takes two muzix_mproc_t* derived from segment tables.  A
 * syscall arriving from a server has two SLOT numbers and no segments, because
 * a slot's whole address space is one map.
 *
 * So this resolves each slot to its map and hands both to the same primitive.
 * It exists as its own entry point, and it defines an muzix_mm_* function,
 * which is what makes it a memory-manager module for
 * tools/check_memory_manager.rb - the same rule that refuses kernel/ipc.c.
 * Putting the user-to-user copy here rather than in the kernel is not a
 * concession, it is where the MM's mediation belongs: the copy crosses a bank
 * boundary, so the code that may program the bank registers is the code that
 * performs it.
 */
/*
 * =====================================================================
 * THE STAGING BUFFER IS A STACK LOCAL, AND IT MUST STAY ONE.
 * =====================================================================
 *
 * This used to be a `static uint8_t g_user_copy_staging[256]` in this file's
 * _DATA.  That is wrong, and the reason is a window trap rather than a bug:
 *
 *   _DATA starts at 0xA000, which is Z80 window 2.  The copy primitives
 *   install a TEMPORARY map - zeta_copy_user_to_kernel() maps the source's
 *   page into the window the source address falls in, and
 *   zeta_copy_kernel_to_user() does the same for the destination.  Under
 *   either temp map, window 2 can become a PROCESS page.
 *
 * So a staging buffer in _DATA is only reliably mapped while the kernel map is
 * installed.  The moment the temp map lands on window 2, "kernel memory"
 * resolves to the process's own text page: the two halves of the copy then
 * exchange bytes with a process page, and the write half scribbles through a
 * window that is not the buffer's.
 *
 * The symptom is not a clean failure.  The FS server's own text gets written
 * through, so it resumes at a stack value and the machine executes garbage -
 * "unknown ED opcode: F6 at PC=F6DC" and a screen of one repeated byte, which
 * is what the eyes see when a copy transfers 68 bytes of a constant.
 *
 * A local array instead lands on the kernel's C stack, which is window 3
 * ($C000-$FFFF).  Both primitives refuse region 3 outright - it is the
 * kernel's own window - so no temp map can ever move it.  That is exactly why
 * muzix_mm_copy_maps() has used a 256-byte local since it was written, and
 * this function only broke by not copying that detail.
 */
int muzix_mm_copy_user_to_user(muzix_mm_service_t *mm,
                               uint8_t src_proc,
                               uint16_t src_vir,
                               uint8_t dst_proc,
                               uint16_t dst_vir,
                               uint16_t bytes)
{
    uint8_t staging[256];
    muzix_kernel_proc_table_t *table;
    muzix_proc_slot_t *src_slot;
    muzix_proc_slot_t *dst_slot;

    if (!mm || bytes == 0 || bytes > sizeof(staging)) {
        return -1;
    }
    if (src_proc >= MUZIX_PROC_TABLE_MAX || dst_proc >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }

    table = &g_loop_ptr->system.startup.proc_table;
    src_slot = &table->slots[src_proc];
    dst_slot = &table->slots[dst_proc];

    if (!src_slot->active || !dst_slot->active) {
        return -1;
    }

    /*
     * Resolve through slot->map - the map the scheduler actually installs -
     * and NOT through muzix_mproc_to_zeta_map().  exec_loader USED to set the
     * mproc segments to a single page - TEXT, DATA and STACK all pointing at
     * the text page under the comment "Stack is in the same page as
     * text/data (single-page userspace model)" - while the map the machine
     * runs on carries a separate stack page in window 1 and the text in
     * window 2 (the boot trace's banks `20 2B 2A 23`).
     * muzix_mproc_to_zeta_map() turned that stale single page into both
     * windows, so a copy aimed at the client's stack read the TEXT page
     * instead: plausible addresses, right length, wrong bytes.  exec_loader
     * now stamps the STACK segment with the real stack page, but slot->map
     * stays the authority: it is the map the context switcher installs.
     *
     * kernel/mm_service.c resolves from slot->map for this reason.
     *
     * THE STATE PASSED IS THE SCHEDULER'S, NOT mm->zeta.
     *
     * map_call_impl() restores the map it saved through `state->bank_reg`, not
     * through the live bank registers the syscall trap writes directly.  So the
     * state handed to the primitives must be the one the context switcher
     * keeps in step - g_zeta_state, which write_all_banks() updates - or the
     * copy restores whatever that structure happened to hold when it was
     * initialised.  muzix_mm_service_init() calls zeta_init(), which leaves
     * bank_reg as the KERNEL map; mm->zeta is therefore guaranteed stale for a
     * copy issued on behalf of a running server, and the map left installed
     * after the copy is the kernel's, not the server's.
     *
     * context_switch.c says as much at the window-3 check: "it is the current
     * map, not state->bank_reg[], which the syscall trap does not update - it
     * writes the ports directly."  The primitives already read muzix_live_banks
     * for the windows they keep; it is the restore that is left holding the
     * stale copy.
     */
    if (zeta_copy_user_to_kernel(&g_zeta_state, &src_slot->map, src_vir,
                                 staging, bytes) != 0) {
        return -1;
    }
    if (zeta_copy_kernel_to_user(&g_zeta_state, &dst_slot->map, dst_vir,
                                 staging, bytes) != 0) {
        return -1;
    }
    return 0;
}
int muzix_mm_copy_kernel_to_map(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                const void *buf,
                                uint16_t len)
{
    /* &mm->zeta is never NULL even when mm is, so the primitive's own state
     * check cannot see a bad mm; only this can. The map and the buffer are the
     * primitive's to check - it refuses both by pointer. */
    if (!mm) {
        return -1;
    }
    /* The scheduler's state, not mm->zeta.  muzix_mm_copy_user_to_user() above
     * spells out why: zeta_init() leaves mm->zeta->bank_reg holding the kernel
     * map, the primitives restore the map they saved through state->bank_reg,
     * and a copy issued on behalf of a running process would restore the
     * kernel's map into the middle of that process.  These two entries had kept
     * the stale spelling, so every delivery the routed-syscall path made - the
     * kernel's message to a server's receive buffer - ended with the wrong map
     * installed and the wrong bytes through the window. */
    return zeta_copy_kernel_to_user(&g_zeta_state, map, vir,
                                    (const uint8_t *)buf, (size_t)len);
}

int muzix_mm_copy_map_to_kernel(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                void *buf,
                                uint16_t len)
{
    /* The same shape as muzix_mm_copy_kernel_to_map() and for the same reasons:
     * the primitive's state check cannot see a NULL mm, the map and the buffer
     * are the primitive's to refuse, and both of its kernel-window refusals
     * (region 0 is the kernel's own page, region 3 is _DATA and the C stack)
     * still apply to a caller that asks for a user's stack. */
    if (!mm) {
        return -1;
    }
    return zeta_copy_user_to_kernel(&g_zeta_state, map, vir, (uint8_t *)buf,
                                    (size_t)len);
}

int muzix_mm_handle_copy_kernel_to_user(muzix_system_service_t *svc,
                                        const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_kernel_proc_table_t *table;
    muzix_mm_service_t *mm;
    process_map_t dst_map;
    uint8_t region;

    if (!msg || msg->dst_proc >= MUZIX_PROC_TABLE_MAX || msg->byte_count == 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    if (!loop) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    table = &loop->system.startup.proc_table;
    if (!table->slots[msg->dst_proc].active) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    mm = loop->mm;
    if (!mm || !msg->mem_ptr) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    /* Use the slot's process map, which is what muzix_zeta_enter_userspace()
     * and the context switcher install.  Deriving a map from mproc instead
     * maps TEXT to window 0, which is the kernel's window on this platform and
     * can never be the destination. */
    dst_map = table->slots[msg->dst_proc].map;
    region = (uint8_t)(msg->dst_vir >> 14);

    /* zeta_copy_kernel_to_user() stages through a 256-byte buffer and silently
     * drops anything larger, so reject oversized and window-crossing requests
     * here where we can still report the failure.
     *
     * Region 0 is rejected here as well as in the primitive: window 0 is the
     * kernel's own page (the boot stub, the syscall vector at 0x0030 and _CODE
     * from 0x0098) and write_all_banks() pins it there regardless of the map,
     * so dst_vir below 0x4000 would land in kernel code. */
    if (msg->byte_count > MUZIX_ZETA_COPY_STAGING ||
        region == 0 || region > 3 ||
        msg->byte_count > (uint16_t)(0x4000u - (msg->dst_vir & 0x3FFFu))) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    /* Propagate the primitive's result. It returns int now, and ignoring it is
     * how muzix_exec_copy_argv came to "succeed": the argv write targets window
     * 3, the primitive refuses it, the refusal was discarded, and exec reported
     * success with a user stack that had never been written. */
    if (muzix_mm_copy_kernel_to_map(mm, &dst_map, msg->dst_vir,
                                    (const void *)(uintptr_t)msg->mem_ptr,
                                    msg->byte_count) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_mm_handle_copy_page_to_kernel(muzix_system_service_t *svc,
                                        const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_mm_service_t *mm;
    uint8_t region, page;
    uint16_t offset;
    void *dst;
    uint16_t len;

    if (!loop || !msg) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    mm = loop->mm;
    if (!mm) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    region = (uint8_t)msg->proc1;
    page = (uint8_t)msg->proc2;
    offset = msg->src_vir;
    dst = (void *)(uintptr_t)msg->dst_vir;
    len = msg->byte_count;

    /* Three checks the two siblings make and this one did not, and the third
     * is not the belt to the second's braces:
     *
     * page > 0x3F is refused here rather than by copy_temp_map(), which
     * returns without filling the caller's map and leaves the primitive
     * installing an uninitialised one.
     *
     * len > 0x4000 - (offset & 0x3FFF) is the window-crossing test the
     * kernel_to_user handler makes against its own in-window address: offset
     * must stay the offset of a length that ends inside the one 16 KiB window,
     * or the copy runs off the end of the mapped page into whatever the next
     * window of the temp map holds. */
    if (!dst || region > 3 || page > 0x3F || len > 0x4000 ||
        len > (uint16_t)(0x4000u - (offset & 0x3FFFu))) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    /* Direct call for the same reason as load_map: the muzix_mm_* wrapper
     * re-dispatches MUZIX_SVC_COPY_PAGE_TO_KERNEL back into this handler. */
    if (zeta_copy_page_to_kernel(&mm->zeta, region, page, offset,
                                 (uint8_t *)dst, (size_t)len) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    return MUZIX_SYS_SERVICE_OK;
}

int muzix_mm_handle_copy_user_to_kernel(muzix_system_service_t *svc,
                                        const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_mm_service_t *mm;
    muzix_kernel_proc_table_t *table;
    muzix_proc_slot_t *slot;
    uint8_t src_proc;
    uint16_t src_vir;
    void *kernel_buf;
    uint16_t len;
    process_map_t src_map;

    if (!loop || !msg) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    mm = loop->mm;
    table = &loop->system.startup.proc_table;
    src_proc = msg->proc1;
    src_vir = msg->src_vir;
    kernel_buf = (void *)(uintptr_t)msg->dst_vir;
    len = msg->byte_count;

    if (!kernel_buf || src_proc >= MUZIX_PROC_TABLE_MAX) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    slot = &table->slots[src_proc];
    if (!slot->active) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    muzix_mproc_to_zeta_map(&slot->mproc, &src_map);

    /* Call the primitive directly. This had two defects:
     *  - it called muzix_mm_copy_user_to_kernel(), which rebuilds a message
     *    with the same m_type and re-enters this handler, so the first hop
     *    recursed once before failing;
     *  - it passed src_map.pages[0] where a SLOT index is required. After the
     *    window-permutation fix pages[0] is always the kernel page 0x20, and
     *    0x20 >= MUZIX_PROC_TABLE_MAX, so the re-dispatched handler rejected
     *    it. SYS_READ and SYS_WRITE, the only correctly MM-mediated user copies
     *    in the syscall layer, could therefore never succeed.
     * The map is already resolved here, so pass it straight through. */
    if (zeta_copy_user_to_kernel(&mm->zeta, &src_map, src_vir,
                                 (uint8_t *)kernel_buf, (size_t)len) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    return MUZIX_SYS_SERVICE_OK;
}
