#include "bootstrap.h"

#include <string.h>

void muzix_bootstrap_init(muzix_kernel_bootstrap_t *boot,
                          const process_map_t *kernel_map,
                          uint8_t init_pid,
                          const process_map_t *init_map)
{
    if (!boot) {
        return;
    }

    memset(boot, 0, sizeof(*boot));
    muzix_runtime_state_init(&boot->runtime, kernel_map);
    boot->init_pid = init_pid;
    boot->boot_done = 0;

    if (init_map) {
        process_map_t zero = {{0, 0, 0, 0}};
        muzix_mproc_t mp;
        memset(&mp, 0, sizeof(mp));
        mp.mp_pid = init_pid;
        mp.mp_flags = 1;
        muzix_runtime_state_register(&boot->runtime, 0, init_pid, &mp, init_map, MUZIX_BOOTSTRAP_TASK_Q);
        /* boot_done means "init is registered and ready to be scheduled", so it
         * belongs to a map with pages in it.  The test was the other way round:
         * an all-zero map -- no window mapped at all -- set boot_done, so
         * muzix_bootstrap_run skipped its first pick_next and the loop ran on
         * with nothing schedulable. */
        if (memcmp(init_map, &zero, sizeof(zero)) != 0) {
            boot->boot_done = 1;
        }
    }
}

void muzix_bootstrap_register_task(muzix_kernel_bootstrap_t *boot,
                                  int slot,
                                  uint8_t pid,
                                  const muzix_mproc_t *mp,
                                  const process_map_t *map,
                                  uint8_t q)
{
    if (!boot) {
        return;
    }

    muzix_runtime_state_register(&boot->runtime, slot, pid, mp, map, q);
}

void muzix_bootstrap_run(muzix_kernel_bootstrap_t *boot)
{
    if (!boot) {
        return;
    }

    if (!boot->boot_done) {
        muzix_runtime_state_pick_next(&boot->runtime, NULL);
        boot->boot_done = 1;
    }
}

void muzix_bootstrap_switch_to_kernel(muzix_kernel_bootstrap_t *boot)
{
    if (!boot) {
        return;
    }

    muzix_runtime_state_switch_to_kernel(&boot->runtime, NULL);
}

int muzix_bootstrap_current_pid(const muzix_kernel_bootstrap_t *boot)
{
    if (!boot) {
        return -1;
    }

    return muzix_runtime_state_current_pid(&boot->runtime);
}
