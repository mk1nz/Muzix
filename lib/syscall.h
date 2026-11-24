#ifndef MUZIX_SYSCALL_H
#define MUZIX_SYSCALL_H

#include "ztypes.h"

#include "proc_info.h"

/* Muzix Syscall Interface
 * The numbering and ABI are Muzix-specific, not MINIX system calls.
 * Syscall numbers must match kernel/syscalls.c definitions. */

/* ------------------------------------------------------------------ */
/* Syscall Numbers - Process Management                               */
/* ------------------------------------------------------------------ */
#define SYS_FORK      1    /* Create new process */
#define SYS_EXEC      2    /* Execute program */
#define SYS_EXIT      3    /* Exit process */
#define SYS_WAIT      4    /* Wait for child */
#define SYS_GETPID    5    /* Get process ID */
#define SYS_GETPPID   6    /* Get parent PID */
#define SYS_TIME      7    /* Seconds since the epoch, from the DS1302 */
#define SYS_TIMES     8    /* Process time accounting */
#define SYS_GETRTC    9    /* Read the DS1302 */
#define SYS_SETRTC    15   /* Set the DS1302 clock */
#define SYS_PROCTAB   16   /* Read one slot of the process table */

/* ------------------------------------------------------------------ */
/* Syscall Numbers - File Operations                                  */
/* ------------------------------------------------------------------ */
#define SYS_OPEN      10   /* Open file */
#define SYS_CLOSE     11   /* Close file */
#define SYS_READ      12   /* Read from file */
#define SYS_WRITE     13   /* Write to file */
#define SYS_SEEK      14   /* Seek in file */

/* ------------------------------------------------------------------ */
/* Syscall Numbers - Directory Operations                             */
/* ------------------------------------------------------------------ */
#define SYS_MKDIR     20   /* Create directory */
#define SYS_RMDIR     21   /* Remove directory */
#define SYS_CHDIR     22   /* Change directory */
#define SYS_GETCWD    23   /* Get current directory */
#define SYS_UNLINK    24   /* Delete file */
#define SYS_RENAME    25   /* Rename file */
#define SYS_STAT      26   /* File statistics */
#define SYS_FSTAT     27   /* File statistics by descriptor */
#define SYS_CREAT     28   /* Create and open file */
#define SYS_LINK      29   /* Create hard link */
#define SYS_DUP       41   /* Duplicate file descriptor */
#define SYS_CHMOD     32   /* Change file mode */
#define SYS_ACCESS    33   /* Check file access */
#define SYS_GETUID    42   /* Get real user ID */
#define SYS_GETGID    43   /* Get real group ID */
#define SYS_SETUID    44   /* Set user ID */
#define SYS_SETGID    45   /* Set group ID */
#define SYS_PIPE      46   /* Create pipe */
#define SYS_IOCTL     47   /* TTY control */
#define SYS_MKNOD     34   /* Create device node */
#define SYS_UMASK     60   /* Set file creation mask */
#define SYS_READDIR   40   /* Read a directory entry */

/* ------------------------------------------------------------------ */
/* Syscall Numbers - System                                           */
/* ------------------------------------------------------------------ */
#define SYS_SYNC      30   /* Sync filesystem */
#define SYS_SIGNAL    31   /* Signal handling */
#define SYS_LOAD      48   /* Read the three CPU busy-fraction windows */
#define SYS_PARK      49   /* Park for ticks on the 120 Hz clock */
#define SYS_IPC_SENDREC 50 /* IPC send/receive */
#define SYS_IPC_COPY_TO_USER 51  /* Copy kernel to user (current process) */
#define SYS_IPC_COPY_FROM_USER 52 /* Copy user to kernel (current process) */
#define SYS_IPC_SRV_COPY_TO_CLIENT 53  /* Server copy to client */
#define SYS_IPC_SRV_COPY_FROM_CLIENT 54 /* Server copy from client */
#define SYS_IPC_SEND   55   /* IPC send only */
#define SYS_NETINFO    59   /* Get network info */
#define SYS_GETTICKS   61   /* Get current tick count */
#define SYS_CHECK_SIGNAL 62 /* Check pending signals for current process (no delivery) */
#define SYS_CPU_MHZ_X10 63   /* Get calibrated CPU frequency in MHz x10 */

/* ------------------------------------------------------------------ */
/* Socket/Network Syscalls                                            */
/* ------------------------------------------------------------------ */
#define SYS_SOCKET     56   /* Create/open a network socket */
#define SYS_NETCTL     57   /* Network control */
#define SYS_NETSTAT    58   /* Query socket status */

/* ------------------------------------------------------------------ */
/* Message format for IPC                                             */
/* ------------------------------------------------------------------ */
struct message {
    int16_t source;
    int16_t dest;
    int16_t type;
    union {
        int64_t ll;
        int32_t l[2];
        char c[8];
    } param;
};
typedef struct message message_t;

/* ------------------------------------------------------------------ */
/* SYSCALL() macro                                                    */
/* ------------------------------------------------------------------ */
/* Generic syscall invocation.  The result comes back through memory
 * (syscall_result), not in registers, because SDCC's 32-bit return
 * convention disagrees with what the compiled caller reads. */
#define SYSCALL(num, arg1, arg2, arg3) \
    ((void)muzix_syscall_void((num), (intptr_t)(arg1), (intptr_t)(arg2), \
                              (intptr_t)(arg3)), \
     syscall_result)

/* ------------------------------------------------------------------ */
/* Low-level syscall entry                                            */
/* ------------------------------------------------------------------ */
typedef int32_t (*muzix_syscall_entry_t)(int32_t syscall_num,
                                         int32_t arg1,
                                         int32_t arg2,
                                         int32_t arg3);

void muzix_set_syscall_entry(muzix_syscall_entry_t entry);
muzix_syscall_entry_t muzix_get_syscall_entry(void);
int32_t _syscall(int32_t syscall_num, int32_t arg1, int32_t arg2, int32_t arg3);
extern int32_t syscall_result;
int32_t muzix_syscall_void(int32_t syscall_num, int32_t arg1, int32_t arg2,
                           int32_t arg3);

/* ------------------------------------------------------------------ */
/* Process Management Syscalls                                        */
/* ------------------------------------------------------------------ */
int32_t sys_fork(void);
int32_t sys_exec(const char *path, const char * const *argv);
void sys_exit(int32_t status);
void _exit(int32_t status);
int32_t sys_wait(int32_t *status);
int32_t sys_getpid(void);
int32_t sys_getppid(void);
int32_t sys_time(int32_t *value);
int32_t sys_times(struct proc_info *buf);
int32_t sys_proctab(struct proc_info *buf, int32_t slot);
int32_t sys_load(uint32_t *windows);

/* ------------------------------------------------------------------ */
/* DS1302 Syscalls                                                    */
/* ------------------------------------------------------------------ */
int32_t sys_getrtc(void *value);
int32_t sys_setrtc(const void *value);
int32_t sys_getrtc_since(void *now, void *since);
int32_t sys_signal(int32_t pid, int32_t signal);

int32_t sys_check_signal(void);

/* ------------------------------------------------------------------ */
/* File Operations Syscalls                                           */
/* ------------------------------------------------------------------ */
int32_t sys_open(const char *pathname, int32_t flags);
int32_t sys_close(int32_t fd);
int32_t sys_read(int32_t fd, void *buf, int32_t count);
int32_t sys_park(int32_t ticks);
int32_t sys_write(int32_t fd, const void *buf, int32_t count);
int32_t sys_seek(int32_t fd, int32_t offset, int32_t whence);

/* ------------------------------------------------------------------ */
/* Directory Operations Syscalls                                      */
/* ------------------------------------------------------------------ */
int32_t sys_mkdir(const char *pathname, int32_t mode);
int32_t sys_rmdir(const char *pathname);
int32_t sys_chdir(const char *path);
char *sys_getcwd(char *buf, int32_t size);
int32_t sys_unlink(const char *pathname);
int32_t sys_rename(const char *oldpath, const char *newpath);
int32_t sys_stat(const char *pathname, void *statbuf);
int32_t sys_fstat(int32_t fd, void *statbuf);
int32_t sys_creat(const char *pathname, int32_t mode);
int32_t sys_link(const char *oldpath, const char *newpath);
int32_t sys_dup(int32_t fd);
int32_t sys_chmod(const char *pathname, int32_t mode);
int32_t sys_access(const char *pathname, int32_t mode);
int32_t sys_getuid(void);
int32_t sys_getgid(void);
int32_t sys_setuid(int32_t uid);
int32_t sys_setgid(int32_t gid);
int32_t sys_pipe(int32_t fds[2]);
int32_t sys_ioctl(int32_t fd, int32_t request, void *argument);
int32_t sys_mknod(const char *pathname, int32_t mode, int32_t device);
int32_t sys_umask(int32_t mask);

/* ------------------------------------------------------------------ */
/* System Syscalls                                                    */
/* ------------------------------------------------------------------ */
int32_t sys_sync(void);
int32_t sys_readdir(int32_t fd, int32_t index, void *entry);
int32_t sys_ipc_sendrec(int32_t dest_slot, void *msg);
int32_t sys_ipc_send(int32_t dest_slot, void *msg);
int32_t sys_ipc_copy_to_user(uint16_t addr, const void *buf, size_t len);
int32_t sys_ipc_copy_from_user(uint16_t addr, void *buf, size_t len);
int32_t sys_ipc_srv_copy_to_client(uint16_t addr, const void *buf, size_t len);
int32_t sys_ipc_srv_copy_from_client(uint16_t addr, void *buf, size_t len);
int32_t sys_cpu_mhz_x10(void);

#endif /* MUZIX_SYSCALL_H */
