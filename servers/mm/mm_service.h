#ifndef MUZIX_MM_SERVICE_H
#define MUZIX_MM_SERVICE_H

#include <stdint.h>
#include <stddef.h>

#include "../../include/muzix_message.h"

/* Process map: 4 bank pages (one per 16 KB window) */
typedef struct {
    uint8_t pages[4];
} process_map_t;

typedef struct muzix_mm_service {
    uint8_t server_id;
    process_map_t kernel_map;
} muzix_mm_service_t;

void muzix_mm_service_init(muzix_mm_service_t *mm, uint8_t server_id);

int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page);
void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page);
void muzix_mm_memory_init(muzix_mm_service_t *mm, const process_map_t *kernel_map);

int muzix_mm_newmap(muzix_mm_service_t *mm, uint8_t proc_slot, uint16_t map_ptr);

int muzix_mm_copy(muzix_mm_service_t *mm,
                  uint8_t src_proc,
                  uint8_t src_seg,
                  uint16_t src_vir,
                  uint8_t dst_proc,
                  uint8_t dst_seg,
                  uint16_t dst_vir,
                  uint16_t byte_count);

int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm,
                                  uint8_t dst_proc,
                                  uint16_t dst_vir,
                                  const void *kernel_buf,
                                  uint16_t byte_count);

int muzix_mm_load_map(muzix_mm_service_t *mm,
                      uint8_t proc,
                      const process_map_t *map);

int muzix_mm_load_kernel_map(muzix_mm_service_t *mm,
                             const process_map_t *map);

int muzix_mm_cross_bank_call(muzix_mm_service_t *mm,
                             const process_map_t *target_map,
                             int (*func)(void *),
                             void *arg);

int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t page,
                                  uint16_t offset,
                                  void *dst,
                                  uint16_t len);

int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t src_proc,
                                  uint16_t src_vir,
                                  void *kernel_buf,
                                  uint16_t len);

int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm,
                                      uint8_t page,
                                      uint16_t offset,
                                      void *dst,
                                      uint16_t len);

void muzix_mm_refresh_visible(muzix_mm_service_t *mm);

void *muzix_mm_service_create(void);
void muzix_mm_service_destroy(void *mm);

#endif