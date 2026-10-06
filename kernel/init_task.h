#ifndef MUZIX_INIT_TASK_H
#define MUZIX_INIT_TASK_H

#include <stdint.h>

#include "bootstrap.h"

#define MUZIX_INIT_TASK_SLOT 0

typedef struct {
    muzix_kernel_bootstrap_t boot;
    uint8_t init_pid;
    uint8_t init_slot;
    uint8_t running;
} muzix_init_task_t;

void muzix_init_task_init(muzix_init_task_t *it,
                          const process_map_t *kernel_map,
                          uint8_t init_pid,
                          const process_map_t *init_map);
void muzix_init_task_register(muzix_init_task_t *it,
                             int slot,
                             uint8_t pid,
                             const muzix_mproc_t *mp,
                             const process_map_t *map,
                             uint8_t q);
void muzix_init_task_run(muzix_init_task_t *it);
void muzix_init_task_switch_to_kernel(muzix_init_task_t *it);
int muzix_init_task_current_pid(const muzix_init_task_t *it);

#endif
