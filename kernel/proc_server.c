#include "proc_server.h"

#include <string.h>

#include "proc_table.h"

void muzix_proc_server_init(muzix_proc_server_t *server,
                            muzix_kernel_proc_table_t *proc_table)
{
    if (!server) {
        return;
    }
    memset(server, 0, sizeof(*server));
    server->proc_table = proc_table;
    server->active = 1;
}

static int find_slot_by_pid(muzix_kernel_proc_table_t *table, uint8_t pid)
{
    if (!table || pid == 0) {
        return -1;
    }
    for (int slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
        if (table->slots[slot].active && table->slots[slot].pid == pid) {
            return slot;
        }
    }
    return -1;
}

int32_t muzix_proc_server_handle(muzix_proc_server_t *server,
                                 const muzix_proc_server_msg_t *msg)
{
    int32_t result = -1;

    if (!server || !msg || !server->active || !server->proc_table) {
        return -1;
    }

    switch (msg->type) {
    case MUZIX_PROC_SERVER_FORK:
        /* Fork is handled inline from syscalls.c for context switch.
         * This message path is for userspace mode. */
        result = -1;
        break;

    case MUZIX_PROC_SERVER_EXIT: {
        int slot = find_slot_by_pid(server->proc_table, msg->pid);
        if (slot >= 0) {
            result = muzix_proc_table_exit_status(server->proc_table, slot,
                                                  msg->arg1, (uint8_t)msg->arg2) == 0 ? 0 : -1;
        }
        break;
    }

    case MUZIX_PROC_SERVER_WAIT: {
        int parent_slot = find_slot_by_pid(server->proc_table, msg->pid);
        if (parent_slot >= 0) {
            for (int slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
                muzix_proc_slot_t *p = &server->proc_table->slots[slot];
                if (!p->exited || p->parent_pid != server->proc_table->slots[parent_slot].pid) {
                    continue;
                }
                result = p->mproc.mp_pid;
                p->exited = 0;
                p->pid = 0;
                break;
            }
        }
        break;
    }

    default:
        result = -1;
        break;
    }

    return result;
}

void muzix_proc_server_loop(muzix_proc_server_t *server)
{
    while (server && server->active) {
        /* In userspace mode, this would:
         * 1. Receive message from kernel via syscall/IPC
         * 2. Call muzix_proc_server_handle()
         * 3. Send reply back
         * For now, park and yield. */
        muzix_proc_table_park(server->proc_table, 0, 240);
        muzix_proc_table_pick_next(server->proc_table);
    }
}