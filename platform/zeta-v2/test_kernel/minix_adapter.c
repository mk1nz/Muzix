#include "minix_adapter.h"

#include <string.h>

static uint8_t muzix_segment_page(const minix_mem_map_t *seg, int region)
{
    uint16_t base;
    if (!seg) {
        return 0;
    }

    base = seg->mem_phys + (uint16_t)region;
    return (uint8_t)(base & 0x07u);
}

uint16_t muzix_umap_segment(const minix_proc_map_t *proc,
                            int seg,
                            uint16_t vir_addr,
                            uint16_t bytes)
{
    const minix_mem_map_t *m;
    uint32_t virtual_start;
    uint32_t virtual_end;
    uint32_t request_end;
    uint32_t physical_address;

    if (!proc || seg < 0 || seg >= MINIX_SEG_COUNT || bytes == 0) {
        return 0;
    }

    m = &proc->seg[seg];
    virtual_start = (uint32_t)m->mem_vir << 12;
    virtual_end = (uint32_t)(m->mem_vir + m->mem_len) << 12;
    request_end = (uint32_t)vir_addr + bytes;
    if ((uint32_t)vir_addr < virtual_start || request_end > virtual_end) {
        return 0;
    }

    physical_address = ((uint32_t)m->mem_phys << 12) +
                       ((uint32_t)vir_addr - virtual_start);
    return physical_address > 0xffffu ? 0 : (uint16_t)physical_address;
}

void muzix_minix_to_process_map(const minix_proc_map_t *proc, process_map_t *out)
{
    int i;
    process_map_t tmp = {{0, 0, 0, 0}};

    if (!proc || !out) {
        return;
    }

    for (i = 0; i < MINIX_SEG_COUNT; i++) {
        if (proc->seg[i].mem_len != 0) {
            tmp.pages[i] = muzix_segment_page(&proc->seg[i], 0);
        }
    }

    out->pages[0] = tmp.pages[0];
    out->pages[1] = tmp.pages[1];
    out->pages[2] = tmp.pages[2];
    out->pages[3] = tmp.pages[3];
}

void muzix_minix_copy_to_kernel(zeta_state_t *state,
                               const minix_proc_map_t *proc,
                               uint16_t user_addr,
                               uint8_t *dst,
                               size_t len)
{
    process_map_t map = {{0, 0, 0, 0}};

    if (!state || !proc || !dst) {
        return;
    }

    muzix_minix_to_process_map(proc, &map);
    zeta_copy_user_to_kernel(state, &map, user_addr, dst, len);
}

void muzix_minix_copy_from_kernel(zeta_state_t *state,
                                 const minix_proc_map_t *proc,
                                 uint16_t user_addr,
                                 const uint8_t *src,
                                 size_t len)
{
    process_map_t map = {{0, 0, 0, 0}};

    if (!state || !proc || !src) {
        return;
    }

    muzix_minix_to_process_map(proc, &map);
    zeta_copy_kernel_to_user(state, &map, user_addr, src, len);
}

void muzix_minix_sys_copy(zeta_state_t *state,
                         const minix_proc_map_t *src_proc,
                         int src_seg,
                         uint16_t src_vir,
                         const minix_proc_map_t *dst_proc,
                         int dst_seg,
                         uint16_t dst_vir,
                         size_t len)
{
    uint16_t src_phys;
    uint16_t dst_phys;

    if (!state || !src_proc || !dst_proc || len == 0 ||
        src_seg < 0 || src_seg >= MINIX_SEG_COUNT ||
        dst_seg < 0 || dst_seg >= MINIX_SEG_COUNT) {
        return;
    }

    if (len > 0xffffu ||
        len > (size_t)0xffffu - src_vir ||
        len > (size_t)0xffffu - dst_vir) {
        return;
    }
    src_phys = muzix_umap_segment(src_proc, src_seg, src_vir,
                                  (uint16_t)len);
    dst_phys = muzix_umap_segment(dst_proc, dst_seg, dst_vir,
                                  (uint16_t)len);
    if ((src_phys == 0 && src_proc->seg[src_seg].mem_phys != 0) ||
        (dst_phys == 0 && dst_proc->seg[dst_seg].mem_phys != 0)) {
        return;
    }
    if ((uint32_t)src_phys + len > ZETA_RAM_STORAGE_SIZE ||
        (uint32_t)dst_phys + len > ZETA_RAM_STORAGE_SIZE) {
        return;
    }

    memmove(&state->ram[dst_phys], &state->ram[src_phys], len);
}
