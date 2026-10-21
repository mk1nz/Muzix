#ifndef MUZIX_MSG_POOL_H
#define MUZIX_MSG_POOL_H

#include <stdint.h>
#include <stddef.h>

#include "../include/muzix_message.h"
#include "../mm/mm_service.h"
#include "proc_table.h"

#define MUZIX_MSG_POOL_SIZE 8
#define MUZIX_MSG_SEND 1
#define MUZIX_MSG_RECEIVE 2
#define MUZIX_MSG_BOTH 3

#define MUZIX_MSG_OK 0
#define MUZIX_MSG_EMPTY 1
#define MUZIX_MSG_FULL 2
#define MUZIX_MSG_EFAULT 3
#define MUZIX_MSG_EINVAL 4

typedef struct {
    muzix_system_message_t messages[MUZIX_MSG_POOL_SIZE];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} muzix_msg_pool_t;

typedef struct {
    int sender_slot;
    int receiver_slot;
    uint8_t op;
    uint16_t deadline;
} muzix_msg_waiter_t;

void muzix_msg_pool_init(muzix_msg_pool_t *pool);
int muzix_msg_send(muzix_msg_pool_t *pool,
                   muzix_mm_service_t *mm,
                   muzix_kernel_proc_table_t *proc_table,
                   int sender_slot,
                   int receiver_slot,
                   const muzix_system_message_t *msg,
                   uint8_t op);
int muzix_msg_receive(muzix_msg_pool_t *pool,
                      muzix_mm_service_t *mm,
                      muzix_kernel_proc_table_t *proc_table,
                      int receiver_slot,
                      int sender_slot,
                      muzix_system_message_t *msg,
                      uint8_t op);
int muzix_msg_respond(muzix_msg_pool_t *pool,
                      muzix_mm_service_t *mm,
                      muzix_kernel_proc_table_t *proc_table,
                      int responder_slot,
                      int sender_slot,
                      const muzix_system_message_t *msg);

#endif