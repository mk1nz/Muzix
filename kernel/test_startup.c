#include "startup.h"

static int test_startup(void)
{
    muzix_kernel_startup_t ks;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t init_map = {{1, 2, 3, 3}};
    muzix_mproc_t init_mp;

    muzix_mproc_init(&init_mp, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 1);
    muzix_kernel_startup_init(&ks, &kernel_map, 1, &init_map);
    muzix_kernel_startup_register(&ks, 0, 1, &init_mp, &init_map, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_kernel_startup_boot(&ks);

    if (muzix_kernel_startup_current_pid(&ks) != 1) {
        return 1;
    }

    muzix_kernel_startup_switch_to_kernel(&ks);
    if (muzix_kernel_startup_current_pid(&ks) != -1) {
        return 2;
    }

    return 0;
}

int main(void)
{
    return test_startup();
}
