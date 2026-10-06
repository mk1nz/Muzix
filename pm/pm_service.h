#ifndef MUZIX_PM_SERVICE_H
#define MUZIX_PM_SERVICE_H

#include "../kernel/system_task.h"
#include "../fs/fs_service.h"
#include "../mm/mm_service.h"

typedef struct {
    muzix_system_task_t *system_task;
    uint8_t source;
} muzix_pm_service_t;

void muzix_pm_service_init(muzix_pm_service_t *pm,
                           muzix_system_task_t *system_task,
                           uint8_t source);

int muzix_pm_fork(muzix_pm_service_t *pm,
                  uint8_t parent_slot,
                  uint8_t child_slot,
                  uint8_t child_pid);

int muzix_pm_exec(muzix_pm_service_t *pm,
                  uint8_t process_slot,
                  uint16_t stack_ptr);


#endif