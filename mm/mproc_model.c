#include "mproc_model.h"

#include <string.h>

static uint8_t muzix_page_from_phys(uint32_t phys)
{
     return (uint8_t)(phys >> 14);
}

void muzix_mproc_init(muzix_mproc_t *mp,
                      uint32_t text_phys,
                      uint32_t data_phys,
                      uint32_t stack_phys,
                      uint16_t text_len,
                      uint16_t data_len,
                      uint16_t stack_len,
                      uint8_t pid)
{
    if (!mp) {
        return;
    }

    memset(mp, 0, sizeof(*mp));
    mp->mp_pid = pid;
    mp->mp_flags = 1;

    mp->mp_seg[MUZIX_SEG_TEXT].mem_vir = 0;
    mp->mp_seg[MUZIX_SEG_TEXT].mem_phys = text_phys;
    mp->mp_seg[MUZIX_SEG_TEXT].mem_len = text_len;

    mp->mp_seg[MUZIX_SEG_DATA].mem_vir = 0;
    mp->mp_seg[MUZIX_SEG_DATA].mem_phys = data_phys;
    mp->mp_seg[MUZIX_SEG_DATA].mem_len = data_len;

    mp->mp_seg[MUZIX_SEG_STACK].mem_vir = 0;
    mp->mp_seg[MUZIX_SEG_STACK].mem_phys = stack_phys;
    mp->mp_seg[MUZIX_SEG_STACK].mem_len = stack_len;
}

void muzix_mproc_set_seg(muzix_mproc_t *mp,
                         int seg,
                         uint16_t vir,
                         uint8_t page,
                         uint16_t len)
{
    if (!mp || seg < 0 || seg >= MUZIX_NR_SEGS) {
        return;
    }

    mp->mp_seg[seg].mem_vir = vir;
    mp->mp_seg[seg].mem_phys = (uint32_t)page * ZETA_BANK_SIZE;
    mp->mp_seg[seg].mem_len = len;
}

void muzix_mproc_set_all_segs(muzix_mproc_t *mp, const muzix_seg_pages_t *pages)
{
    if (!mp || !pages) {
        return;
    }

    mp->mp_seg[MUZIX_SEG_TEXT].mem_vir = 0;
    mp->mp_seg[MUZIX_SEG_TEXT].mem_phys = (uint32_t)pages->text_page * ZETA_BANK_SIZE;
    mp->mp_seg[MUZIX_SEG_TEXT].mem_len = 0x4000;

    mp->mp_seg[MUZIX_SEG_DATA].mem_vir = 0;
    mp->mp_seg[MUZIX_SEG_DATA].mem_phys = (uint32_t)pages->data_page * ZETA_BANK_SIZE;
    mp->mp_seg[MUZIX_SEG_DATA].mem_len = 0x4000;

    mp->mp_seg[MUZIX_SEG_STACK].mem_vir = 0;
    mp->mp_seg[MUZIX_SEG_STACK].mem_phys = (uint32_t)pages->stack_page * ZETA_BANK_SIZE;
    mp->mp_seg[MUZIX_SEG_STACK].mem_len = 0x4000;
}

int muzix_mproc_range_valid(const muzix_mproc_t *mp,
                            int seg,
                            uint16_t vir_addr,
                            size_t bytes)
{
    const muzix_mem_map_t *m;
    uint16_t offset;

    if (!mp || seg < 0 || seg >= MUZIX_NR_SEGS || bytes == 0) {
        return 0;
    }

    m = &mp->mp_seg[seg];
    if (vir_addr < m->mem_vir) {
        return 0;
    }

    offset = (uint16_t)(vir_addr - m->mem_vir);
    return bytes <= (size_t)m->mem_len && offset <= (uint16_t)(m->mem_len - bytes);
}

uint16_t muzix_mproc_umap(const muzix_mproc_t *mp,
                         int seg,
                         uint16_t vir_addr,
                         uint16_t bytes)
{
    const muzix_mem_map_t *m;
    uint32_t base;

    if (!muzix_mproc_range_valid(mp, seg, vir_addr, bytes)) {
        return 0;
    }

    m = &mp->mp_seg[seg];
    base = m->mem_phys;
    return (uint16_t)(base + vir_addr - m->mem_vir);
}

/* Convert a MINIX-style 3-segment process into the 4-window bank map this
 * platform actually uses.
 *
 * The segment-to-window mapping is NOT the identity. Window 0 ($0000-$3FFF) is
 * always the kernel: it holds the boot stub, the syscall vector installed at
 * 0x0030, and the start of _CODE at 0x0098. Userspace text is linked at
 * 0x8000, which is window 2, and the stack top 0xFFFF is window 3.
 *
 * The previous version mapped TEXT->window 0, DATA->window 1, STACK->window 2
 * and hardcoded window 3 to the kernel. That put a user page where the kernel
 * and the syscall vector live, and left the stack in a text window. Any map
 * built from it and installed with zeta_load_map produced a machine that
 * fetched ROM page 0 instead of the kernel - observed as bank registers
 * 20 06 22 23 / 20 00 00 00 and a jump into the boot stub.
 *
 * A segment with mem_len == 0 falls back to the kernel page rather than 0, so
 * an unset segment can never leave a writable window pointing at ROM. */
void muzix_mproc_to_zeta_map(const muzix_mproc_t *mp, process_map_t *out)
{
    process_map_t tmp = {{MUZIX_ZETA_KERNEL_BANK_0, MUZIX_ZETA_KERNEL_BANK_1,
                         MUZIX_ZETA_KERNEL_BANK_2, MUZIX_ZETA_KERNEL_BANK_3}};

    if (!mp || !out) {
        return;
    }

    if (mp->mp_seg[MUZIX_SEG_STACK].mem_len != 0) {
        tmp.pages[1] = muzix_page_from_phys(mp->mp_seg[MUZIX_SEG_STACK].mem_phys);
    }
    if (mp->mp_seg[MUZIX_SEG_TEXT].mem_len != 0) {
        tmp.pages[2] = muzix_page_from_phys(mp->mp_seg[MUZIX_SEG_TEXT].mem_phys);
    }

    *out = tmp;
}
