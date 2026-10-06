#ifndef MUZIX_MM_COPY_H
#define MUZIX_MM_COPY_H

#include <stddef.h>
#include <stdint.h>

#include "mm_service.h"
#include "mproc_model.h"
#include "../kernel/system_service.h"

#define MUZIX_COPY_OK 0
#define MUZIX_COPY_EFAULT 1

typedef struct {
    uint8_t src_proc;
    uint8_t dst_proc;
    int src_space;
    int dst_space;
    uint16_t src_vir;
    uint16_t dst_vir;
    size_t bytes;
} muzix_copy_request_t;

/* Map-to-map transfer between two processes.  This is the memory manager's
 * job, not the kernel's: it derives both process maps from the mprocs and
 * resolves each (seg, vir) pair through muzix_mproc_umap() before a byte moves.
 * It was kernel/system_copy.c, where it reached the platform copy primitives
 * directly with &mm->zeta - the same zeta_state_t the MM passes, so the
 * primitives' own region 0/3 refusals still applied, but the path was no longer
 * the MM's and nothing in the build said so.  tools/check_memory_manager.rb now
 * derives the memory manager as the only place these primitives may be called. */
int muzix_mm_copy_maps(muzix_mm_service_t *mm,
                       const muzix_mproc_t *src_mp,
                       int src_seg,
                       uint16_t src_vir,
                       const muzix_mproc_t *dst_mp,
                       int dst_seg,
                       uint16_t dst_vir,
                       size_t bytes);

/* Copy a kernel buffer into an already-resolved process map.
 *
 * The map is passed rather than a slot, because the destination is not always
 * a slot's installed map: the fork path hands over a child map whose stack page
 * has not been published to the process table yet, because it is only published
 * if every chunk of the copy succeeds.  Routing that through
 * muzix_mm_copy_kernel_to_user() would mean installing the map first and rolling
 * it back on failure, which is two extra stores to save nothing - the byte
 * movement is identical either way. */
int muzix_mm_copy_kernel_to_map(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                const void *buf,
                                uint16_t len);

/* Copy a process page into a kernel buffer: the other direction of the same
 * window, and the reason the fork path can stage through a buffer the size of
 * the copy primitive's own rather than one the size of a stack.
 *
 * The fork handler has to read the parent's active stack and write it into a
 * child's page, and it cannot do either with a single call: the destination is
 * a map the process table has not published yet, and the two sides are two
 * different windows. Reading the source and writing the destination are two
 * transfers either way, and this is the one that reads.
 *
 * It is also the reason the entry no longer copies the stack itself. The entry
 * runs before the kernel map is installed and is the only point where the
 * parent's page is mapped at all; it used to ldir the whole active stack into
 * muzix_fork_stack_scratch, which is why that buffer had to be 1024 bytes. This
 * reaches the same page from the kernel side, through the primitive that saves
 * and restores the complete bank map, so the buffer only ever has to hold one
 * transfer - MUZIX_ZETA_COPY_STAGING bytes, which is what it is now. */
int muzix_mm_copy_map_to_kernel(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                void *buf,
                                uint16_t len);

/* The service side of muzix_mm_copy_kernel_to_user(),
 * muzix_mm_copy_page_to_kernel() and muzix_mm_copy_user_to_kernel(): the
 * handlers those three entry points dispatch to through the system task.
 *
 * They live here rather than in kernel/system_service.c because they are the
 * memory manager's, and because they are the only code left that programs a bank
 * register on their own - which is why they could not simply call the muzix_mm_*
 * entry points that reach them: those rebuild a message with the same m_type and
 * re-enter the handler, which is how muzix_mm_refresh_visible() used to recurse
 * without bound and run the kernel stack roughly 57 KB down. */
int muzix_mm_handle_copy_kernel_to_user(muzix_system_service_t *svc,
                                        const muzix_system_message_t *msg);
int muzix_mm_handle_copy_page_to_kernel(muzix_system_service_t *svc,
                                        const muzix_system_message_t *msg);
int muzix_mm_handle_copy_user_to_kernel(muzix_system_service_t *svc,
                                        const muzix_system_message_t *msg);

#endif
