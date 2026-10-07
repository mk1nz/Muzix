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
