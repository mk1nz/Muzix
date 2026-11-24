/*
 * syscalls_ipc.h - IPC-based syscall dispatch declarations
 */

#ifndef MUZIX_SYSCALLS_IPC_H
#define MUZIX_SYSCALLS_IPC_H

#include "ztypes.h"
#include "syscalls.h"

/* Global socket manager instance (defined in socket_manager.c). */
extern struct muzix_socket_manager g_muzix_socket_manager;

#endif /* MUZIX_SYSCALLS_IPC_H */
