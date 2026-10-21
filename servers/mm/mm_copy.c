#include "mm_copy.h"
#include "syscall.h"
#include <string.h>

#define MUZIX_ZETA_COPY_STAGING 256

int muzix_mm_copy_maps(muzix_mm_service_t *mm,
                       const muzix_mproc_t *src_mp,
                       int src_seg,
                       uint16_t src_vir,
                       const muzix_mproc_t *dst_mp,
                       int dst_seg,
                       uint16_t dst_vir,
                       size_t bytes)
{
    uint8_t tmp[MUZIX_ZETA_COPY_STAGING];
    process_map_t src_map = {{0, 0, 0, 0}};
    process_map_t dst_map = {{0, 0, 0, 0}};
    size_t offset = 0;
    size_t remaining = bytes;
    int result;

    if (!mm || !src_mp || !dst_mp ||
        !muzix_mproc_range_valid(src_mp, src_seg, src_vir, bytes) ||
        !muzix_mproc_range_valid(dst_mp, dst_seg, dst_vir, bytes)) {
        return MUZIX_COPY_EFAULT;
    }

    if (bytes > sizeof(tmp)) {
        return MUZIX_COPY_EFAULT;
    }

    if ((src_vir >> 14) == 0 || (src_vir >> 14) == 3 ||
        (dst_vir >> 14) == 0 || (dst_vir >> 14) == 3) {
        return MUZIX_COPY_EFAULT;
    }

    if (muzix_mproc_umap(src_mp, src_seg, src_vir, (uint16_t)bytes) == 0 ||
        muzix_mproc_umap(dst_mp, dst_seg, dst_vir, (uint16_t)bytes) == 0) {
        return MUZIX_COPY_EFAULT;
    }

    muzix_mproc_to_zeta_map(src_mp, &src_map);
    muzix_mproc_to_zeta_map(dst_mp, &dst_map);

    while (remaining > 0) {
        size_t chunk = remaining > MUZIX_ZETA_COPY_STAGING ? MUZIX_ZETA_COPY_STAGING : remaining;
        
        /* The kernel refuses SYS_IPC_COPY_TO_USER/FROM_USER from userspace:
         * the "kernel buffer" argument is the caller's own address, and the
         * single-sided primitive would read it under the kernel map.  The
         * SRV pair resolves both sides through the stored IPC client with the
         * map-to-map primitives, which is what a copy on behalf of a client
         * needs.  The general map-to-map path is the kernel's own
         * muzix_mm_copy_maps() in mm/mm_copy.c; this server-side twin serves
         * the single client the kernel has recorded for us. */
        result = sys_ipc_srv_copy_from_client((uint16_t)(src_vir + offset), tmp, chunk);
        if (result != 0) {
            return MUZIX_COPY_EFAULT;
        }
        
        result = sys_ipc_srv_copy_to_client((uint16_t)(dst_vir + offset), tmp, chunk);
        if (result != 0) {
            return MUZIX_COPY_EFAULT;
        }
        
        offset += chunk;
        remaining -= chunk;
    }

    return MUZIX_COPY_OK;
}

int muzix_mm_copy_kernel_to_map(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                const void *buf,
                                uint16_t len)
{
    (void)mm;
    (void)map;
    
    if (!buf || len > MUZIX_ZETA_COPY_STAGING) {
        return -1;
    }
    
    return sys_ipc_srv_copy_to_client(vir, buf, len);
}

int muzix_mm_copy_map_to_kernel(muzix_mm_service_t *mm,
                                const process_map_t *map,
                                uint16_t vir,
                                void *buf,
                                uint16_t len)
{
    (void)mm;
    (void)map;
    
    if (!buf || len > MUZIX_ZETA_COPY_STAGING) {
        return -1;
    }
    
    return sys_ipc_srv_copy_from_client(vir, buf, len);
}