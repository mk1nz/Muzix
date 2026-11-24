#include "syscall.h"

int32_t
sys_time(int32_t *value)
{
    return SYSCALL(SYS_TIME, value, 0, 0);
}

int32_t
sys_times(struct proc_info *value)
{
    return SYSCALL(SYS_TIMES, value, 0, 0);
}

int32_t
sys_proctab(struct proc_info *value, int32_t slot)
{
    return SYSCALL(SYS_PROCTAB, value, slot, 0);
}

int32_t
sys_load(uint32_t *windows)
{
    return SYSCALL(SYS_LOAD, windows, 0, 0);
}

int32_t
sys_getrtc(void *value)
{
    return SYSCALL(SYS_GETRTC, value, 0, 0);
}

int32_t
sys_getrtc_since(void *now, void *since)
{
    return SYSCALL(SYS_GETRTC, now, since, 0);
}

int32_t
sys_setrtc(const void *value)
{
    return SYSCALL(SYS_SETRTC, value, 0, 0);
}

int32_t
sys_signal(int32_t pid, int32_t signal)
{
    return SYSCALL(SYS_SIGNAL, pid, signal, 0);
}
