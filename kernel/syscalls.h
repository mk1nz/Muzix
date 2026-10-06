#ifndef MUZIX_SYSCALLS_H
#define MUZIX_SYSCALLS_H

#include <stdint.h>

#include "../mm/mm_copy.h"
#include "kernel_loop.h"
#include "proc_table.h"
#include "../mm/mm_service.h"
#include "../platform/zeta-v2/rtc_ds1302.h"

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
int muzix_sys_copy_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);
int muzix_sys_yield_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);
int muzix_sys_exit_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);
int32_t muzix_handle_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3);

#endif
