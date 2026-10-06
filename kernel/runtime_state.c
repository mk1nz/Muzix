#include "runtime_state.h"

#include <string.h>
#include "../mm/mm_service.h"

/* Every operation here is a forward onto proc_table. The struct no longer holds
 * slots, a current index, or ready queues of its own: it used to hold all three,
 * and the copies drifted apart. The startup chain asked its shadow who was
 * runnable, the yield path asked proc_table, and the two answered differently -
 * which is why muzix_kernel_loop_yield never saw a second candidate to switch
 * to and the handoff it performs had nothing to hand off.
 *
 * These functions stay because system_entry -> startup -> init_task -> bootstrap
 * -> runtime_state is a five-deep forwarder chain that other code calls into.
 * Collapsing the whole chain is a separate change; removing the duplicated
 * state inside it is what fixes the scheduler. */

void muzix_runtime_state_init(muzix_runtime_state_t *rt, const process_map_t *kernel_map)
{
    if (!rt) {
        return;
    }

    memset(rt, 0, sizeof(*rt));
    if (kernel_map) {
        rt->kernel_map = *kernel_map;
    }
}

void muzix_runtime_state_register(muzix_runtime_state_t *rt,
                                 int slot,
                                 uint8_t pid,
                                 const muzix_mproc_t *mp,
                                 const process_map_t *map,
                                 uint8_t q)
{
    if (!rt || !rt->table || !mp || !map ||
        slot < 0 || slot >= MUZIX_RUNTIME_MAX_PROC) {
        return;
    }

    muzix_proc_table_register(rt->table, slot, pid, mp, map, q, 0);
}

void muzix_runtime_state_pick_next(muzix_runtime_state_t *rt, muzix_mm_service_t *mm)
{
    if (!rt || !rt->table) {
        return;
    }

    /* Dequeues from the shared ready queues, so the first pick and every
     * subsequent yield work off the same list. The old version only peeked at
     * its own head, which left the process it had already chosen in the queue
     * and starved the other one.
     *
     * The map is not installed from here: the caller is
     * muzix_kernel_loop_run(), which performs the handoff itself, in window 0,
     * via muzix_zeta_enter_userspace(). A message handler or a shadow scheduler
     * in window 1 cannot install a map - see muzix_kernel_loop_yield(). */
    if (muzix_proc_table_pick_next(rt->table) < 0) {
        rt->table->current = -1;
        if (mm) {
            muzix_mm_load_kernel_map(mm, NULL);
        }
    }
}

void muzix_runtime_state_switch_to_kernel(muzix_runtime_state_t *rt, muzix_mm_service_t *mm)
{
    if (!rt || !rt->table) {
        return;
    }

    rt->table->current = -1;
    if (mm) {
        muzix_mm_load_kernel_map(mm, NULL);
    }
}

int muzix_runtime_state_current_pid(const muzix_runtime_state_t *rt)
{
    if (!rt || !rt->table) {
        return -1;
    }

    return muzix_proc_table_current_pid(rt->table);
}
