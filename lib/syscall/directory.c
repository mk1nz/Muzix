#include "syscall.h"

int32_t
sys_mkdir(const char *pathname, int32_t mode)
{
    return SYSCALL(SYS_MKDIR, pathname, mode, 0);
}

int32_t
sys_rmdir(const char *pathname)
{
    return SYSCALL(SYS_RMDIR, pathname, 0, 0);
}

int32_t
sys_chdir(const char *path)
{
    return SYSCALL(SYS_CHDIR, path, 0, 0);
}

char *
sys_getcwd(char *buf, int32_t size)
{
    int32_t ret;

    ret = SYSCALL(SYS_GETCWD, buf, size, 0);
    if (ret < 0) {
        return NULL;
    }
    return buf;
}

int32_t
sys_unlink(const char *pathname)
{
    return SYSCALL(SYS_UNLINK, pathname, 0, 0);
}

int32_t
sys_rename(const char *oldpath, const char *newpath)
{
    return SYSCALL(SYS_RENAME, oldpath, newpath, 0);
}
