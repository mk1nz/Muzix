#include "mm_copy.h"
#include "../kernel/kernel_loop.h"
#include "../kernel/proc_table.h"

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
    return zeta_copy_kernel_to_user(&mm->zeta, map, vir,
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
    return zeta_copy_user_to_kernel(&mm->zeta, map, vir, (uint8_t *)buf,
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
    region = (uint8_t)msg->proc1;
    page = (uint8_t)msg->proc2;
    offset = msg->src_vir;
    dst = (void *)(uintptr_t)msg->dst_vir;
    len = msg->byte_count;

    if (!dst || region > 3 || len > 0x4000) {
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
