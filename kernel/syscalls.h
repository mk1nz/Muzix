/* * * Kernel syscall dispatch interface: internal syscall numbers,
 * syscall context structures, and handler declarations.
 * * Two numbering schemes, one router. The kernel-internal
 * MUZIX_SYS_* numbers (0, 1, 10, 11) are a separate namespace
 * from the user numbers (1..60 with gaps) defined in
 * lib/syscall.h. They collide numerically - MUZIX_SYS_COPY=1
 * is user SYS_FORK, MUZIX_SYS_YIELD=10 is user SYS_OPEN,
 * MUZIX_SYS_EXIT=11 is user SYS_CLOSE - but they are
 * disambiguated by dispatch order in muzix_syscall_to_server()
 * (kernel/syscalls_ipc.c): internal numbers are matched before
 * server ranges. Do not assume one number means the same call
 * on both paths.
 */

#ifndef MUZIX_SYSCALLS_H
#define MUZIX_SYSCALLS_H

#include "ztypes.h"

#include "../mm/mm_copy.h"
#include "kernel_loop.h"
#include "proc_table.h"
#include "../mm/mm_service.h"
#include "../platform/zeta-v2/rtc_ds1302.h"
#include "../include/muzix_ipc.h"

/* ------------------------------------------------------------------------- */
/* Compile-time gate: MUZIX_MESSAGE_DISPATCH */
/* ------------------------------------------------------------------------- */

/* * When defined (e.g. `make rom MUZIX_MESSAGE_DISPATCH=1`),
 * the kernel includes a message-routing layer compiled as
 * static inline logic so SDCC can eliminate it when
 * vm_base == 0 at runtime.
 * * At runtime, ctx->vm_base selects the path:
 * vm_base == 0 -> inline dispatch (existing, default, zero overhead)
 * vm_base != 0 -> message dispatch (microkernel IPC path)
 * * When undefined, the kernel dispatches exactly as before.
 */
#ifdef MUZIX_MESSAGE_DISPATCH
#define MUZIX_MSG_DISPATCH 1
#else
#define MUZIX_MSG_DISPATCH 0
#endif

/* ------------------------------------------------------------------------- */
/* Message routing endpoint identifiers */
/* ------------------------------------------------------------------------- */

/* No endpoint. */
#define MUZIX_MSG_ENDPOINT_NONE 0
/* Filesystem server endpoint. */
#define MUZIX_MSG_ENDPOINT_FS 1

/* ------------------------------------------------------------------------- */
/* Kernel-internal syscall numbers */
/* ------------------------------------------------------------------------- */

/* No-op. */
#define MUZIX_SYS_NULL 0
/* Copy between processes. */
#define MUZIX_SYS_COPY 1
/* Re-queue current process. */
#define MUZIX_SYS_YIELD 10
/* Exit process. */
#define MUZIX_SYS_EXIT 11

/* ------------------------------------------------------------------------- */
/* Userspace syscall numbers (kernel-internal view) */
/* ------------------------------------------------------------------------- */

/* Exit process. */
#define MUZIX_USER_SYS_EXIT 3
/* Create new process. */
#define MUZIX_USER_SYS_FORK 1
/* Execute program. */
#define MUZIX_USER_SYS_EXEC 2
/* Wait for child. */
#define MUZIX_USER_SYS_WAIT 4
/* Get process ID. */
#define MUZIX_USER_SYS_GETPID 5
/* Get parent PID. */
#define MUZIX_USER_SYS_GETPPID 6
/* Seconds since the epoch. */
#define MUZIX_USER_SYS_TIME 7
/* Process time accounting. */
#define MUZIX_USER_SYS_TIMES 8
/* Read the DS1302. */
#define MUZIX_USER_SYS_GETRTC 9
/* Set the DS1302 clock. */
#define MUZIX_USER_SYS_SETRTC 15
/* Read one slot of the process table. */
#define MUZIX_USER_SYS_PROCTAB 16
/* Open file. */
#define MUZIX_USER_SYS_OPEN 10
/* Close file. */
#define MUZIX_USER_SYS_CLOSE 11
/* Read from file. */
#define MUZIX_USER_SYS_READ 12
/* Write to file. */
#define MUZIX_USER_SYS_WRITE 13
/* Seek in file. */
#define MUZIX_USER_SYS_SEEK 14
/* Create directory. */
#define MUZIX_USER_SYS_MKDIR 20
/* Remove directory. */
#define MUZIX_USER_SYS_RMDIR 21
/* Change directory. */
#define MUZIX_USER_SYS_CHDIR 22
/* Get current directory. */
#define MUZIX_USER_SYS_GETCWD 23
/* Delete file. */
#define MUZIX_USER_SYS_UNLINK 24
/* Rename file. */
#define MUZIX_USER_SYS_RENAME 25
/* File statistics. */
#define MUZIX_USER_SYS_STAT 26
/* File statistics by descriptor. */
#define MUZIX_USER_SYS_FSTAT 27
/* Create and open file. */
#define MUZIX_USER_SYS_CREAT 28
/* Create hard link. */
#define MUZIX_USER_SYS_LINK 29
/* Duplicate file descriptor. */
#define MUZIX_USER_SYS_DUP 41
/* Change file mode. */
#define MUZIX_USER_SYS_CHMOD 32
/* Check file access. */
#define MUZIX_USER_SYS_ACCESS 33
/* Get real user ID. */
#define MUZIX_USER_SYS_GETUID 42
/* Get real group ID. */
#define MUZIX_USER_SYS_GETGID 43
/* Set user ID. */
#define MUZIX_USER_SYS_SETUID 44
/* Set group ID. */
#define MUZIX_USER_SYS_SETGID 45
/* Three busy-fraction windows out; see load.c. */
#define MUZIX_USER_SYS_LOAD 48
/* Give up the processor until a deadline. */
#define MUZIX_USER_SYS_PARK 49
/* Create pipe. */
#define MUZIX_USER_SYS_PIPE 46
/* TTY control. */
#define MUZIX_USER_SYS_IOCTL 47
/* Create device node. */
#define MUZIX_USER_SYS_MKNOD 34
/* Set file creation mask. */
#define MUZIX_USER_SYS_UMASK 60
/* Read a directory entry. */
#define MUZIX_USER_SYS_READDIR 40
/* Sync filesystem. */
#define MUZIX_USER_SYS_SYNC 30
/* Signal handling. */
#define MUZIX_USER_SYS_SIGNAL 31
/* IPC send/receive. */
#define MUZIX_USER_SYS_IPC_SENDREC 50
/* ROM read. */
#define MUZIX_USER_SYS_ROM_READ 61
/* Create/open a network socket. */
#define MUZIX_USER_SYS_SOCKET 56
/* Network control. */
#define MUZIX_USER_SYS_NETCTL 57
/* Query socket status. */
#define MUZIX_USER_SYS_NETSTAT 58
/* Get network info. */
#define MUZIX_USER_SYS_NETINFO 59
/* Get current tick count. */
#define MUZIX_USER_SYS_GETTICKS 61
/* Check pending signals for current process. */
#define MUZIX_USER_SYS_CHECK_SIGNAL 62
/* Get calibrated CPU frequency in MHz x10. */
#define MUZIX_USER_SYS_CPU_MHZ_X10 63

/* ------------------------------------------------------------------------- */
/* Syscall result codes */
/* ------------------------------------------------------------------------- */

/* Syscall succeeded. */
#define MUZIX_SYSCALL_OK 0
/* Syscall failed. */
#define MUZIX_SYSCALL_FAIL 1

/* ------------------------------------------------------------------------- */
/* Syscall structures */
/* ------------------------------------------------------------------------- */

/* * Kernel-internal syscall record.
 */
typedef struct {
 uint8_t call; /*< Syscall number */
 uint8_t pid; /*< Calling process id */
 uint8_t target_pid; /*< Target process id */
 uint16_t arg0; /*< First argument */
 uint16_t arg1; /*< Second argument */
 uint16_t arg2; /*< Third argument */
 const muzix_copy_request_t *copy_request; /*< Copy request, if any */
} muzix_syscall_t;

/* * Syscall dispatch context, wired up by the kernel.
 */
typedef struct {
 muzix_kernel_loop_t *loop; /*< Kernel loop */
 muzix_mm_service_t *mm; /*< MM service */
 muzix_kernel_proc_table_t *process_table; /*< Process table */
} muzix_syscall_context_t;

struct muzix_fs_service;

/* * Exec loader function pointer type.
 */
typedef int (*muzix_exec_loader_t)(const char *path,
 const char *const *argv,
 int slot);

/* * Userspace syscall context, with server links and identity.
 */
typedef struct muzix_userspace_syscall_context {
 muzix_syscall_context_t *kernel; /*< Kernel context */
 struct muzix_fs_service *fs; /*< FS service link */
 int32_t pid; /*< Process id */
 int32_t ppid; /*< Parent process id */
 muzix_exec_loader_t exec_loader; /*< Exec loader function */
 uint16_t uid; /*< Real user id */
 uint16_t gid; /*< Real group id */
 uint16_t euid; /*< Effective user id */
 uint16_t egid; /*< Effective group id */
 uint16_t umask; /*< File creation mask */
 /* * Virtual memory base for message dispatch. 0 (default) means inline
 * dispatch; the kernel calls service functions directly. Non-zero
 * enables the message path: the syscall is packed into a
 * muzix_system_message_t and routed through the endpoint identified by
 * muzix_msg_route(). Always zeroed at runtime init.
 */
 uint16_t vm_base;
} muzix_userspace_syscall_context_t;

/* * Compile-time assertion: the RTC struct crossing the
 * syscall boundary must be 7 bytes. Userspace declares its
 * own struct rtc_time in lib/libc.h with the same layout;
 * if the two drift, the kernel copies seven bytes into the
 * wrong fields.
 */
#define MUZIX_RTC_STRUCT_SIZE ((uint16_t)sizeof(muzix_rtc_time_t))
typedef char muzix_rtc_layout_check[
 (sizeof(muzix_rtc_time_t) == 7u) ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* Syscall handler declarations */
/* ------------------------------------------------------------------------- */

/* * Handle a kernel-internal syscall.
 * \param[in,out] ctx: syscall context
 * call: syscall record
 * 0 on success, negative error code otherwise
 */
int muzix_handle_syscall(muzix_syscall_context_t *ctx,
 const muzix_syscall_t *call);

/* * Handle a userspace syscall.
 * \param[in,out] ctx: userspace syscall context
 * syscall_num: syscall number
 * arg1: first argument
 * arg2: second argument
 * arg3: third argument
 * Syscall result
 */
int32_t muzix_handle_userspace_syscall(
 muzix_userspace_syscall_context_t *ctx,
 int32_t syscall_num,
 int32_t arg1,
 int32_t arg2,
 int32_t arg3);

/* ------------------------------------------------------------------------- */
/* IPC-based syscall dispatcher helpers */
/* ------------------------------------------------------------------------- */

/* * Return the current process slot number.
 * ctx: userspace syscall context
 * Current slot number
 */
int muzix_current_slot(muzix_userspace_syscall_context_t *ctx);

/* * Return the current process entry pointer.
 * ctx: userspace syscall context
 * Process slot pointer
 */
muzix_proc_slot_t *muzix_current_process(muzix_userspace_syscall_context_t *ctx);

/* * Dispatch a userspace syscall through IPC routing.
 * \param[in,out] ctx: userspace syscall context
 * syscall_num: syscall number
 * arg1: first argument
 * arg2: second argument
 * arg3: third argument
 * Syscall result
 */
int32_t muzix_dispatch_userspace_syscall(
 muzix_userspace_syscall_context_t *ctx,
 int32_t syscall_num,
 int32_t arg1,
 int32_t arg2,
 int32_t arg3);

/* ------------------------------------------------------------------------- */
/* User memory copy helpers */
/* ------------------------------------------------------------------------- */

/* * Copy from the current process's userspace memory.
 * \param[in,out] ctx: userspace syscall context
 * address: source userspace address
 * buffer: destination kernel buffer
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 */
int muzix_copy_user_from_current(
 muzix_userspace_syscall_context_t *ctx,
 uint16_t address,
 uint8_t *buffer,
 size_t length);

/* * Copy to the current process's userspace memory.
 * \param[in,out] ctx: userspace syscall context
 * address: destination userspace address
 * buffer: source kernel buffer
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 */
int muzix_copy_user_to_current(
 muzix_userspace_syscall_context_t *ctx,
 uint16_t address,
 const uint8_t *buffer,
 size_t length);

/* ------------------------------------------------------------------------- */
/* Server copy helpers (copy to/from specific process slot) */
/* ------------------------------------------------------------------------- */

/* * Copy to a specific process's userspace memory.
 * \param[in,out] ctx: userspace syscall context
 * target_slot: destination process slot
 * target_addr: destination userspace address
 * buffer: source kernel buffer
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 */
int muzix_copy_to_proc(
 muzix_userspace_syscall_context_t *ctx,
 uint8_t target_slot,
 uint16_t target_addr,
 const uint8_t *buffer,
 uint16_t length);

/* * Copy from a specific process's userspace memory.
 * \param[in,out] ctx: userspace syscall context
 * target_slot: source process slot
 * target_addr: source userspace address
 * buffer: destination kernel buffer
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 */
int muzix_copy_from_proc(
 muzix_userspace_syscall_context_t *ctx,
 uint8_t target_slot,
 uint16_t target_addr,
 uint8_t *buffer,
 uint16_t length);

/* ------------------------------------------------------------------------- */
/* Cross-process copy helpers */
/* ------------------------------------------------------------------------- */

/* * Copy from client userspace to server userspace.
 * \param[in,out] ctx: userspace syscall context
 * client_slot: client process slot
 * client_addr: client userspace address
 * server_slot: server process slot
 * server_addr: server userspace address
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 * Both directions are a two-hop move through kernel staging.
 */
int muzix_copy_client_to_server(
 muzix_userspace_syscall_context_t *ctx,
 uint8_t client_slot,
 uint16_t client_addr,
 uint8_t server_slot,
 uint16_t server_addr,
 uint16_t length);

/* * Copy from server userspace to client userspace.
 * \param[in,out] ctx: userspace syscall context
 * client_slot: client process slot
 * client_addr: client userspace address
 * server_slot: server process slot
 * server_addr: server userspace address
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 * Both directions are a two-hop move through kernel staging.
 */
int muzix_copy_server_to_client(
 muzix_userspace_syscall_context_t *ctx,
 uint8_t client_slot,
 uint16_t client_addr,
 uint8_t server_slot,
 uint16_t server_addr,
 uint16_t length);

/* ------------------------------------------------------------------------- */
/* IPC sendrec / server copy handlers */
/* ------------------------------------------------------------------------- */

/* * Handle a server-to-client copy request.
 * \param[in,out] ctx: userspace syscall context
 * client_slot: client process slot
 * client_addr: client userspace address
 * buffer: source kernel buffer
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 */
int32_t muzix_handle_srv_copy_to_client(
 muzix_userspace_syscall_context_t *ctx,
 int16_t client_slot,
 uint16_t client_addr,
 const uint8_t *buffer,
 uint16_t length);

/* * Handle a client-to-server copy request.
 * \param[in,out] ctx: userspace syscall context
 * client_slot: client process slot
 * client_addr: client userspace address
 * buffer: destination kernel buffer
 * length: number of bytes to copy
 * 0 on success, negative error code otherwise
 */
int32_t muzix_handle_srv_copy_from_client(
 muzix_userspace_syscall_context_t *ctx,
 int16_t client_slot,
 uint16_t client_addr,
 uint8_t *buffer,
 uint16_t length);

#endif /* MUZIX_SYSCALLS_H */
