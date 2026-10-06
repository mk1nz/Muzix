/*
 * Muzix userspace syscall wrappers (Muzix-specific ABI)
 * 
 * These functions provide the C-level interface to kernel syscalls
 */

#include "syscall.h"

#include <stdint.h>
#include <string.h>

static muzix_syscall_entry_t syscall_entry;

void muzix_set_syscall_entry(muzix_syscall_entry_t entry)
{
    syscall_entry = entry;
}

/* The entry currently installed, or 0 if there is none.
 *
 * Added for test_host/syscall_host.c, which has to stand in for the trap in
 * lib/syscall.asm and so has to know who to call.  `syscall_entry` is static,
 * and the alternative - the shim defining muzix_set_syscall_entry() as well -
 * collides with the one above, which stops the host runner linking this whole
 * module and takes every sys_* wrapper with it.  A getter is two instructions
 * of userspace code and changes no behaviour. */
muzix_syscall_entry_t muzix_get_syscall_entry(void)
{
    return syscall_entry;
}

/* Process Management Syscalls */

int32_t sys_fork(void)
{
    return SYSCALL(SYS_FORK, 0, 0, 0);
}

int32_t sys_exec(const char *path, const char * const *argv)
{
    return SYSCALL(SYS_EXEC, path, argv, 0);
}

void sys_exit(int32_t status)
{
    SYSCALL(SYS_EXIT, status, 0, 0);
    /* Never returns */
    while (1);
}

void _exit(int32_t status)
{
    sys_exit(status);
}

int32_t sys_wait(int32_t *status)
{
    return SYSCALL(SYS_WAIT, status, 0, 0);
}

int32_t sys_getpid(void)
{
    return SYSCALL(SYS_GETPID, 0, 0, 0);
}

int32_t sys_getppid(void)
{
    return SYSCALL(SYS_GETPPID, 0, 0, 0);
}

int32_t sys_time(int32_t *value)
{
    return SYSCALL(SYS_TIME, value, 0, 0);
}

int32_t sys_times(struct proc_info *value)
{
    return SYSCALL(SYS_TIMES, value, 0, 0);
}

int32_t sys_proctab(struct proc_info *value, int32_t slot)
{
    return SYSCALL(SYS_PROCTAB, value, slot, 0);
}

int32_t sys_load(uint32_t *windows)
{
    return SYSCALL(SYS_LOAD, windows, 0, 0);
}

int32_t sys_getrtc(void *value)
{
    return SYSCALL(SYS_GETRTC, value, 0, 0);
}

int32_t sys_getrtc_since(void *now, void *since)
{
    return SYSCALL(SYS_GETRTC, now, since, 0);
}

int32_t sys_setrtc(const void *value)
{
    return SYSCALL(SYS_SETRTC, value, 0, 0);
}

int32_t sys_signal(int32_t pid, int32_t signal)
{
    return SYSCALL(SYS_SIGNAL, pid, signal, 0);
}

/* File Operations Syscalls */

int32_t sys_open(const char *pathname, int32_t flags)
{
    return SYSCALL(SYS_OPEN, pathname, flags, 0);
}

int32_t sys_close(int32_t fd)
{
    return SYSCALL(SYS_CLOSE, fd, 0, 0);
}

int32_t sys_read(int32_t fd, void *buf, int32_t count)
{
    return SYSCALL(SYS_READ, fd, buf, count);
}

/* Park, then come back.  Returns only when the deadline does; the argument is a
 * tick count on the 120 Hz clock, and 240 is two seconds. */
int32_t sys_park(int32_t ticks)
{
    return SYSCALL(SYS_PARK, ticks, 0, 0);
}

int32_t sys_write(int32_t fd, const void *buf, int32_t count)
{
    return SYSCALL(SYS_WRITE, fd, buf, count);
}

int32_t sys_seek(int32_t fd, int32_t offset, int32_t whence)
{
    return SYSCALL(SYS_SEEK, fd, offset, whence);
}

/* Directory Operations Syscalls */

int32_t sys_mkdir(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_MKDIR, pathname, mode, 0);
}

int32_t sys_rmdir(const char *pathname)
{
    return SYSCALL(SYS_RMDIR, pathname, 0, 0);
}

int32_t sys_chdir(const char *path)
{
    return SYSCALL(SYS_CHDIR, path, 0, 0);
}

char *sys_getcwd(char *buf, int32_t size)
{
    int32_t ret = SYSCALL(SYS_GETCWD, buf, size, 0);
    if (ret < 0) {
        return NULL;
    }
    return buf;
}

int32_t sys_unlink(const char *pathname)
{
    return SYSCALL(SYS_UNLINK, pathname, 0, 0);
}

int32_t sys_rename(const char *oldpath, const char *newpath)
{
    return SYSCALL(SYS_RENAME, oldpath, newpath, 0);
}

int32_t sys_stat(const char *pathname, void *statbuf)
{
    return SYSCALL(SYS_STAT, pathname, statbuf, 0);
}

int32_t sys_fstat(int32_t fd, void *statbuf)
{
    return SYSCALL(SYS_FSTAT, fd, statbuf, 0);
}

int32_t sys_creat(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_CREAT, pathname, mode, 0);
}

int32_t sys_link(const char *oldpath, const char *newpath)
{
    return SYSCALL(SYS_LINK, oldpath, newpath, 0);
}

int32_t sys_dup(int32_t fd)
{
    return SYSCALL(SYS_DUP, fd, 0, 0);
}

int32_t sys_chmod(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_CHMOD, pathname, mode, 0);
}

int32_t sys_access(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_ACCESS, pathname, mode, 0);
}

int32_t sys_getuid(void)
{
    return SYSCALL(SYS_GETUID, 0, 0, 0);
}

int32_t sys_getgid(void)
{
    return SYSCALL(SYS_GETGID, 0, 0, 0);
}

int32_t sys_setuid(int32_t uid)
{
    return SYSCALL(SYS_SETUID, uid, 0, 0);
}

int32_t sys_setgid(int32_t gid)
{
    return SYSCALL(SYS_SETGID, gid, 0, 0);
}

int32_t sys_pipe(int32_t fds[2])
{
    return SYSCALL(SYS_PIPE, fds, 0, 0);
}

int32_t sys_ioctl(int32_t fd, int32_t request, void *argument)
{
    return SYSCALL(SYS_IOCTL, fd, request, argument);
}

int32_t sys_mknod(const char *pathname, int32_t mode, int32_t device)
{
    return SYSCALL(SYS_MKNOD, pathname, mode, device);
}

int32_t sys_umask(int32_t mask)
{
    return SYSCALL(SYS_UMASK, mask, 0, 0);
}

/* System Syscalls */

int32_t sys_sync(void)
{
    return SYSCALL(SYS_SYNC, 0, 0, 0);
}

int32_t sys_readdir(int32_t fd, int32_t index, void *entry)
{
    return SYSCALL(SYS_READDIR, fd, index, entry);
}
