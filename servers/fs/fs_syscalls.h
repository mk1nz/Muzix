#ifndef MUZIX_FS_SYSCALLS_H
#define MUZIX_FS_SYSCALLS_H

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers for FS server userspace operations */
#define SYS_OPEN      10
#define SYS_CLOSE     11
#define SYS_READ      12
#define SYS_WRITE     13
#define SYS_SEEK      14
#define SYS_IOCTL     47
#define SYS_PARK      49
#define SYS_IPC_SENDREC 50
#define SYS_IPC_COPY_TO_USER 51
#define SYS_IPC_COPY_FROM_USER 52

/* ROM block read syscall - reads a 512-byte block from the ROM filesystem */
#define SYS_ROM_READ  61

/* ROM filesystem geometry */
#define MUZIX_ROMFS_BASE_OFFSET  0x20000u
#define MUZIX_ROMFS_TOTAL_BYTES  (512u * 1024u - MUZIX_ROMFS_BASE_OFFSET)
#define MUZIX_ROMFS_BLOCK_COUNT  768u

/* Use muzix_server_message_t and its m1_i1/m1_p1 fields from server.h */
/* SYSCALL macro and syscall wrappers come from lib/syscall.h */
#include "../../lib/syscall.h"

/* ROM read syscall wrapper */
static inline int32_t sys_rom_read(uint16_t block, uint8_t *buffer) {
    /* Use SYSCALL macro directly for SYS_ROM_READ */
    return SYSCALL(SYS_ROM_READ, block, buffer, 0);
}

#endif
