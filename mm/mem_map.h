#ifndef MUZIX_MM_MEM_MAP_H
#define MUZIX_MM_MEM_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "../mm/mm_service.h"
#include "../platform/zeta-v2/bank_io.h"

#define MUZIX_SEG_TEXT 0
#define MUZIX_SEG_DATA 1
#define MUZIX_SEG_STACK 2
#define MUZIX_SEG_COUNT 3

typedef struct {
    uint16_t mem_vir;
    uint32_t mem_phys;
    uint16_t mem_len;
} muzix_mem_map_t;

typedef struct {
    muzix_mem_map_t seg[MUZIX_SEG_COUNT];
    uint8_t pid;
} muzix_proc_t;

void muzix_proc_init(muzix_proc_t *proc,
                     uint32_t text_phys,
                     uint32_t data_phys,
                     uint32_t stack_phys,
                     uint16_t text_len,
                     uint16_t data_len,
                     uint16_t stack_len);
void muzix_proc_to_zeta_map(const muzix_proc_t *proc, process_map_t *out);
uint16_t muzix_umap(const muzix_proc_t *proc, int seg, uint16_t vir_addr, uint16_t bytes);
void muzix_copy_in(muzix_mm_service_t *mm,
                  const muzix_proc_t *proc,
                  int seg,
                  uint16_t vir_addr,
                  uint8_t *dst,
                  size_t len);
void muzix_copy_out(muzix_mm_service_t *mm,
                   const muzix_proc_t *proc,
                   int seg,
                   uint16_t vir_addr,
                   const uint8_t *src,
                   size_t len);

#endif
