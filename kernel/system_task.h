#ifndef MUZIX_SYSTEM_TASK_H
#define MUZIX_SYSTEM_TASK_H

#include "system_service.h"

struct muzix_system_task_t {
    muzix_system_service_t service;
    void *loop;
};

typedef struct muzix_system_task_t muzix_system_task_t;

void muzix_system_task_init(muzix_system_task_t *task, void *loop, const process_map_t *kernel_map);
void muzix_system_task_stop(muzix_system_task_t *task);
int muzix_system_task_active(const muzix_system_task_t *task);
int muzix_system_task_handle_message(muzix_system_task_t *task,
                                    const muzix_system_message_t *msg);
int muzix_system_task_handle_syscall(muzix_system_task_t *task,
                                    const void *call);

#endif
