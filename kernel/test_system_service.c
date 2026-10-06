/*
 * host-test-not-portable: this test drives the Z80 memory model, which has no host
 * equivalent.  It passes a muzix_mproc_t pointer and a struct rtc_time buffer to
 * the dispatcher, both truncated to sixteen bits, and they reach
 * zeta_read_visible() and zeta_write_visible() - a volatile byte dereference at a
 * REAL 16-bit address, hardware on the target and a fault at address 0x0100 on a
 * 64-bit host.
 *
 * It never compiled until kernel/kernel_loop.h was added on 2026-10-04, so the
 * type muzix_kernel_loop_t was undeclared and the fault below - a
 * muzix_system_service_t initialised with a null loop, which
 * muzix_system_service_dispatch() accepts and then dereferences - had never been
 * reached.  Whether the dispatcher should refuse a null loop is a separate question
 * from this file being runnable, and it is unresolved.
 */
#include "system_service.h"
#include "kernel_loop.h"

static int test_system_service(void)
{
    muzix_kernel_loop_t loop;
    muzix_system_service_t svc;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t init_map = {{1, 2, 3, 3}};
    muzix_mproc_t init_mp;
    muzix_mproc_t replacement_mp;
    muzix_system_message_t msg;
    uint32_t times[4];

    muzix_mproc_init(&init_mp, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 1);
    muzix_mproc_init(&replacement_mp, 0x3000u, 0x4000u, 0x5000u,
                     0x0800u, 0x1000u, 0x0800u, 2);
    muzix_kernel_loop_init(&loop, &kernel_map, 1, &init_map);
    muzix_kernel_loop_register(&loop, 0, 1, &init_mp, &init_map, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_system_service_init(&svc, &loop);

    {
        muzix_system_service_t detached;
        muzix_system_message_t detached_msg = {0};
        muzix_system_service_init(&detached, 0);
        detached_msg.m_type = MUZIX_SVC_YIELD;
        if (muzix_system_service_dispatch(&detached, &detached_msg) !=
            MUZIX_SYS_SERVICE_ERR) {
            return 1;
        }
    }

    msg.m_type = MUZIX_SVC_FORK;
    msg.proc1 = 0;
    msg.proc2 = 1;
    msg.pid = 2;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        !loop.system.startup.proc_table.slots[1].active ||
        loop.system.startup.proc_table.slots[1].p_flags != MUZIX_PROC_NO_MAP ||
        loop.system.startup.proc_table.slots[1].parent_pid != 1) {
        return 1;
    }

    msg.m_type = MUZIX_SVC_EXEC;
    msg.proc1 = 1;
    msg.stack_ptr = 0xbeefu;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        loop.system.startup.proc_table.slots[1].stack_ptr != 0xbeefu) {
        return 2;
    }
    msg.m_type = MUZIX_SVC_GETSP;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        msg.stack_ptr != 0xbeefu) {
        return 2;
    }

    msg.m_type = MUZIX_SVC_NEWMAP;
    msg.proc1 = 1;
    msg.mem_ptr = (uint16_t)(uintptr_t)&replacement_mp;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        loop.system.startup.proc_table.slots[1].mproc.mp_pid != 2 ||
        loop.system.startup.proc_table.slots[1].mproc.mp_seg[MUZIX_SEG_DATA].mem_phys !=
            0x4000u ||
        loop.system.startup.proc_table.slots[1].map.pages[0] != 3 ||
        loop.system.startup.proc_table.slots[1].map.pages[1] != 0 ||
        loop.system.startup.proc_table.slots[1].map.pages[2] != 1 ||
        loop.system.startup.proc_table.slots[1].p_flags != 0) {
        return 3;
    }

    msg.m_type = MUZIX_SVC_XIT;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        loop.system.startup.proc_table.slots[1].active) {
        return 3;
    }

    loop.system.startup.proc_table.slots[1].active = 1;
    loop.system.startup.proc_table.slots[1].exited = 0;
    loop.system.startup.proc_table.slots[1].parent_pid = 2;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_ERR ||
        !loop.system.startup.proc_table.slots[1].active) {
        return 3;
    }
    loop.system.startup.proc_table.slots[1].parent_pid = 1;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        loop.system.startup.proc_table.slots[1].active) {
        return 3;
    }

    msg.m_type = MUZIX_SVC_YIELD;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK) {
        return 4;
    }

    msg.m_type = MUZIX_SVC_XIT;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK) {
        return 5;
    }

    msg.m_type = 0xff;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_ERR) {
        return 6;
    }

    msg.m_type = MUZIX_SVC_FORK;
    msg.proc1 = 0;
    msg.proc2 = MUZIX_PROC_TABLE_MAX;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_ERR) {
        return 7;
    }

    msg.m_type = MUZIX_SVC_COPY;
    msg.src_proc = 0;
    msg.dst_proc = 0;
    msg.byte_count = 0;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_ERR) {
        return 8;
    }

    msg.m_type = MUZIX_SVC_GETSP;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_ERR) {
        return 9;
    }
    msg.m_type = MUZIX_SVC_TIMES;
    msg.proc1 = 0;
    msg.mem_ptr = (uint16_t)(uintptr_t)times;
    loop.system.startup.proc_table.slots[0].user_time = 1;
    loop.system.startup.proc_table.slots[0].sys_time = 2;
    loop.system.startup.proc_table.slots[0].child_user_time = 3;
    loop.system.startup.proc_table.slots[0].child_sys_time = 4;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_OK ||
        times[0] != 1 || times[1] != 2 || times[2] != 3 || times[3] != 4) {
        return 10;
    }
    msg.m_type = MUZIX_SVC_SIG;
    if (muzix_system_service_dispatch(&svc, &msg) != MUZIX_SYS_SERVICE_ERR) {
        return 11;
    }

    return 0;
}

int main(void)
{
    return test_system_service();
}
