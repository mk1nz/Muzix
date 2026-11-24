#include "socket.h"
#include "syscall.h"
#include "socket_priv.h"

int
socket(uint8_t proto, uint8_t reserved, uint8_t unused)
{
    return SYSCALL(SYS_SOCKET, proto, 0, 0);
}

int
connect(int sock, const uint8_t *ip4, uint16_t port)
{
    uint16_t sock_op;

    sock_op = (uint16_t)((sock << 8) | NETCTL_OP_CONNECT);
    return SYSCALL(SYS_NETCTL, sock_op, port, (uint16_t)ip4);
}

int
send(int sock, const uint8_t *buf, uint16_t len)
{
    uint16_t sock_op;

    sock_op = (uint16_t)((sock << 8) | NETCTL_OP_SEND);
    return SYSCALL(SYS_NETCTL, sock_op, len, (uint16_t)buf);
}

int
recv(int sock, uint8_t *buf, uint16_t maxlen)
{
    uint16_t sock_op;

    sock_op = (uint16_t)((sock << 8) | NETCTL_OP_RECV);
    return SYSCALL(SYS_NETCTL, sock_op, maxlen, (uint16_t)buf);
}

int
net_close(int sock)
{
    uint16_t sock_op;

    sock_op = (uint16_t)((sock << 8) | NETCTL_OP_CLOSE);
    return SYSCALL(SYS_NETCTL, sock_op, 0, 0);
}

int
netstat(int sock, int what)
{
    uint16_t sock_id;

    sock_id = (uint16_t)((sock << 8) & 0xFF00);
    return SYSCALL(SYS_NETSTAT, sock_id, what, 0);
}

int
net_peer_ip(int sock, uint8_t ip[4])
{
    uint16_t sock_id;
    int result;

    sock_id = (uint16_t)((sock << 8) & 0xFF00);
    result = SYSCALL(SYS_NETSTAT, sock_id, NETSTAT_PEER_IP, (uint16_t)ip);
    if (result < 0) {
        return -1;
    }
    return 0;
}
