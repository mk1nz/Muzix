#include "msg_pool.h"

#include <string.h>

#include "../platform/zeta-v2/tick.h"
#include "../mm/mm_copy.h"

static int muzix_msg_pool_enqueue(muzix_msg_pool_t *pool,
                                  const muzix_system_message_t *msg)
{
    if (!pool || pool->count >= MUZIX_MSG_POOL_SIZE) {
        return MUZIX_MSG_FULL;
    }
    pool->messages[pool->tail] = *msg;
    pool->tail = (pool->tail + 1) & (MUZIX_MSG_POOL_SIZE - 1);
    pool->count++;
    return MUZIX_MSG_OK;
}

static int muzix_msg_pool_dequeue(muzix_msg_pool_t *pool,
                                  muzix_system_message_t *msg)
{
    if (!pool || pool->count == 0) {
        return MUZIX_MSG_EMPTY;
    }
    *msg = pool->messages[pool->head];
    pool->head = (pool->head + 1) & (MUZIX_MSG_POOL_SIZE - 1);
    pool->count--;
    return MUZIX_MSG_OK;
}

static int muzix_msg_pool_find(muzix_msg_pool_t *pool,
                               int sender_slot,
                               int receiver_slot,
                               muzix_system_message_t *msg)
{
    uint8_t i;
    uint8_t idx;

    if (!pool || pool->count == 0) {
        return MUZIX_MSG_EMPTY;
    }

    for (i = 0; i < pool->count; i++) {
        idx = (pool->head + i) & (MUZIX_MSG_POOL_SIZE - 1);
        if ((sender_slot < 0 || pool->messages[idx].source == (uint8_t)sender_slot) &&
            (receiver_slot < 0 || pool->messages[idx].dst_proc == (uint8_t)receiver_slot)) {
            *msg = pool->messages[idx];
            for (; i < pool->count - 1; i++) {
                uint8_t from = (idx + 1) & (MUZIX_MSG_POOL_SIZE - 1);
                uint8_t to = idx;
                pool->messages[to] = pool->messages[from];
                idx = from;
            }
            pool->tail = (pool->tail + MUZIX_MSG_POOL_SIZE - 1) & (MUZIX_MSG_POOL_SIZE - 1);
            pool->count--;
            return MUZIX_MSG_OK;
        }
    }
    return MUZIX_MSG_EMPTY;
}

static void muzix_msg_copy_from_user(muzix_mm_service_t *mm,
                                     int src_proc,
                                     const muzix_system_message_t *src,
                                     muzix_system_message_t *dst)
{
    (void)mm;
    (void)src_proc;
    *dst = *src;
}

static void muzix_msg_copy_to_user(muzix_mm_service_t *mm,
                                   int dst_proc,
                                   const muzix_system_message_t *src,
                                   muzix_system_message_t *dst)
{
    (void)mm;
    (void)dst_proc;
    *dst = *src;
}

void muzix_msg_pool_init(muzix_msg_pool_t *pool)
{
    if (!pool) {
        return;
    }
    memset(pool, 0, sizeof(*pool));
}

int muzix_msg_send(muzix_msg_pool_t *pool,
                   muzix_mm_service_t *mm,
                   muzix_kernel_proc_table_t *proc_table,
                   int sender_slot,
                   int receiver_slot,
                   const muzix_system_message_t *msg,
                   uint8_t op)
{
    muzix_system_message_t kern_msg;
    int result;

    if (!pool || !mm || !proc_table || !msg ||
        sender_slot < 0 || sender_slot >= MUZIX_PROC_TABLE_MAX ||
        receiver_slot < 0 || receiver_slot >= MUZIX_PROC_TABLE_MAX) {
        return MUZIX_MSG_EINVAL;
    }

    if (!proc_table->slots[receiver_slot].active) {
        return MUZIX_MSG_EFAULT;
    }

    muzix_msg_copy_from_user(mm, sender_slot, msg, &kern_msg);
    kern_msg.source = (uint8_t)sender_slot;
    kern_msg.dst_proc = (uint8_t)receiver_slot;

    if ((op & MUZIX_MSG_RECEIVE) != 0) {
        if (proc_table->slots[receiver_slot].p_flags & MUZIX_PROC_WAIT_TICK) {
            if (muzix_msg_pool_find(pool, receiver_slot, sender_slot, &kern_msg) == MUZIX_MSG_OK) {
                muzix_msg_copy_to_user(mm, receiver_slot, &kern_msg,
                                       (muzix_system_message_t *)msg);
                return MUZIX_MSG_OK;
            }
        }
    }

    result = muzix_msg_pool_enqueue(pool, &kern_msg);
    if (result != MUZIX_MSG_OK) {
        return result;
    }

    if ((op & MUZIX_MSG_SEND) != 0) {
        if (!(proc_table->slots[receiver_slot].p_flags & MUZIX_PROC_WAIT_TICK)) {
            return MUZIX_MSG_OK;
        }

        muzix_proc_table_park(proc_table, sender_slot, 240);
        return MUZIX_MSG_OK;
    }

    return MUZIX_MSG_OK;
}

int muzix_msg_receive(muzix_msg_pool_t *pool,
                      muzix_mm_service_t *mm,
                      muzix_kernel_proc_table_t *proc_table,
                      int receiver_slot,
                      int sender_slot,
                      muzix_system_message_t *msg,
                      uint8_t op)
{
    muzix_system_message_t kern_msg;
    int result;

    if (!pool || !mm || !proc_table || !msg ||
        receiver_slot < 0 || receiver_slot >= MUZIX_PROC_TABLE_MAX) {
        return MUZIX_MSG_EINVAL;
    }

    if (!proc_table->slots[receiver_slot].active) {
        return MUZIX_MSG_EFAULT;
    }

    if (sender_slot >= 0 &&
        (sender_slot >= MUZIX_PROC_TABLE_MAX || !proc_table->slots[sender_slot].active)) {
        return MUZIX_MSG_EFAULT;
    }

    result = muzix_msg_pool_find(pool, sender_slot, receiver_slot, &kern_msg);
    if (result == MUZIX_MSG_OK) {
        muzix_msg_copy_to_user(mm, receiver_slot, &kern_msg, msg);
        return MUZIX_MSG_OK;
    }

    if ((op & MUZIX_MSG_SEND) != 0) {
        return MUZIX_MSG_EMPTY;
    }

    muzix_proc_table_park(proc_table, receiver_slot, 240);
    return MUZIX_MSG_EMPTY;
}

int muzix_msg_respond(muzix_msg_pool_t *pool,
                      muzix_mm_service_t *mm,
                      muzix_kernel_proc_table_t *proc_table,
                      int responder_slot,
                      int sender_slot,
                      const muzix_system_message_t *msg)
{
    muzix_system_message_t kern_msg;

    if (!pool || !mm || !proc_table || !msg ||
        responder_slot < 0 || responder_slot >= MUZIX_PROC_TABLE_MAX ||
        sender_slot < 0 || sender_slot >= MUZIX_PROC_TABLE_MAX) {
        return MUZIX_MSG_EINVAL;
    }

    if (!proc_table->slots[sender_slot].active ||
        !proc_table->slots[responder_slot].active) {
        return MUZIX_MSG_EFAULT;
    }

    muzix_msg_copy_from_user(mm, responder_slot, msg, &kern_msg);
    kern_msg.source = (uint8_t)responder_slot;
    kern_msg.dst_proc = (uint8_t)sender_slot;

    return muzix_msg_pool_enqueue(pool, &kern_msg);
}