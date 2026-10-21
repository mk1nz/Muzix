#include "mm_service.h"
#include "syscall.h"
#include <string.h>

#define MUZIX_SVC_ALLOC_PAGE  100
#define MUZIX_SVC_FREE_PAGE   101
#define MUZIX_SVC_NEWMAP      102
#define MUZIX_SVC_LOAD_MAP    103
#define MUZIX_SVC_LOAD_KERNEL_MAP 104
#define MUZIX_SVC_COPY        105
#define MUZIX_SVC_COPY_K2U    106
#define MUZIX_SVC_COPY_U2K    107
#define MUZIX_SVC_COPY_ROM    108
#define MUZIX_SVC_REFRESH     109

#define MUZIX_IPC_MM 2

void muzix_mm_service_init(muzix_mm_service_t *mm, uint8_t server_id)
{
    if (!mm) return;
    mm->server_id = server_id;
    mm->kernel_map.pages[0] = 0x20;
    mm->kernel_map.pages[1] = 0x21;
    mm->kernel_map.pages[2] = 0x22;
    mm->kernel_map.pages[3] = 0x23;
}

static int mm_ipc_call(muzix_mm_service_t *mm, uint8_t type, void *msg, size_t msg_size)
{
    (void)mm;
    (void)type;
    (void)msg;
    (void)msg_size;
    /* Stub: MM server cannot use IPC to talk to itself.
     * Should implement local page allocator instead. */
    return -1;
}

int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page)
{
    uint8_t result_page;
    int result;
    
    if (!mm || !page) return -1;
    
    result = mm_ipc_call(mm, MUZIX_SVC_ALLOC_PAGE, &result_page, sizeof(result_page));
    if (result >= 0) {
        *page = result_page;
        return 0;
    }
    return -1;
}

void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page)
{
    if (!mm) return;
    mm_ipc_call(mm, MUZIX_SVC_FREE_PAGE, &page, sizeof(page));
}

void muzix_mm_memory_init(muzix_mm_service_t *mm, const process_map_t *kernel_map)
{
    if (!mm || !kernel_map) return;
    mm->kernel_map = *kernel_map;
}

int muzix_mm_newmap(muzix_mm_service_t *mm, uint8_t proc_slot, uint16_t map_ptr)
{
    struct {
        uint8_t proc_slot;
        uint16_t map_ptr;
    } msg;
    int result;
    
    if (!mm) return -1;
    
    msg.proc_slot = proc_slot;
    msg.map_ptr = map_ptr;
    
    result = mm_ipc_call(mm, MUZIX_SVC_NEWMAP, &msg, sizeof(msg));
    return result;
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
    struct {
        uint8_t src_proc;
        uint8_t src_seg;
        uint16_t src_vir;
        uint8_t dst_proc;
        uint8_t dst_seg;
        uint16_t dst_vir;
        uint16_t byte_count;
    } msg;
    int result;
    
    if (!mm) return -1;
    
    msg.src_proc = src_proc;
    msg.src_seg = src_seg;
    msg.src_vir = src_vir;
    msg.dst_proc = dst_proc;
    msg.dst_seg = dst_seg;
    msg.dst_vir = dst_vir;
    msg.byte_count = byte_count;
    
    result = mm_ipc_call(mm, MUZIX_SVC_COPY, &msg, sizeof(msg));
    return result;
}

int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm,
                                  uint8_t dst_proc,
                                  uint16_t dst_vir,
                                  const void *kernel_buf,
                                  uint16_t byte_count)
{
    struct {
        uint8_t dst_proc;
        uint16_t dst_vir;
        uint16_t byte_count;
        const void *kernel_buf;
    } msg;
    int result;
    
    if (!mm || !kernel_buf) return -1;
    
    msg.dst_proc = dst_proc;
    msg.dst_vir = dst_vir;
    msg.byte_count = byte_count;
    msg.kernel_buf = kernel_buf;
    
    result = mm_ipc_call(mm, MUZIX_SVC_COPY_K2U, &msg, sizeof(msg));
    return result;
}

int muzix_mm_load_map(muzix_mm_service_t *mm,
                      uint8_t proc,
                      const process_map_t *map)
{
    struct {
        uint8_t proc;
        process_map_t map;
    } msg;
    int result;
    
    if (!mm || !map) return -1;
    
    msg.proc = proc;
    msg.map = *map;
    
    result = mm_ipc_call(mm, MUZIX_SVC_LOAD_MAP, &msg, sizeof(msg));
    return result;
}

int muzix_mm_load_kernel_map(muzix_mm_service_t *mm,
                             const process_map_t *map)
{
    struct {
        process_map_t map;
    } msg;
    int result;
    
    if (!mm || !map) return -1;
    
    msg.map = *map;
    
    result = mm_ipc_call(mm, MUZIX_SVC_LOAD_KERNEL_MAP, &msg, sizeof(msg));
    return result;
}

int muzix_mm_cross_bank_call(muzix_mm_service_t *mm,
                             const process_map_t *target_map,
                             int (*func)(void *),
                             void *arg)
{
    (void)mm;
    (void)target_map;
    (void)func;
    (void)arg;
    return -1;
}

int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t page,
                                  uint16_t offset,
                                  void *dst,
                                  uint16_t len)
{
    struct {
        uint8_t page;
        uint16_t offset;
        uint16_t len;
        void *dst;
    } msg;
    int result;
    
    if (!mm || !dst) return -1;
    
    msg.page = page;
    msg.offset = offset;
    msg.len = len;
    msg.dst = dst;
    
    result = mm_ipc_call(mm, MUZIX_SVC_COPY_U2K, &msg, sizeof(msg));
    return result;
}

int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm,
                                  uint8_t src_proc,
                                  uint16_t src_vir,
                                  void *kernel_buf,
                                  uint16_t len)
{
    struct {
        uint8_t src_proc;
        uint16_t src_vir;
        uint16_t len;
        void *kernel_buf;
    } msg;
    int result;
    
    if (!mm || !kernel_buf) return -1;
    
    msg.src_proc = src_proc;
    msg.src_vir = src_vir;
    msg.len = len;
    msg.kernel_buf = kernel_buf;
    
    result = mm_ipc_call(mm, MUZIX_SVC_COPY_U2K, &msg, sizeof(msg));
    return result;
}

int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm,
                                      uint8_t page,
                                      uint16_t offset,
                                      void *dst,
                                      uint16_t len)
{
    struct {
        uint8_t page;
        uint16_t offset;
        uint16_t len;
        void *dst;
    } msg;
    int result;
    
    if (!mm || !dst) return -1;
    
    msg.page = page;
    msg.offset = offset;
    msg.len = len;
    msg.dst = dst;
    
    result = mm_ipc_call(mm, MUZIX_SVC_COPY_ROM, &msg, sizeof(msg));
    return result;
}

void muzix_mm_refresh_visible(muzix_mm_service_t *mm)
{
    if (!mm) return;
    mm_ipc_call(mm, MUZIX_SVC_REFRESH, NULL, 0);
}

void *muzix_mm_service_create(void)
{
    return 0;
}

void muzix_mm_service_destroy(void *mm)
{
    (void)mm;
}