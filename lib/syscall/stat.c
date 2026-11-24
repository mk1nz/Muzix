#include "syscall.h"

int32_t
sys_stat(const char *pathname, void *statbuf)
{
    return SYSCALL(SYS_STAT, pathname, statbuf, 0);
}

int32_t
sys_fstat(int32_t fd, void *statbuf)
{
    return SYSCALL(SYS_FSTAT, fd, statbuf, 0);
}

int32_t
sys_creat(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_CREAT, pathname, mode, 0);
}

int32_t
sys_link(const char *oldpath, const char *newpath)
{
    return SYSCALL(SYS_LINK, oldpath, newpath, 0);
}

int32_t
sys_dup(int32_t fd)
{
    return SYSCALL(SYS_DUP, fd, 0, 0);
}

int32_t
sys_chmod(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_CHMOD, pathname, mode, 0);
}

int32_t
sys_access(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_ACCESS, pathname, mode, 0);
}

int32_t
sys_getuid(void)
{
    return SYSCALL(SYS_GETUID, 0, 0, 0);
}

int32_t
sys_getgid(void)
{
    return SYSCALL(SYS_GETGID, 0, 0, 0);
}

int32_t
sys_setuid(int32_t uid)
{
    return SYSCALL(SYS_SETUID, uid, 0, 0);
}

int32_t
sys_setgid(int32_t gid)
{
    return SYSCALL(SYS_SETGID, gid, 0, 0);
}

int32_t
sys_pipe(int32_t fds[2])
{
    return SYSCALL(SYS_PIPE, fds, 0, 0);
}

int32_t
sys_ioctl(int32_t fd, int32_t request, void *argument)
{
    return SYSCALL(SYS_IOCTL, fd, request, argument);
}

int32_t
sys_mknod(const char *pathname, int32_t mode, int32_t device)
{
    return SYSCALL(SYS_MKNOD, pathname, mode, device);
}

int32_t
sys_umask(int32_t mask)
{
    return SYSCALL(SYS_UMASK, mask, 0, 0);
}
