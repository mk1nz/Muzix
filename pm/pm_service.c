#include "pm_service.h"

#include <string.h>

#include "../fs/fs_service.h"
#include "../kernel/exec_loader.h"
#include "../kernel/proc_table.h"
#include "../mm/mm_service.h"

extern void muzix_zeta_uart_send(uint8_t byte, void *userdata);

void muzix_pm_service_init(muzix_pm_service_t *pm,
                           muzix_system_task_t *system_task,
                           uint8_t source)
{
    if (!pm) {
        return;
    }

    memset(pm, 0, sizeof(*pm));
    pm->system_task = system_task;
    pm->source = source;
}

int muzix_pm_fork(muzix_pm_service_t *pm,
                  uint8_t parent_slot,
                  uint8_t child_slot,
                  uint8_t child_pid)
{
    muzix_system_message_t msg;

    if (!pm || !pm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_FORK;
    msg.source = pm->source;
    msg.proc1 = parent_slot;
    msg.proc2 = child_slot;
    msg.pid = child_pid;
    return muzix_system_task_handle_message(pm->system_task, &msg);
}


int muzix_pm_exec(muzix_pm_service_t *pm,
                  uint8_t process_slot,
                  uint16_t stack_ptr)
{
    muzix_system_message_t msg;

    if (!pm || !pm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_EXEC;
    msg.source = pm->source;
    msg.proc1 = process_slot;
    msg.stack_ptr = stack_ptr;
    return muzix_system_task_handle_message(pm->system_task, &msg);
}
