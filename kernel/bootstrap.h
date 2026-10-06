#ifndef MUZIX_BOOTSTRAP_H
#define MUZIX_BOOTSTRAP_H

#include <stdint.h>

#include "runtime_state.h"

/* Queue indices now come from the one process table. These used to be spelled
 * MUZIX_SCHED_TASK_Q / MUZIX_SCHED_USER_Q, from the scheduler.c copy that has
 * been deleted; scheduler.h is gone, so anything still naming MUZIX_SCHED_*
 * no longer compiles. */
#define MUZIX_BOOTSTRAP_TASK_Q MUZIX_PROC_TASK_Q
#define MUZIX_BOOTSTRAP_USER_Q MUZIX_PROC_USER_Q

typedef struct {
    muzix_runtime_state_t runtime;
    uint8_t init_pid;
    uint8_t boot_done;
} muzix_kernel_bootstrap_t;

void muzix_bootstrap_init(muzix_kernel_bootstrap_t *boot,
                          const process_map_t *kernel_map,
                          uint8_t init_pid,
                          const process_map_t *init_map);
void muzix_bootstrap_register_task(muzix_kernel_bootstrap_t *boot,
                                  int slot,
                                  uint8_t pid,
                                  const muzix_mproc_t *mp,
                                  const process_map_t *map,
                                  uint8_t q);
void muzix_bootstrap_run(muzix_kernel_bootstrap_t *boot);
void muzix_bootstrap_switch_to_kernel(muzix_kernel_bootstrap_t *boot);
int muzix_bootstrap_current_pid(const muzix_kernel_bootstrap_t *boot);

#endif
