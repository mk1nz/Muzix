#ifndef MUZIX_RUNTIME_STATE_H
#define MUZIX_RUNTIME_STATE_H

#include <stdint.h>

#include "../mm/mproc_model.h"
#include "../mm/mm_service.h"
#include "proc_table.h"

#define MUZIX_RUNTIME_MAX_PROC MUZIX_PROC_TABLE_MAX

/* A view onto the one process table, not a second copy of it.
 *
 * This used to carry its own slot array, its own `current`, and a whole
 * embedded muzix_scheduler_t used as a bare ready-queue array. That made it a
 * shadow of muzix_kernel_proc_table_t, and the two disagreed: the startup chain
 * picked the first process out of its own queues, while muzix_kernel_loop_yield
 * picked the next one out of proc_table's, so the scheduler only ever had one
 * candidate in it and the context-switch handoff could not fire.
 *
 * It now forwards to proc_table, so "who is runnable" and "who is running" have
 * exactly one answer. `table` is wired up by muzix_kernel_startup_init(), which
 * is the layer that owns both structures. */
typedef struct {
    muzix_kernel_proc_table_t *table;
    process_map_t kernel_map;
} muzix_runtime_state_t;

void muzix_runtime_state_init(muzix_runtime_state_t *rt, const process_map_t *kernel_map);
void muzix_runtime_state_register(muzix_runtime_state_t *rt,
                                 int slot,
                                 uint8_t pid,
                                 const muzix_mproc_t *mp,
                                 const process_map_t *map,
                                 uint8_t q);
void muzix_runtime_state_pick_next(muzix_runtime_state_t *rt, muzix_mm_service_t *mm);
void muzix_runtime_state_switch_to_kernel(muzix_runtime_state_t *rt, muzix_mm_service_t *mm);
int muzix_runtime_state_current_pid(const muzix_runtime_state_t *rt);

#endif
