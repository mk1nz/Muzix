#include "system_task.h"
#include "../mm/mm_service.h"
#include "../kernel/kernel_loop.h"
#include "syscalls.h"

#include <string.h>

void muzix_system_task_init(muzix_system_task_t *task, void *loop, const process_map_t *kernel_map)
{
    if (!task) {
        return;
    }

    memset(task, 0, sizeof(*task));
    task->loop = loop;
    muzix_system_service_init(&task->service, loop);
    if (kernel_map) {
        task->service.kernel_map = *kernel_map;
    }
}

void muzix_system_task_stop(muzix_system_task_t *task)
{
    if (!task) {
        return;
    }

    task->service.active = 0;
}

int muzix_system_task_active(const muzix_system_task_t *task)
{
    if (!task) {
        return 0;
    }

    return task->service.active != 0;
}

int muzix_system_task_handle_message(muzix_system_task_t *task,
                                    const muzix_system_message_t *msg)
{
    if (!task || !msg || !muzix_system_task_active(task)) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    muzix_kernel_loop_t *loop = (muzix_kernel_loop_t *)task->loop;
    return muzix_system_service_dispatch(&task->service, msg);
}

int muzix_system_task_handle_syscall(muzix_system_task_t *task,
                                    const void *call)
{
    muzix_syscall_context_t ctx;

    if (!task || !call || !muzix_system_task_active(task)) {
        return MUZIX_SYSCALL_FAIL;
    }

    muzix_kernel_loop_t *loop = (muzix_kernel_loop_t *)task->loop;
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = task->loop;
    if (loop) {
        ctx.mm = loop->mm;
        ctx.process_table = &loop->system.startup.proc_table;
    }
    return muzix_handle_syscall(&ctx, (const muzix_syscall_t *)call);
}
