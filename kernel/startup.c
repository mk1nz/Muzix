#include "startup.h"

#include <string.h>

void muzix_kernel_startup_init(muzix_kernel_startup_t *ks,
                               const process_map_t *kernel_map,
                               uint8_t init_pid,
                               const process_map_t *init_map)
{
    if (!ks) {
        return;
    }

    memset(ks, 0, sizeof(*ks));
    muzix_proc_table_init(&ks->proc_table, kernel_map);
    muzix_init_task_init(&ks->init_task, kernel_map, init_pid, init_map);

    /* Point the startup chain at the one process table.
     *
     * muzix_kernel_startup_t holds both proc_table and the init_task chain that
     * used to keep a shadow of it. This is the layer that owns both, so it is
     * where the chain learns which table is the real one - the alternative would
     * be threading the pointer through system_entry_init, startup_init,
     * init_task_init, bootstrap_init and runtime_state_init, five signatures
     * changed to pass a value all five already have a copy of. */
    ks->init_task.boot.runtime.table = &ks->proc_table;

    ks->booted = 0;
    ks->current_pid = -1;
}

void muzix_kernel_startup_register(muzix_kernel_startup_t *ks,
                                   int slot,
                                   uint8_t pid,
                                   const muzix_mproc_t *mp,
                                   const process_map_t *map,
                                   uint8_t q,
                                   uint8_t flags)
{
    if (!ks || !mp || !map) {
        return;
    }

    muzix_proc_table_register(&ks->proc_table, slot, pid, mp, map, q, flags);
    muzix_proc_table_ready(&ks->proc_table, slot);

    /* No muzix_init_task_register() here any more. It reached the same table by
     * a second route - the init_task chain forwards to runtime_state, which
     * forwards to proc_table - so the slot was registered twice. */
}

void muzix_kernel_startup_boot(muzix_kernel_startup_t *ks)
{
    if (!ks) {
        return;
    }

    if (!ks->booted) {
        /* Dequeue from the shared ready queues. This used to store the SLOT
         * index returned by pick_next() into current_pid, which is a pid field,
         * so a booted system reported slot 1 as "pid 1" purely because the two
         * happened to coincide at boot. */
        if (muzix_proc_table_pick_next(&ks->proc_table) >= 0) {
            ks->booted = 1;
        }
    }
}

void muzix_kernel_startup_switch_to_kernel(muzix_kernel_startup_t *ks)
{
    if (!ks) {
        return;
    }

    muzix_proc_table_switch_to_kernel(&ks->proc_table);
    ks->current_pid = -1;
    ks->booted = 0;
}

int muzix_kernel_startup_current_pid(const muzix_kernel_startup_t *ks)
{
    if (!ks) {
        return -1;
    }

    /* Read it from the table every time rather than caching it in current_pid.
     * The cache went stale the moment muzix_kernel_loop_yield() switched
     * processes - it updates the table, not this field - so the pid reported
     * after a context switch described the old process. */
    return muzix_proc_table_current_pid(&ks->proc_table);
}
