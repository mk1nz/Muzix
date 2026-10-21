#ifndef MUZIX_PROC_SERVER_H
#define MUZIX_PROC_SERVER_H

#include <stdint.h>
#include <stddef.h>

#include "proc_table.h"

#define MUZIX_PROC_SERVER_FORK   1
#define MUZIX_PROC_SERVER_EXIT   11
#define MUZIX_PROC_SERVER_WAIT   4

typedef struct {
    uint8_t type;
    uint8_t src_slot;
    uint8_t dst_slot;
    uint8_t pid;
    uint8_t signal;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
    int32_t result;
} muzix_proc_server_msg_t;

typedef struct {
    muzix_kernel_proc_table_t *proc_table;
    uint8_t active;
} muzix_proc_server_t;

void muzix_proc_server_init(muzix_proc_server_t *server,
                            muzix_kernel_proc_table_t *proc_table);

int32_t muzix_proc_server_handle(muzix_proc_server_t *server,
                                 const muzix_proc_server_msg_t *msg);

void muzix_proc_server_loop(muzix_proc_server_t *server);

#endif