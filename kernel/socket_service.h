#ifndef MUZIX_SOCKET_SERVICE_H
#define MUZIX_SOCKET_SERVICE_H

#include "syscalls.h"

int muzix_handle_netctl(muzix_userspace_syscall_context_t *ctx,
                        int sock, int op, uint16_t len, uint16_t data_ptr);
int muzix_handle_netstat(muzix_userspace_syscall_context_t *ctx,
                         int sock, int what, uint16_t data_ptr);
int muzix_handle_netinfo(muzix_userspace_syscall_context_t *ctx,
                         uint16_t data_ptr);

int muzix_w5100_ensure_init(void);

#endif
