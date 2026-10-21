#ifndef MUZIX_SYSCALLS_H
#define MUZIX_SYSCALLS_H

#include <stdint.h>

#include "../mm/mm_copy.h"
#include "kernel_loop.h"
#include "proc_table.h"
#include "../mm/mm_service.h"
#include "../platform/zeta-v2/rtc_ds1302.h"
#include "../include/muzix_ipc.h"

/* Compile-time gate: MUZIX_MESSAGE_DISPATCH
 *
 * When defined (e.g. `make rom MUZIX_MESSAGE_DISPATCH=1`), the kernel
 * includes a message-routing layer compiled as static inline logic so
 * SDCC can eliminate it when vm_base == 0 at runtime.  At runtime,
 * ctx->vm_base selects the path:
 *
 *   vm_base == 0   ->  inline dispatch (existing, default)
 *   vm_base != 0   ->  message dispatch (microkernel IPC path)
 *
 * When undefined, the kernel dispatches exactly as before — zero overhead. */
#ifdef MUZIX_MESSAGE_DISPATCH
#define MUZIX_MSG_DISPATCH 1
#else
#define MUZIX_MSG_DISPATCH 0
#endif

/* Message routing endpoint identifiers.  Currently only FS is defined;
 * the kernel-internal endpoints (MM, PM) are reserved for future use. */
#define MUZIX_MSG_ENDPOINT_NONE  0
#define MUZIX_MSG_ENDPOINT_FS    1

#define MUZIX_SYS_NULL 0
#define MUZIX_SYS_COPY 1
#define MUZIX_SYS_YIELD 10
#define MUZIX_SYS_EXIT 11

#define MUZIX_USER_SYS_EXIT 3
#define MUZIX_USER_SYS_FORK 1
#define MUZIX_USER_SYS_EXEC 2
#define MUZIX_USER_SYS_WAIT 4
#define MUZIX_USER_SYS_GETPID 5
#define MUZIX_USER_SYS_GETPPID 6
#define MUZIX_USER_SYS_TIME 7
#define MUZIX_USER_SYS_TIMES 8
#define MUZIX_USER_SYS_GETRTC 9
#define MUZIX_USER_SYS_SETRTC 15
#define MUZIX_USER_SYS_PROCTAB 16
#define MUZIX_USER_SYS_OPEN 10
#define MUZIX_USER_SYS_CLOSE 11
#define MUZIX_USER_SYS_READ 12
#define MUZIX_USER_SYS_WRITE 13
#define MUZIX_USER_SYS_SEEK 14
#define MUZIX_USER_SYS_MKDIR 20
#define MUZIX_USER_SYS_RMDIR 21
#define MUZIX_USER_SYS_CHDIR 22
#define MUZIX_USER_SYS_GETCWD 23
#define MUZIX_USER_SYS_UNLINK 24
#define MUZIX_USER_SYS_RENAME 25
#define MUZIX_USER_SYS_STAT 26
#define MUZIX_USER_SYS_FSTAT 27
#define MUZIX_USER_SYS_CREAT 28
#define MUZIX_USER_SYS_LINK 29
#define MUZIX_USER_SYS_DUP 41
#define MUZIX_USER_SYS_CHMOD 32
#define MUZIX_USER_SYS_ACCESS 33
#define MUZIX_USER_SYS_GETUID 42
#define MUZIX_USER_SYS_GETGID 43
#define MUZIX_USER_SYS_SETUID 44
#define MUZIX_USER_SYS_SETGID 45
#define MUZIX_USER_SYS_LOAD 48   /* three busy-fraction windows out; see load.c */
#define MUZIX_USER_SYS_PARK 49   /* give up the processor until a deadline */
#define MUZIX_USER_SYS_PIPE 46
#define MUZIX_USER_SYS_IOCTL 47
#define MUZIX_USER_SYS_MKNOD 34
#define MUZIX_USER_SYS_UMASK 60
#define MUZIX_USER_SYS_READDIR 40
#define MUZIX_USER_SYS_SYNC 30
#define MUZIX_USER_SYS_SIGNAL 31
#define MUZIX_USER_SYS_IPC_SENDREC 50
#define MUZIX_USER_SYS_ROM_READ 61
 
#define MUZIX_SYSCALL_OK 0
#define MUZIX_SYSCALL_FAIL 1

typedef struct {
    uint8_t call;
    uint8_t pid;
    uint8_t target_pid;
    uint16_t arg0;
    uint16_t arg1;
    uint16_t arg2;
    const muzix_copy_request_t *copy_request;
} muzix_syscall_t;

typedef struct {
    muzix_kernel_loop_t *loop;
    muzix_mm_service_t *mm;
    muzix_kernel_proc_table_t *process_table;
} muzix_syscall_context_t;

struct muzix_fs_service;

typedef int (*muzix_exec_loader_t)(const char *path,
                                  const char *const *argv,
                                  int slot);

typedef struct muzix_userspace_syscall_context {
    muzix_syscall_context_t *kernel;
    struct muzix_fs_service *fs;
    int32_t pid;
    int32_t ppid;
    muzix_exec_loader_t exec_loader;
    uint16_t uid;
    uint16_t gid;
    uint16_t euid;
    uint16_t egid;
    uint16_t umask;
    /* Virtual memory base for message dispatch.  0 (default) means inline
     * dispatch; the kernel calls service functions directly.  Non-zero
     * enables the message path: the syscall is packed into a
     * muzix_system_message_t and routed through the endpoint identified by
     * muzix_msg_route().  A microkernel build would set this to the shared
     * message-page address.  Always zeroed at runtime init; only non-zero
     * when MUZIX_MESSAGE_DISPATCH is compiled in and explicitly configured. */
    uint16_t vm_base;
} muzix_userspace_syscall_context_t;

/* muzix_tms_t - the four counters SYS_TIMES used to answer with - is gone.
 * SYS_TIMES now returns a struct proc_info, which is exactly those four
 * counters with pid, flags, active and ppid in front of them: a superset of
 * what the old struct carried.  The reason it is gone rather than kept is that
 * building one field at a time is what SYS_TIMES spent 225 bytes of _CODE on,
 * more than the whole kernel had spare, and there is only room for one of the
 * two.  struct proc_info's layout is chosen so the kernel hands the front of the
 * slot straight to the copy-out with no per-field code at all - see
 * lib/proc_info.h. */

/*
 * The struct that crosses the RTC syscall boundary is the driver's own
 * muzix_rtc_time_t, used directly rather than copied field by field into a
 * second type here.  The copy that a second struct forces is not free - it is
 * fourteen assignments in each direction, twice - and it is fourteen chances to
 * transpose two fields.
 *
 * Userspace cannot include the driver header, so lib/libc.h declares its own
 * `struct rtc_time` with the same layout.  That pairing is what the check below
 * is for: if the two ever drift apart, the kernel would copy seven bytes into
 * the wrong fields and produce a plausible wrong time rather than a failure.
 */
#define MUZIX_RTC_STRUCT_SIZE ((uint16_t)sizeof(muzix_rtc_time_t))
typedef char muzix_rtc_layout_check[
    (sizeof(muzix_rtc_time_t) == 7u) ? 1 : -1];

int muzix_handle_syscall(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);
int32_t muzix_handle_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3);

/* Functions used by IPC-based syscall dispatcher */
int muzix_current_slot(muzix_userspace_syscall_context_t *ctx);
muzix_proc_slot_t *muzix_current_process(muzix_userspace_syscall_context_t *ctx);
int32_t muzix_dispatch_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3);

/* User memory copy helpers */
int muzix_copy_user_from_current(
    muzix_userspace_syscall_context_t *ctx,
    uint16_t address,
    uint8_t *buffer,
    size_t length);
int muzix_copy_user_to_current(
    muzix_userspace_syscall_context_t *ctx,
    uint16_t address,
    const uint8_t *buffer,
    size_t length);

/* Server copy helpers (copy to/from specific process slot) */
int muzix_copy_to_proc(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t target_slot,
    uint16_t target_addr,
    const uint8_t *buffer,
    uint16_t length);
int muzix_copy_from_proc(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t target_slot,
    uint16_t target_addr,
    uint8_t *buffer,
    uint16_t length);

/*
 * Copy between two userspace processes, by slot.
 *
 * These exist because muzix_copy_from_proc()/muzix_copy_to_proc() take a
 * KERNEL destination, and a server's buffer is not one: while the client's page
 * is installed in the window the address falls in, the "kernel" destination
 * resolves to the client's own page, so the copy writes onto its source and
 * transfers nothing - silently.  A server must use these instead.
 *
 * Both directions are the same two-hop move (source user -> kernel staging ->
 * destination user), so `server_slot` is the same process's slot either way and
 * the two functions differ only in which end is named first.
 */
int muzix_copy_client_to_server(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t client_slot,
    uint16_t client_addr,
    uint8_t server_slot,
    uint16_t server_addr,
    uint16_t length);
int muzix_copy_server_to_client(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t client_slot,
    uint16_t client_addr,
    uint8_t server_slot,
    uint16_t server_addr,
    uint16_t length);

/* IPC sendrec handler */

/* IPC handler for server copy */
int32_t muzix_handle_srv_copy_to_client(
    muzix_userspace_syscall_context_t *ctx,
    int16_t client_slot,
    uint16_t client_addr,
    const uint8_t *buffer,
    uint16_t length);
int32_t muzix_handle_srv_copy_from_client(
    muzix_userspace_syscall_context_t *ctx,
    int16_t client_slot,
    uint16_t client_addr,
    uint8_t *buffer,
    uint16_t length);

#endif
