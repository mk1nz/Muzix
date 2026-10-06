#include "mem_map.h"
#include "../mm/mm_service.h"

#include <string.h>

static uint8_t muzix_page_from_phys(uint32_t phys)
{
    return (uint8_t)(phys / ZETA_BANK_SIZE);
}

void muzix_proc_init(muzix_proc_t *proc,
                     uint32_t text_phys,
                     uint32_t data_phys,
                     uint32_t stack_phys,
                     uint16_t text_len,
                     uint16_t data_len,
                     uint16_t stack_len)
{
    if (!proc) {
        return;
    }

    memset(proc, 0, sizeof(*proc));
    proc->seg[MUZIX_SEG_TEXT].mem_vir = 0;
    proc->seg[MUZIX_SEG_TEXT].mem_phys = text_phys;
    proc->seg[MUZIX_SEG_TEXT].mem_len = text_len;

    proc->seg[MUZIX_SEG_DATA].mem_vir = 0;
    proc->seg[MUZIX_SEG_DATA].mem_phys = data_phys;
    proc->seg[MUZIX_SEG_DATA].mem_len = data_len;

    proc->seg[MUZIX_SEG_STACK].mem_vir = 0;
    proc->seg[MUZIX_SEG_STACK].mem_phys = stack_phys;
    proc->seg[MUZIX_SEG_STACK].mem_len = stack_len;
}

void muzix_proc_to_zeta_map(const muzix_proc_t *proc, process_map_t *out)
{
    process_map_t tmp = {{0, 0, 0, 0}};

    if (!proc || !out) {
        return;
    }

    if (proc->seg[MUZIX_SEG_TEXT].mem_len != 0) {
        tmp.pages[0] = muzix_page_from_phys(proc->seg[MUZIX_SEG_TEXT].mem_phys);
    }
    if (proc->seg[MUZIX_SEG_DATA].mem_len != 0) {
        tmp.pages[1] = muzix_page_from_phys(proc->seg[MUZIX_SEG_DATA].mem_phys);
    }
    if (proc->seg[MUZIX_SEG_STACK].mem_len != 0) {
        tmp.pages[2] = muzix_page_from_phys(proc->seg[MUZIX_SEG_STACK].mem_phys);
    }
    tmp.pages[3] = MUZIX_ZETA_KERNEL_BANK_3;

    *out = tmp;
}

uint16_t muzix_umap(const muzix_proc_t *proc, int seg, uint16_t vir_addr, uint16_t bytes)
{
    const muzix_mem_map_t *m;
    uint16_t seg_base;

    if (!proc || seg < 0 || seg >= MUZIX_SEG_COUNT || bytes == 0) {
        return 0;
    }

    m = &proc->seg[seg];
    if ((vir_addr + bytes) > (m->mem_vir + m->mem_len)) {
        return 0;
    }

    seg_base = m->mem_phys;
    return (uint16_t)(seg_base + vir_addr - m->mem_vir);
}

void muzix_copy_in(muzix_mm_service_t *mm,
                  const muzix_proc_t *proc,
                  int seg,
                  uint16_t vir_addr,
                  uint8_t *dst,
                  size_t len)
{
    process_map_t map = {{0, 0, 0, 0}};

    if (!mm || !proc || !dst || len == 0) {
        return;
    }

    muzix_proc_to_zeta_map(proc, &map);
    muzix_mm_copy_user_to_kernel(mm, 0, vir_addr, dst, len);
}

void muzix_copy_out(muzix_mm_service_t *mm,
                   const muzix_proc_t *proc,
                   int seg,
                   uint16_t vir_addr,
                   const uint8_t *src,
                   size_t len)
{
    process_map_t map = {{0, 0, 0, 0}};

    if (!mm || !proc || !src || len == 0) {
        return;
    }

    muzix_proc_to_zeta_map(proc, &map);
    muzix_mm_copy_kernel_to_user(mm, 0, vir_addr, src, len);
}
