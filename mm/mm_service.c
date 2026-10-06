#include "mm_service.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../kernel/system_task.h"
#include "../kernel/kernel_loop.h"
#include "../kernel/proc_table.h"

#include <string.h>

void muzix_mm_service_init(muzix_mm_service_t *mm,
                           struct muzix_system_task_t *system_task,
                           uint8_t source)
{
    if (!mm) {
        return;
    }

    memset(mm, 0, sizeof(*mm));
    mm->system_task = system_task;
    mm->source = source;
}

int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page)
{
    if (!mm || !mm->system_task || !page) {
        return -1;
    }
    return zeta_memory_alloc_page(page);
}

void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page)
{
    if (!mm) {
        return;
    }
    zeta_memory_release_page(page);
}

void muzix_mm_memory_init(muzix_mm_service_t *mm, const process_map_t *kernel_map)
{
    if (!mm) {
        return;
    }
    mm->kernel_map = *kernel_map;
    zeta_init(&mm->zeta);
    zeta_memory_init(kernel_map);
}

int muzix_mm_newmap(muzix_mm_service_t *mm, uint8_t proc_slot, uint16_t map_ptr)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_NEWMAP;
    msg.source = mm->source;
    msg.proc1 = proc_slot;
    msg.mem_ptr = map_ptr;
    return muzix_system_task_handle_message(mm->system_task, &msg);
}

int muzix_mm_copy(muzix_mm_service_t *mm,
                  uint8_t src_proc,
                  uint8_t src_seg,
                  uint16_t src_vir,
                  uint8_t dst_proc,
                  uint8_t dst_seg,
                  uint16_t dst_vir,
                  uint16_t byte_count)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_COPY;
    msg.source = mm->source;
    msg.src_proc = src_proc;
    msg.src_seg = src_seg;
    msg.src_vir = src_vir;
    msg.dst_proc = dst_proc;
    msg.dst_seg = dst_seg;
    msg.dst_vir = dst_vir;
    msg.byte_count = byte_count;
return muzix_system_task_handle_message(mm->system_task, &msg);
}
int muzix_mm_load_map(muzix_mm_service_t *mm,
                       uint8_t proc,
                       const process_map_t *map)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_LOAD_MAP;
    msg.source = mm->source;
    msg.proc1 = proc;
    msg.mem_ptr = (uint16_t)(uintptr_t)map;
    return muzix_system_task_handle_message(mm->system_task, &msg);
}

int muzix_mm_load_kernel_map(muzix_mm_service_t *mm,
                              const process_map_t *map)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_LOAD_KERNEL_MAP;
    msg.source = mm->source;
    msg.mem_ptr = (uint16_t)(uintptr_t)map;
    return muzix_system_task_handle_message(mm->system_task, &msg);
}
int muzix_mm_cross_bank_call(muzix_mm_service_t *mm,
                               const process_map_t *target_map,
                               int (*func)(void *),
                               void *arg)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_CROSS_BANK_CALL;
    msg.source = mm->source;
    msg.mem_ptr = (uint16_t)(uintptr_t)target_map;
    msg.stack_ptr = (uint16_t)(uintptr_t)func;
    msg.src_vir = (uint16_t)(uintptr_t)arg;
    return muzix_system_task_handle_message(mm->system_task, &msg);
}

int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t page,
                                  uint16_t offset,
                                  void *dst,
                                  uint16_t len)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task || !dst) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_COPY_PAGE_TO_KERNEL;
    msg.source = mm->source;
    msg.proc1 = 0;  /* region not used with new API */
    msg.proc2 = page;
    msg.src_vir = offset;
    msg.dst_vir = (uint16_t)(uintptr_t)dst;
    msg.byte_count = len;
    int result = muzix_system_task_handle_message(mm->system_task, &msg);
    return result;
}

/* Copy from ROM bank page to kernel buffer (for ROMFS reads). The bank
 * switching is the platform layer's job - see muzix_mm_copy_rom_page_to_kernel. */

int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm,
                                      uint8_t page,
                                      uint16_t offset,
                                      void *dst,
                                      uint16_t len)
{
    /* Delegate to the platform layer, which owns the bank registers.
     *
     * This used to do the bank switch itself, with region 1 and the window
     * base 0x4000 hardcoded in the MM. That is the manual bank switching
     * AGENTS.md forbids, it saved and restored a single region rather than the
     * complete kernel map, and it was the only banked operation in the tree not
     * going through the platform layer's temp-map machinery.
     *
     * The primitive keeps the staged ldir, which is the reason the path is
     * viable at all: the byte-at-a-time version cost about 145k cycles per
     * 512-byte block, so loading the 6.7 KiB shell image took some 12 million
     * cycles before userspace ran its first instruction.
     */
    if (!mm || !dst || len == 0) {
        return -1;
    }

    return zeta_copy_rom_page_to_kernel(&mm->zeta, page, offset,
                                        (uint8_t *)dst, (size_t)len);
}

int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t src_proc,
                                 uint16_t src_vir,
                                 void *kernel_buf,
                                 uint16_t len)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task || !kernel_buf) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_COPY_USER_TO_KERNEL;
    msg.source = mm->source;
    msg.proc1 = src_proc;
    msg.src_vir = src_vir;
    msg.dst_vir = (uint16_t)(uintptr_t)kernel_buf;
    msg.byte_count = len;
    return muzix_system_task_handle_message(mm->system_task, &msg);
}

int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm,
                                  uint8_t dst_proc,
                                  uint16_t dst_vir,
                                  const void *kernel_buf,
                                  uint16_t len)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task || !kernel_buf) {
        return MUZIX_SYS_SERVICE_ERR;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_COPY_KERNEL_TO_USER;
    msg.source = mm->source;
    msg.dst_proc = dst_proc;
    msg.dst_vir = dst_vir;
    msg.mem_ptr = (uint16_t)(uintptr_t)kernel_buf;
    msg.byte_count = len;
    return muzix_system_task_handle_message(mm->system_task, &msg);
}

void muzix_mm_refresh_visible(muzix_mm_service_t *mm)
{
    muzix_system_message_t msg;

    if (!mm || !mm->system_task) {
        return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_type = MUZIX_SVC_REFRESH_VISIBLE;
    msg.source = mm->source;
    muzix_system_task_handle_message(mm->system_task, &msg);
}
