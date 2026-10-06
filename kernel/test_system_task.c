#include "system_task.h"

/* muzix_kernel_loop_t for the loop below, muzix_syscall_t for the syscall it
 * handles.  system_task.h declares neither, and neither is defined here. */
#include "kernel_loop.h"
#include "syscalls.h"

static int test_system_task(void)
{
    muzix_kernel_loop_t loop;
    muzix_system_task_t task;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t init_map = {{1, 2, 3, 3}};
    muzix_mproc_t init_mp;
    muzix_system_message_t msg;
    muzix_syscall_t call;

    muzix_mproc_init(&init_mp, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 1);
    muzix_kernel_loop_init(&loop, &kernel_map, 1, &init_map);
    muzix_kernel_loop_register(&loop, 0, 1, &init_mp, &init_map, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    /* Third argument.  The system task used to find the kernel map through the
     * loop; it is now handed the map and keeps its own copy, because that is
     * what lets a service ask for the kernel map without reaching back through
     * the loop.  Asserted below so the copy is not merely passed. */
    muzix_system_task_init(&task, &loop, &kernel_map);

    if (!muzix_system_task_active(&task) ||
        task.service.kernel_map.pages[0] != 7 ||
        task.service.kernel_map.pages[3] != 7) {
        return 1;
    }

    msg.m_type = MUZIX_SVC_YIELD;
    if (muzix_system_task_handle_message(&task, &msg) != MUZIX_SYS_SERVICE_OK) {
        return 2;
    }

    call.call = MUZIX_SYS_YIELD;
    call.pid = 1;
    call.target_pid = 1;
    if (muzix_system_task_handle_syscall(&task, &call) != MUZIX_SYSCALL_OK) {
        return 3;
    }

    muzix_system_task_stop(&task);
    if (muzix_system_task_active(&task)) {
        return 4;
    }
    if (muzix_system_task_handle_message(&task, &msg) != MUZIX_SYS_SERVICE_ERR) {
        return 5;
    }

    return 0;
}

int main(void)
{
    return test_system_task();
}
