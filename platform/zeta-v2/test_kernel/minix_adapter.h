#ifndef MUZIX_MINIX_ADAPTER_H
#define MUZIX_MINIX_ADAPTER_H

#include <stdint.h>

#include "bank_model.h"

#define MINIX_SEG_TEXT 0
#define MINIX_SEG_DATA 1
#define MINIX_SEG_STACK 2
#define MINIX_SEG_COUNT 3

typedef struct {
    uint16_t mem_vir;
    uint16_t mem_phys;
    uint16_t mem_len;
} minix_mem_map_t;

typedef struct {
    minix_mem_map_t seg[MINIX_SEG_COUNT];
} minix_proc_map_t;

uint16_t muzix_umap_segment(const minix_proc_map_t *proc,
                            int seg,
                            uint16_t vir_addr,
                            uint16_t bytes);
void muzix_minix_to_process_map(const minix_proc_map_t *proc, process_map_t *out);
void muzix_minix_copy_to_kernel(zeta_state_t *state,
                               const minix_proc_map_t *proc,
                               uint16_t user_addr,
                               uint8_t *dst,
                               size_t len);
void muzix_minix_copy_from_kernel(zeta_state_t *state,
                                 const minix_proc_map_t *proc,
                                 uint16_t user_addr,
                                 const uint8_t *src,
                                 size_t len);
void muzix_minix_sys_copy(zeta_state_t *state,
                         const minix_proc_map_t *src_proc,
                         int src_seg,
                         uint16_t src_vir,
                         const minix_proc_map_t *dst_proc,
                         int dst_seg,
                         uint16_t dst_vir,
                         size_t len);

#endif
