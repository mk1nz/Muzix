#ifndef MUZIX_MM_SERVICE_H
#define MUZIX_MM_SERVICE_H

#include "../platform/zeta-v2/test_kernel/bank_model.h"
#include "../platform/zeta-v2/context_switch.h"

/* Forward declaration to break circular dependency with system_task.h */
struct muzix_system_task_t;

typedef struct muzix_mm_service {
    struct muzix_system_task_t *system_task;
    uint8_t source;
    zeta_state_t zeta;
    process_map_t kernel_map;
} muzix_mm_service_t;

void muzix_mm_service_init(muzix_mm_service_t *mm,
                           struct muzix_system_task_t *system_task,
                           uint8_t source);

/* Physical page allocation */
int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page);
void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page);
void muzix_mm_memory_init(muzix_mm_service_t *mm, const process_map_t *kernel_map);

/* Process memory map management */
int muzix_mm_newmap(muzix_mm_service_t *mm, uint8_t proc_slot, uint16_t map_ptr);

/* Cross-process memory copy (via system task) */
int muzix_mm_copy(muzix_mm_service_t *mm,
                  uint8_t src_proc,
                  uint8_t src_seg,
                  uint16_t src_vir,
                  uint8_t dst_proc,
                  uint8_t dst_seg,
                  uint16_t dst_vir,
                  uint16_t byte_count);

/* Copy from kernel buffer (in common area) to user process */
int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm,
                                  uint8_t dst_proc,
                                  uint16_t dst_vir,
                                  const void *kernel_buf,
                                  uint16_t byte_count);

/* Context switching - switch to a process's bank map */


/* Enter kernel mode - save user context, load kernel map */


/* Exit kernel mode - restore user context */


/* Load a process's memory map into MM service */
int muzix_mm_load_map(muzix_mm_service_t *mm,
                      uint8_t proc,
                      const process_map_t *map);

/* Load kernel memory map into MM service */
int muzix_mm_load_kernel_map(muzix_mm_service_t *mm,
                             const process_map_t *map);

/* Get kernel map for saving/restoring */


/* Temporary bank mapping for cross-bank copy operations */


/* Cross-bank call */
int muzix_mm_cross_bank_call(muzix_mm_service_t *mm,
                             const process_map_t *target_map,
                             int (*func)(void *),
                             void *arg);

/* Copy from physical bank page to kernel buffer */
int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t page,
                                  uint16_t offset,
                                  void *dst,
                                  uint16_t len);

/* Copy from user space to kernel buffer */
int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t src_proc,
                                  uint16_t src_vir,
                                  void *kernel_buf,
                                  uint16_t len);

/* Copy from kernel buffer to user space */
int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm,
                                  uint8_t dst_proc,
                                  uint16_t dst_vir,
                                  const void *kernel_buf,
                                  uint16_t len);

/* Copy from ROM bank page to kernel buffer (for ROMFS reads) */
int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm,
                                      uint8_t page,
                                      uint16_t offset,
                                      void *dst,
                                      uint16_t len);

/* Refresh visible bank mapping */
void muzix_mm_refresh_visible(muzix_mm_service_t *mm);

#endif
