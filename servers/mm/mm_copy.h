#ifndef MUZIX_MM_COPY_H
#define MUZIX_MM_COPY_H

#include <stddef.h>
#include <stdint.h>

#include "mm_service.h"
#include "mproc_model.h"
#include "../../include/muzix_message.h"

#define MUZIX_COPY_OK 0
#define MUZIX_COPY_EFAULT 1

typedef struct {
    uint8_t src_proc;
    uint8_t dst_proc;
    int src_space;
    int dst_space;
    uint16_t src_vir;
    uint16_t dst_vir;
    size_t bytes;
} muzix_copy_request_t;

int muzix_mm_copy_maps(muzix_mm_service_t *mm,
                       const muzix_mproc_t *src_mp,
                       int src_seg,
                       uint16_t src_vir,
                       const muzix_mproc_t *dst_mp,
                       int dst_seg,
                       uint16_t dst_vir,
                       size_t bytes);

int muzix_mm_copy_kernel_to_map(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                const void *buf,
                                uint16_t len);

int muzix_mm_copy_map_to_kernel(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                void *buf,
                                uint16_t len);

#endif