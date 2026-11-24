#include "syscall.h"

int32_t
sys_ipc_sendrec(int32_t dest_slot, void *msg)
{
    return SYSCALL(SYS_IPC_SENDREC, dest_slot, msg, 0);
}

int32_t
sys_ipc_send(int32_t dest_slot, void *msg)
{
    return SYSCALL(SYS_IPC_SEND, dest_slot, msg, 0);
}

int32_t
sys_ipc_copy_to_user(uint16_t addr, const void *buf, size_t len)
{
    return SYSCALL(SYS_IPC_COPY_TO_USER, addr, buf, len);
}

int32_t
sys_ipc_copy_from_user(uint16_t addr, void *buf, size_t len)
{
    return SYSCALL(SYS_IPC_COPY_FROM_USER, addr, buf, len);
}

int32_t
sys_ipc_srv_copy_to_client(uint16_t addr, const void *buf, size_t len)
{
    return SYSCALL(SYS_IPC_SRV_COPY_TO_CLIENT, addr, buf, len);
}

int32_t
sys_ipc_srv_copy_from_client(uint16_t addr, void *buf, size_t len)
{
    return SYSCALL(SYS_IPC_SRV_COPY_FROM_CLIENT, addr, buf, len);
}
