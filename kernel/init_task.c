#include "init_task.h"

#include <string.h>

void muzix_init_task_init(muzix_init_task_t *it,
                          const process_map_t *kernel_map,
                          uint8_t init_pid,
                          const process_map_t *init_map)
{
    if (!it) {
        return;
    }

    memset(it, 0, sizeof(*it));
    it->init_pid = init_pid;
    it->init_slot = MUZIX_INIT_TASK_SLOT;
    it->running = 0;
    muzix_bootstrap_init(&it->boot, kernel_map, init_pid, init_map);
}

void muzix_init_task_register(muzix_init_task_t *it,
                             int slot,
                             uint8_t pid,
                             const muzix_mproc_t *mp,
                             const process_map_t *map,
                             uint8_t q)
{
    if (!it) {
        return;
    }

    muzix_bootstrap_register_task(&it->boot, slot, pid, mp, map, q);
}

void muzix_init_task_run(muzix_init_task_t *it)
{
    if (!it) {
        return;
    }

    if (!it->running) {
        muzix_bootstrap_run(&it->boot);
        it->running = 1;
    }
}

void muzix_init_task_switch_to_kernel(muzix_init_task_t *it)
{
    if (!it) {
        return;
    }

    muzix_bootstrap_switch_to_kernel(&it->boot);
    it->running = 0;
}

int muzix_init_task_current_pid(const muzix_init_task_t *it)
{
    if (!it) {
        return -1;
    }

    return muzix_bootstrap_current_pid(&it->boot);
}
