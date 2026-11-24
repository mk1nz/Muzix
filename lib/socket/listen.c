#include "socket.h"
#include "syscall.h"
#include "socket_priv.h"

int
listen(uint16_t port)
{
    int sock;
    uint16_t sock_op;
    int rc;

    sock = socket(SOCK_STREAM, 0, 0);
    if (sock < 0) {
        return -1;
    }

    sock_op = (uint16_t)((sock << 8) | NETCTL_OP_LISTEN);
    rc = SYSCALL(SYS_NETCTL, sock_op, port, 0);
    if (rc < 0) {
        net_close(sock);
        return -1;
    }
    return sock;
}

int
net_listen_on_socket(int sock, uint16_t port)
{
    uint16_t sock_op;
    int rc;

    sock_op = (uint16_t)((sock << 8) | NETCTL_OP_LISTEN);
    rc = SYSCALL(SYS_NETCTL, sock_op, port, 0);
    if (rc < 0) {
        net_close(sock);
        return -1;
    }
    return sock;
}
