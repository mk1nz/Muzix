#include "system_entry.h"

static int test_system_entry(void)
{
    muzix_system_entry_t sys;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t init_map = {{1, 2, 3, 3}};
    muzix_mproc_t init_mp;

    muzix_mproc_init(&init_mp, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 1);
    muzix_system_entry_init(&sys, &kernel_map, 1, &init_map);
    muzix_system_entry_register(&sys, 0, 1, &init_mp, &init_map, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_system_entry_start(&sys);

    if (muzix_system_entry_current_pid(&sys) != 1) {
        return 1;
    }

    muzix_system_entry_switch_to_kernel(&sys);
    if (muzix_system_entry_current_pid(&sys) != -1) {
        return 2;
    }

    return 0;
}

int main(void)
{
    return test_system_entry();
}
