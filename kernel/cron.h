#ifndef MUZIX_CRON_H
#define MUZIX_CRON_H
 
#include "ztypes.h"
#include "../platform/zeta-v2/tick.h"
 
struct muzix_ipc_table;
struct muzix_kernel_loop;
struct muzix_mm_service;
 
#define MUZIX_CRON_TICKS_PER_MINUTE \
    (MUZIX_TICKS_PER_SECOND * 60u)

#define CRON_IPC_TICK   0x64
#define CRON_IPC_STATUS 0x6b
#define CRON_IPC_START  0x69
#define CRON_IPC_STOP   0x6a
#define CRON_IPC_LIST   0x67
#define CRON_IPC_ADD    0x65
#define CRON_IPC_DEL    0x66

#define CRON_SERVER_SLOT 5

/* Special values for minute/hour fields in add requests: */
/* 0xFF = wildcard (match any minute/hour) */
/* 0xFE = invalid */
#define CRON_WILDCARD 0xFF
/* Interval stored in m1_i3: N for every-N-minutes, 0 for no interval */
#define CRON_INTERVAL_NONE 0
 
void muzix_cron_tick_notify(
    struct muzix_ipc_table *ipc,
    struct muzix_kernel_loop *loop,
    uint16_t now_ticks);
 
#endif
