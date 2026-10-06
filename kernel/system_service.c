#include "system_service.h"
#include "../platform/zeta-v2/trace.h"

#include <string.h>
#include "../platform/zeta-v2/trace.h"
#include "proc_table.h"
#include "kernel_loop.h"
#include "../mm/mm_service.h"
#include "../mm/mm_copy.h"
#include "../platform/zeta-v2/context_switch.h"

int muzix_system_service_load_map(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_load_kernel_map(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_with_temp_map(muzix_system_service_t *svc, const muzix_system_message_t *msg);
int muzix_system_service_cross_bank_call(muzix_system_service_t *svc, const muzix_system_message_t *msg);
void muzix_system_service_refresh_visible(muzix_system_service_t *svc, const muzix_system_message_t *msg);
static int muzix_system_service_valid_slot(uint8_t slot)
{
    return slot < MUZIX_PROC_TABLE_MAX;
}

void muzix_system_service_init(muzix_system_service_t *svc, void *loop)
{
    if (!svc) {
        return;
    }

    memset(svc, 0, sizeof(*svc));
    svc->loop = loop;
    svc->active = 1;
}

int muzix_system_service_dispatch(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    if (!svc || !msg || !svc->active) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    switch (msg->m_type) {
    case MUZIX_SVC_FORK:
        return muzix_system_service_fork(svc, msg);
    case MUZIX_SVC_NEWMAP:
        return muzix_system_service_newmap(svc, msg);
    case MUZIX_SVC_EXEC:
        return muzix_system_service_exec(svc, msg);
    case MUZIX_SVC_XIT:
        return muzix_system_service_xit(svc, msg);
    case MUZIX_SVC_YIELD:
        if (!svc->loop) {
            return MUZIX_SYS_SERVICE_ERR;
        }
        muzix_kernel_loop_yield((muzix_kernel_loop_t *)svc->loop);
        return MUZIX_SYS_SERVICE_OK;
    case MUZIX_SVC_GETSP:
        return muzix_system_service_getsp(svc, msg);
    case MUZIX_SVC_TIMES:
        return muzix_system_service_times(svc, msg);
    case MUZIX_SVC_ABORT:
        return muzix_system_service_abort(svc, msg);
    case MUZIX_SVC_SIG:
        return muzix_system_service_sig(svc, msg);
    case MUZIX_SVC_COPY:
        return muzix_system_service_copy(svc, msg);
    case MUZIX_SVC_COPY_KERNEL_TO_USER:
        return muzix_mm_handle_copy_kernel_to_user(svc, msg);
    case MUZIX_SVC_LOAD_MAP:
        return muzix_system_service_load_map(svc, msg);
    case MUZIX_SVC_LOAD_KERNEL_MAP:
        return muzix_system_service_load_kernel_map(svc, msg);
    case MUZIX_SVC_CROSS_BANK_CALL:
        return muzix_system_service_cross_bank_call(svc, msg);
    case MUZIX_SVC_COPY_PAGE_TO_KERNEL:
        return muzix_mm_handle_copy_page_to_kernel(svc, msg);
    case MUZIX_SVC_COPY_USER_TO_KERNEL:
        return muzix_mm_handle_copy_user_to_kernel(svc, msg);
    case MUZIX_SVC_REFRESH_VISIBLE:
        muzix_system_service_refresh_visible(svc, msg);
        return MUZIX_SYS_SERVICE_OK;
    default:
        return MUZIX_SYS_SERVICE_ERR;
    }
}

int muzix_system_service_fork(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    ;;
    ;;
    if (!msg || !muzix_system_service_valid_slot(msg->proc1) ||
        !muzix_system_service_valid_slot(msg->proc2) || msg->proc1 == msg->proc2) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    if (!loop) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    if (muzix_proc_table_fork(&loop->system.startup.proc_table,
                              msg->proc1, msg->proc2, msg->pid) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_newmap(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_proc_slot_t *slot;
    muzix_mproc_t *mp;
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;

    if (!loop || !msg || !msg->mem_ptr ||
        !muzix_system_service_valid_slot(msg->proc1)) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    slot = &loop->system.startup.proc_table.slots[msg->proc1];
    if (!slot->active) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    mp = (muzix_mproc_t *)(uintptr_t)msg->mem_ptr;
    slot->mproc = *mp;
    muzix_mproc_to_zeta_map(&slot->mproc, &slot->map);
    slot->p_flags &= (uint8_t)~MUZIX_PROC_NO_MAP;
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_exec(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    if (!loop || !msg || !muzix_system_service_valid_slot(msg->proc1) ||
        muzix_proc_table_exec(&loop->system.startup.proc_table,
                              msg->proc1, msg->stack_ptr) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_xit(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_kernel_proc_table_t *table;

    if (!msg || !muzix_system_service_valid_slot(msg->proc1) ||
        !muzix_system_service_valid_slot(msg->proc2)) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    if (!loop) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    table = &loop->system.startup.proc_table;
    if (!table->slots[msg->proc1].active || !table->slots[msg->proc2].active ||
        table->slots[msg->proc2].parent_pid != table->slots[msg->proc1].pid) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    /* Return the exiting process's owned pages before the slot is torn down,
     * while the ownership record still points at them. Earlier versions only
     * freed pages on exec failure; this exit path closes that leak. A
     * forked-but-not-yet-exec'd child may own a private stack page, so release
     * the recorded ownership rather than assuming the child owns nothing. */
    muzix_proc_table_release_pages(table, msg->proc2, loop->mm);

    if (muzix_proc_table_exit_status(table, msg->proc2,
                                     table->slots[msg->proc1].pid, 0) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    muzix_kernel_loop_switch_to_kernel(loop);
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_getsp(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_proc_slot_t *slot;
    muzix_system_message_t *response;

    if (!loop || !msg ||
        !muzix_system_service_valid_slot(msg->proc1)) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    slot = &loop->system.startup.proc_table.slots[msg->proc1];
    if (!slot->active) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    response = (muzix_system_message_t *)(uintptr_t)msg;
    response->stack_ptr = slot->stack_ptr;
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_times(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_proc_slot_t *slot;
    typedef struct {
        uint32_t user_time;
        uint32_t sys_time;
        uint32_t child_user_time;
        uint32_t child_sys_time;
    } muzix_service_tms_t;
    muzix_service_tms_t *times;

    if (!loop || !msg ||
        !muzix_system_service_valid_slot(msg->proc1) || !msg->mem_ptr) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    slot = &loop->system.startup.proc_table.slots[msg->proc1];
    if (!slot->active) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    times = (muzix_service_tms_t *)(uintptr_t)msg->mem_ptr;
    times->user_time = slot->user_time;
    times->sys_time = slot->sys_time;
    times->child_user_time = slot->child_user_time;
    times->child_sys_time = slot->child_sys_time;
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_abort(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    if (!loop || !msg) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    muzix_kernel_loop_switch_to_kernel(loop);
    loop->state = MUZIX_KERNEL_LOOP_SHUTDOWN;
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_sig(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    if (!loop || !msg ||
        !muzix_system_service_valid_slot(msg->proc1) ||
        muzix_proc_table_queue_signal(
            &loop->system.startup.proc_table,
            msg->proc1, msg->signal) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_copy(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_kernel_proc_table_t *table;

    if (!msg || !muzix_system_service_valid_slot(msg->src_proc) ||
        !muzix_system_service_valid_slot(msg->dst_proc) || msg->byte_count == 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    if (!loop) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    table = &loop->system.startup.proc_table;
    if (!table->slots[msg->src_proc].active || !table->slots[msg->dst_proc].active) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    return muzix_mm_copy_maps(loop->mm,
                              &table->slots[msg->src_proc].mproc,
                              msg->src_seg,
                              msg->src_vir,
                              &table->slots[msg->dst_proc].mproc,
                              msg->dst_seg,
                              msg->dst_vir,
                              msg->byte_count) == MUZIX_COPY_OK
                 ? MUZIX_SYS_SERVICE_OK
                 : MUZIX_SYS_SERVICE_ERR;
}

int muzix_system_service_load_map(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_kernel_proc_table_t *table;
    muzix_mm_service_t *mm;
    const process_map_t *map;

    if (!loop || !msg) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    table = &loop->system.startup.proc_table;
    mm = loop->mm;
    map = (const process_map_t *)(uintptr_t)msg->mem_ptr;
    if (!map) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    if (msg->proc1 < MUZIX_PROC_TABLE_MAX) {
        muzix_proc_slot_t *slot = &table->slots[msg->proc1];
        if (slot->active) {
            slot->map = *map;
        }
    }

    /* Call the primitive directly. muzix_mm_load_map() builds a message with
     * the same m_type and re-enters this handler, so calling it from here is
     * unbounded recursion on a stack with about 1.3 KiB of headroom. The
     * primitive's result is propagated: this handler used to return OK whatever
     * it did, so a refused map install was indistinguishable from a successful
     * one at every call site. */
    if (zeta_load_map(&mm->zeta, map) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    return MUZIX_SYS_SERVICE_OK;
}

int muzix_system_service_load_kernel_map(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_mm_service_t *mm;
    const process_map_t *map;

    if (!loop || !msg) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    mm = loop->mm;
    if (!mm) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    /* A NULL map means "install the kernel map", which is what all five callers
     * want: kernel/{scheduler,process_state,runtime_state}.c pass NULL from
     * their switch-to-kernel paths. This used to return OK immediately without
     * touching a single bank register, so every one of those calls did nothing
     * and reported that it had worked. */
    map = (const process_map_t *)(uintptr_t)msg->mem_ptr;
    if (!map) {
        map = &mm->kernel_map;
    }

    if (zeta_load_map(&mm->zeta, map) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    return MUZIX_SYS_SERVICE_OK;
}

void muzix_system_service_refresh_visible(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_mm_service_t *mm;

    if (!loop) {
        return;
    }

    mm = loop->mm;

    /* Call the primitive directly. muzix_mm_refresh_visible() builds a message
     * with the same m_type and re-enters this handler, so calling it from here
     * recursed without bound. Every user copy reaches this, through
     * copy_user_to_kernel, and the recursion ran the kernel stack roughly
     * 57 KB down - well past its 1.2 KB - before wrapping. */
    zeta_refresh_visible(&mm->zeta);
}

int muzix_system_service_cross_bank_call(muzix_system_service_t *svc, const muzix_system_message_t *msg)
{
    muzix_kernel_loop_t *loop;

    if (!svc) {
        return MUZIX_SYS_SERVICE_ERR;
    }
    loop = (muzix_kernel_loop_t *)svc->loop;
    muzix_mm_service_t *mm;
    const process_map_t *target_map;
    int (*func)(void *);
    void *arg;
    zeta_callback_id_t cb;

    if (!loop || !msg) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    mm = loop->mm;
    target_map = (const process_map_t *)(uintptr_t)msg->mem_ptr;
    func = (int (*)(void *))(uintptr_t)msg->stack_ptr;
    arg = (void *)(uintptr_t)msg->src_vir;
    if (!target_map || !func) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    cb = zeta_callback_from_fn(func);
    if (cb == ZETA_CB_NONE) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    /* Direct call: the muzix_mm_* wrapper re-dispatches the same m_type. */
    if (zeta_cross_bank_call_impl(&mm->zeta, target_map, cb, arg) != 0) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    return MUZIX_SYS_SERVICE_OK;
}
