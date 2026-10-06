#ifndef MUZIX_KERNEL_LOOP_H
#define MUZIX_KERNEL_LOOP_H

#include <stdint.h>

#include "system_entry.h"
#include "../platform/zeta-v2/test_kernel/bank_model.h"

struct muzix_mm_service;

struct muzix_zeta_syscall_runtime;
struct muzix_fs_service;

#define MUZIX_KERNEL_LOOP_IDLE 0
#define MUZIX_KERNEL_LOOP_RUNNING 1
#define MUZIX_KERNEL_LOOP_SHUTDOWN 2

typedef struct muzix_kernel_loop {
    muzix_system_entry_t system;
    uint8_t state;
    uint32_t tick;
    int current_pid;
    struct muzix_mm_service *mm;
} muzix_kernel_loop_t;

void muzix_kernel_loop_init(muzix_kernel_loop_t *loop,
                           const process_map_t *kernel_map,
                           uint8_t init_pid,
                           const process_map_t *init_map);
void muzix_kernel_loop_register(muzix_kernel_loop_t *loop,
                               int slot,
                               uint8_t pid,
                               const muzix_mproc_t *mp,
                               const process_map_t *map,
                               uint8_t q,
                               uint8_t flags);
void muzix_kernel_loop_run(muzix_kernel_loop_t *loop);

/* Give up the processor and never come back: wake whatever is due, stop the CPU
 * if nothing is runnable, and switch to whatever is.
 *
 * The point of it being unreturning is that a caller cannot do anything sensible
 * afterwards - it has just made itself unrunnable, and returning to it would run
 * the process once more and then strand it.  This is FUZIX's plt_switchout. */
void muzix_kernel_switchout(muzix_kernel_loop_t *loop);
void muzix_kernel_loop_yield(muzix_kernel_loop_t *loop);
void muzix_kernel_loop_switch_to_kernel(muzix_kernel_loop_t *loop);
int muzix_kernel_loop_current_pid(const muzix_kernel_loop_t *loop);
void muzix_kernel_loop_bind_syscalls(
    muzix_kernel_loop_t *loop,
    struct muzix_zeta_syscall_runtime *runtime,
    struct muzix_fs_service *fs);

#endif
