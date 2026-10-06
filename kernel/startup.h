#ifndef MUZIX_STARTUP_H
#define MUZIX_STARTUP_H

#include <stdint.h>

#include "init_task.h"
#include "proc_table.h"

#define MUZIX_STARTUP_INIT_SLOT 0

typedef struct {
    muzix_init_task_t init_task;
    muzix_kernel_proc_table_t proc_table;
    uint8_t booted;
    int current_pid;
} muzix_kernel_startup_t;

void muzix_kernel_startup_init(muzix_kernel_startup_t *ks,
                              const process_map_t *kernel_map,
                              uint8_t init_pid,
                              const process_map_t *init_map);
void muzix_kernel_startup_register(muzix_kernel_startup_t *ks,
                                  int slot,
                                  uint8_t pid,
                                  const muzix_mproc_t *mp,
                                  const process_map_t *map,
                                  uint8_t q,
                                  uint8_t flags);
void muzix_kernel_startup_boot(muzix_kernel_startup_t *ks);
void muzix_kernel_startup_switch_to_kernel(muzix_kernel_startup_t *ks);
int muzix_kernel_startup_current_pid(const muzix_kernel_startup_t *ks);

#endif
