#ifndef MUZIX_SYSTEM_SERVICE_H
#define MUZIX_SYSTEM_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "../include/muzix_message.h"
#include "../mm/mm_service.h"

/* Forward declaration to break circular dependency with mm_service.h */
struct muzix_mm_service;

#define MUZIX_SYS_SERVICE_OK 0
#define MUZIX_SYS_SERVICE_ERR 1

#define MUZIX_SVC_FORK 1
#define MUZIX_SVC_NEWMAP 2
#define MUZIX_SVC_EXEC 3
#define MUZIX_SVC_XIT 4
#define MUZIX_SVC_GETSP 5
#define MUZIX_SVC_TIMES 6
#define MUZIX_SVC_ABORT 7
#define MUZIX_SVC_SIG 8
#define MUZIX_SVC_COPY 9
#define MUZIX_SVC_YIELD 10
#define MUZIX_SVC_EXIT 11
#define MUZIX_SVC_COPY_KERNEL_TO_USER 12
#define MUZIX_SVC_CONTEXT_SWITCH 20
#define MUZIX_SVC_KERNEL_ENTER 21
#define MUZIX_SVC_KERNEL_EXIT 22
#define MUZIX_SVC_LOAD_MAP 23
#define MUZIX_SVC_LOAD_KERNEL_MAP 24
#define MUZIX_SVC_WITH_TEMP_MAP 25
#define MUZIX_SVC_CROSS_BANK_CALL 26
#define MUZIX_SVC_COPY_PAGE_TO_KERNEL 27
#define MUZIX_SVC_COPY_USER_TO_KERNEL 28
#define MUZIX_SVC_REFRESH_VISIBLE 29

typedef struct {
    void *loop;
    process_map_t kernel_map;
    uint8_t active;
} muzix_system_service_t;

void muzix_system_service_init(muzix_system_service_t *svc, void *loop);
int muzix_system_service_dispatch(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_fork(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_newmap(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_exec(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_xit(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_getsp(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_times(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_abort(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_sig(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_copy(muzix_system_service_t *svc, const muzix_system_message_t *msg);

int muzix_system_service_load_map(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_load_kernel_map(muzix_system_service_t *svc, const muzix_system_message_t *msg);

int muzix_system_service_cross_bank_call(muzix_system_service_t *svc, const muzix_system_message_t *msg);
void muzix_system_service_refresh_visible(muzix_system_service_t *svc, const muzix_system_message_t *msg);

#endif
