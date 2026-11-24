#include "cron.h"
#include "../include/muzix_ipc.h"
#include "../platform/zeta-v2/rtc_ds1302.h"
#include "../platform/zeta-v2/uart_io.h"
#include "kernel_loop.h"
#include "proc_table.h"
#include "../mm/mm_service.h"
#include "libc.h"

static uint16_t g_cron_last_tick = 0;
static uint8_t g_cron_initialized = 0;

void
muzix_cron_init(void) {
    g_cron_last_tick = muzix_tick_now(); g_cron_initialized = 1;
}

void
muzix_cron_tick_notify(
    muzix_ipc_table_t *ipc,
    muzix_kernel_loop_t *loop,
    uint16_t now_ticks) {
    if (!g_cron_initialized) { return; }

    uint16_t elapsed = (uint16_t)(now_ticks - g_cron_last_tick);
    if (elapsed < MUZIX_CRON_TICKS_PER_MINUTE) { return; }

    g_cron_last_tick = now_ticks;

    muzix_rtc_time_t now;
    if (muzix_zeta_rtc_get(&now) < 0) {
        memset(&now, 0, sizeof(now));
    }

    muzix_ipc_message_t msg; memset(&msg, 0, sizeof(msg));
    msg.m_type = CRON_IPC_TICK;
    msg.m_source = 0;
    msg.m1_i1 = (int16_t)now.minute;
    msg.m1_i2 = (int16_t)now.hour;

    int32_t result = muzix_ipc_send(ipc, 0, MUZIX_PROC_CRON, &msg);
    if (result >= 10) {
        int woken_slot = result - 10;
        muzix_ipc_proc_t *rp = muzix_ipc_proc(ipc, (int16_t)woken_slot);
        if (loop && rp && (rp->p_flags & MUZIX_IPC_FLAG_DELIVERED)) {
            muzix_kernel_proc_table_t *table = &loop->system.startup.proc_table;
            uint16_t retval = (rp->p_getfrom == MUZIX_IPC_ANY) ? (uint16_t)0 : (uint16_t)rp->p_msg.m1_i1;
            if (rp->p_user_addr != 0 && loop->mm) {
                muzix_mm_copy_kernel_to_user(loop->mm, (uint8_t)woken_slot,
                    rp->p_user_addr,
                    (const uint8_t *)&rp->p_msg, sizeof(rp->p_msg));
            }
            muzix_proc_table_set_resume(table,
                                        woken_slot,
                                        table->slots[woken_slot].resume_pc,
                                        table->slots[woken_slot].resume_sp,
                                        retval);
            rp->p_flags &= (uint8_t)~MUZIX_IPC_FLAG_DELIVERED;
            muzix_proc_table_ready(table, woken_slot);
        }
    }
}
