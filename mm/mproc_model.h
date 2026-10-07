#ifndef MUZIX_MPROC_MODEL_H
#define MUZIX_MPROC_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "../mm/mm_service.h"
#include "../platform/zeta-v2/bank_io.h"

#define MUZIX_SEG_TEXT 0
#define MUZIX_SEG_DATA 1
#define MUZIX_SEG_STACK 2
#define MUZIX_NR_SEGS 3

typedef struct {
    uint16_t mem_vir;
    uint32_t mem_phys;
    uint16_t mem_len;
} muzix_mem_map_t;

typedef struct {
    muzix_mem_map_t mp_seg[MUZIX_NR_SEGS];
    uint8_t mp_pid;
    uint8_t mp_flags;
} muzix_mproc_t;

typedef struct {
    muzix_mproc_t slots[4];
} muzix_proc_table_t;

void muzix_mproc_init(muzix_mproc_t *mp,
                      uint32_t text_phys,
                      uint32_t data_phys,
                      uint32_t stack_phys,
                      uint16_t text_len,
                      uint16_t data_len,
                      uint16_t stack_len,
                      uint8_t pid);
void muzix_mproc_set_seg(muzix_mproc_t *mp,
                         int seg,
                         uint16_t vir,
                         uint8_t page,
                         uint16_t len);

typedef struct {
    uint8_t text_page;
    uint8_t data_page;
    uint8_t stack_page;
} muzix_seg_pages_t;

void muzix_mproc_set_all_segs(muzix_mproc_t *mp, const muzix_seg_pages_t *pages);
int muzix_mproc_range_valid(const muzix_mproc_t *mp,
                            int seg,
                            uint16_t vir_addr,
                            size_t bytes);
uint16_t muzix_mproc_umap(const muzix_mproc_t *mp,
                         int seg,
                         uint16_t vir_addr,
                         uint16_t bytes);
void muzix_mproc_to_zeta_map(const muzix_mproc_t *mp, process_map_t *out);
#endif
