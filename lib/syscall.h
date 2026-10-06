/*
 * Muzix Syscall Interface
 * The numbering and ABI are Muzix-specific, not MINIX system calls.
 * 
 * Syscall numbers must match kernel/syscalls.c definitions
 */

#ifndef MUZIX_SYSCALL_H
#define MUZIX_SYSCALL_H

#include <stdint.h>

#include "proc_info.h"

/* Syscall Numbers - Process Management */
#define SYS_FORK      1    /* Create new process */
#define SYS_EXEC      2    /* Execute program */
#define SYS_EXIT      3    /* Exit process */
#define SYS_WAIT      4    /* Wait for child */
#define SYS_GETPID    5    /* Get process ID */
#define SYS_GETPPID   6    /* Get parent PID */
#define SYS_TIME      7    /* Seconds since the epoch, from the DS1302 */
#define SYS_TIMES     8    /* Process time accounting */
#define SYS_GETRTC    9    /* Read the DS1302; arg2 may ask for the boot reading */
#define SYS_SETRTC    15   /* Set the DS1302 clock from a muzix_rtc_time_t */
#define SYS_PROCTAB   16   /* Read one slot of the process table (buf, slot) */

/* Syscall Numbers - File Operations */
#define SYS_OPEN      10   /* Open file */
#define SYS_CLOSE     11   /* Close file */
#define SYS_READ      12   /* Read from file */
#define SYS_WRITE     13   /* Write to file */
#define SYS_SEEK      14   /* Seek in file */

/* Syscall Numbers - Directory Operations */
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

/* Syscall Numbers - System */
#define SYS_SYNC      30   /* Sync filesystem */
#define SYS_SIGNAL    31   /* Signal handling */
#define SYS_READDIR    40  /* Read a directory entry */

/* Message format for IPC (same as kernel) */
struct message {
    int16_t source;        /* Sending process */
    int16_t dest;          /* Destination process */
    int16_t type;          /* Message type */
    
    union {
        int64_t ll;
        int32_t l[2];
        char c[8];
    } param;               /* Message parameters */
};

typedef struct message message_t;

/*
 * Syscall wrapper macro - Generic syscall invocation
 * Z80 doesn't have a hardware syscall instruction,
 * so we use a software interrupt or jump to kernel handler
 */
/*
 * The result comes back through memory, not in registers.
 *
 * _syscall is hand-written assembly, so its return value depends on which of the
 * two 32-bit return conventions the caller applies - and that is not something
 * this build gets right consistently.  Returning the value in DE:HL, the form
 * a plain `-mz80 --opt-code-size` function returning an unsigned long uses, gave
 * sys_read a zero low word; returning it in HL:DE gave 65536.  Either way
 * getchar() saw something that was not 1, returned -1, and the shell read its
 * own prompt as end of file and quit, having already echoed what was typed.
 *
 * _syscall_dispatch calls the trap and then returns syscall_result, which the
 * assembly fills in.  A memory cell has no convention to disagree about.
 */
/*
 * The trap is *called* with the ordinary int32 prototype on purpose.  Declaring
 * it void makes SDCC choose a different parameter layout - with no return value
 * to free up, DE and HL are no longer taken by the first parameter - and
 * lib/user_syscall.s is hand-written against the int32 layout.  The value it
 * leaves in the registers is simply discarded; the result is read from
 * syscall_result.
 */
#define SYSCALL(num, arg1, arg2, arg3) \
    ((void)muzix_syscall_void((num), (intptr_t)(arg1), (intptr_t)(arg2), \
                               (intptr_t)(arg3)), \
     syscall_result)

/* Kernel entry installed by the platform startup code. */
typedef int32_t (*muzix_syscall_entry_t)(int32_t syscall_num,
                                         int32_t arg1,
                                         int32_t arg2,
                                         int32_t arg3);

void muzix_set_syscall_entry(muzix_syscall_entry_t entry);

/* The entry currently installed, or 0 if there is none.  Present so that a host
 * test standing in for the Z80 trap can find out whether one was installed;
 * see test_host/syscall_host.c. */
muzix_syscall_entry_t muzix_get_syscall_entry(void);

/* Low-level syscall dispatch function.  Performs the trap; the result is left in
 * syscall_result as well as being returned in whatever registers this build
 * decides on. */
int32_t _syscall(int32_t syscall_num, int32_t arg1, int32_t arg2, int32_t arg3);

/* The value the last trap returned, as written by lib/user_syscall.s. */
extern int32_t syscall_result;

/* The trap itself.  Its return value is ignored - SYSCALL() reads
 * syscall_result instead - but the prototype is kept int32 so that the
 * parameters arrive in the layout lib/user_syscall.s expects. */
int32_t muzix_syscall_void(int32_t syscall_num, int32_t arg1, int32_t arg2,
                            int32_t arg3);

/*
 * Process Management Syscalls
 */

/* Fork - Create child process */
int32_t sys_fork(void);

/* Exec - Execute program */
int32_t sys_exec(const char *path, const char * const *argv);

/* Exit - Exit process */
void sys_exit(int32_t status);
void _exit(int32_t status);

/* Wait - Wait for child process */
int32_t sys_wait(int32_t *status);

/* Getpid - Get own process ID */
int32_t sys_getpid(void);

/* Getppid - Get parent process ID */
int32_t sys_getppid(void);
int32_t sys_time(int32_t *value);

/* One slot of the process table, as a struct proc_info (see lib/proc_info.h).
 *
 * This is the only call in the system that reports a process other than the
 * caller, so it is what anything that has to enumerate processes is given.
 *
 * `slot` is a slot number, not a pid: slots are handed out in order starting at
 * 0, slot 0 is the kernel, and a free slot comes back with active == 0 rather
 * than as an error.  There are MUZIX_PROC_INFO_SLOTS of them and the kernel
 * refuses anything outside that range, so walking 0..MUZIX_PROC_INFO_SLOTS-1 is
 * safe.
 *
 * SYS_TIMES is the same call for the calling process's own slot and takes just
 * the buffer; it used to answer with a four-counter struct and now answers with
 * this one, which is a superset of it. */
int32_t sys_proctab(struct proc_info *buf, int32_t slot);

/* SYS_LOAD = 48
 *
 * arg1 = uint32_t[3] out: the busy-fraction windows in Q14, for 1, 5 and 15
 * minutes.  48 is in the gap MINIX left between its own assignments and
 * Muzix's; the used numbers are 1..16, 20..34, 40..47 and 60.
 *
 * The kernel folds the interval that has just ended into the three accumulators
 * on the way in, so a caller polling twice a second gets the average over the
 * last two seconds rather than the same stale figure twice.
 */
#define SYS_LOAD 48

int32_t sys_load(uint32_t *windows);
int32_t sys_times(struct proc_info *buf);

/* The on-board DS1302, as a struct rtc_time (see libc.h).  Both take a pointer
 * to a 7-byte struct that the kernel copies in or out; the kernel refuses
 * anything longer than its staging buffer, so these are deliberately small. */
int32_t sys_getrtc(void *value);
int32_t sys_setrtc(const void *value);

/* One read of the chip, and both ends of a difference: the clock now into
 * `now`, and the reading the kernel took at boot into `since`.
 *
 * `since` is what makes uptime answerable, and it is not a second clock - it is
 * the same DS1302, read once at the first exec after power-on and kept.  One
 * call rather than two so that the two readings cannot come from different
 * instants of a chip that is being adjusted underneath the program.
 *
 * The kernel returns -1 if the live read was refused, and also if there is no
 * boot reading to give: a chip that did not answer at boot has no baseline, and
 * one is not invented later.  Neither buffer is then worth reading. */
int32_t sys_getrtc_since(void *now, void *since);
/* Queue a bounded default-action signal (1..16) for a process. */
int32_t sys_signal(int32_t pid, int32_t signal);

/*
 * File Operations Syscalls
 */

/* Open - Open file */
int32_t sys_open(const char *pathname, int32_t flags);

/* Close - Close file */
int32_t sys_close(int32_t fd);

/* Read - Read from file.
 *
 * Returns the number of bytes read, 0 at end of file, MUZIX_READ_AGAIN when a
 * character device has nothing yet (see lib/tty_ioctl.h), and -1 for an error.
 * Only MUZIX_READ_AGAIN is worth retrying, which is why it is not -1.  Prefer
 * read() from lib/libc.h: it parks on MUZIX_READ_AGAIN and returns everything
 * else, so a program that wants to wait can ask rather than spin. */
int32_t sys_read(int32_t fd, void *buf, int32_t count);

#define SYS_PARK 49
/* Give up the processor for `ticks` on the 120 Hz clock - 240 is two seconds -
 * and carry on from the instruction after this call when the deadline arrives.
 *
 * The one call that parks.  It does not return until it does, and while it is away
 * the scheduler stops the CPU if nothing else is runnable, so a process with
 * nothing to do costs nothing instead of spinning.
 *
 * Park from the process and not from inside a system call, and the reason is
 * where the continuation would be.  The kernel knows the user's stack pointer
 * only while the trap is still on the stack, so it can record a resume point
 * for this process - but that point is in the process, not in the kernel, and a
 * park taken inside, say, a read would resume inside the kernel and then have
 * the process's map installed under it.  This call, made from the process, has
 * no such problem: the instruction after it is the process's own. */
int32_t sys_park(int32_t ticks);

/* Write - Write to file */
int32_t sys_write(int32_t fd, const void *buf, int32_t count);

/* Seek - Seek in file */
int32_t sys_seek(int32_t fd, int32_t offset, int32_t whence);

/*
 * Directory Operations Syscalls
 */

/* Mkdir - Create directory */
int32_t sys_mkdir(const char *pathname, int32_t mode);

/* Rmdir - Remove directory */
int32_t sys_rmdir(const char *pathname);

/* Chdir - Change directory */
int32_t sys_chdir(const char *path);

/* Getcwd - Get current working directory */
char *sys_getcwd(char *buf, int32_t size);

/* Unlink - Delete file */
int32_t sys_unlink(const char *pathname);

/* Rename - Rename file */
int32_t sys_rename(const char *oldpath, const char *newpath);

/* Stat - Get file statistics */
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

/*
 * System Syscalls
 */

/* Sync - Synchronize filesystem */
int32_t sys_sync(void);
int32_t sys_readdir(int32_t fd, int32_t index, void *entry);

#endif /* MUZIX_SYSCALL_H */
