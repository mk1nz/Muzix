#ifndef MUZIX_ZETA_SYSCALL_RUNTIME_H
#define MUZIX_ZETA_SYSCALL_RUNTIME_H

#include "../../kernel/kernel_loop.h"
#include "../../kernel/syscalls.h"
#include "../../fs/fs_service.h"

#include "syscall_entry.h"

typedef struct muzix_zeta_syscall_runtime {
    muzix_syscall_context_t kernel;
    muzix_userspace_syscall_context_t userspace;
    muzix_tty_device_t tty;
    muzix_fs_service_t *fs;
    uint8_t bound;
} muzix_zeta_syscall_runtime_t;

void muzix_zeta_syscall_runtime_init(
    muzix_zeta_syscall_runtime_t *runtime,
    muzix_kernel_loop_t *loop,
    muzix_fs_service_t *fs);
void muzix_zeta_syscall_runtime_reset(
    muzix_zeta_syscall_runtime_t *runtime);

#endif
