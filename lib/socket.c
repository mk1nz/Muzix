#include "socket.h"
#include "syscall.h"

#define NETCTL_OP_CONNECT  0
#define NETCTL_OP_SEND     1
#define NETCTL_OP_RECV     2
#define NETCTL_OP_CLOSE    3
#define NETCTL_OP_LISTEN   4

#define NETSTAT_SR         0
#define NETSTAT_RX_AVAIL   1
#define NETSTAT_SnIR       2

#define W5100_SR_ESTABLISHED 0x17
#define W5100_SR_CLOSED      0x00
#define W5100_Sn_IR_TIMEOUT  0x08

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

int
socket_connect_blocking(int sock, const uint8_t *ip4, uint16_t port)
{
    int rc;
    int retries;
    uint8_t sr;
    uint8_t ir;

    rc = connect(sock, ip4, port);
    if (rc == 0) {
        return 0;
    }
    if (rc == -1) {
        net_close(sock);
        return -1;
    }

    retries = 100;
    while (retries > 0) {
        sys_park(36);
        --retries;
        sr = (uint8_t)netstat(sock, NETSTAT_SR);
        ir = (uint8_t)netstat(sock, NETSTAT_SnIR);
        if (sr == W5100_SR_ESTABLISHED) {
            return 0;
        }
        if (ir & W5100_Sn_IR_TIMEOUT) {
            net_close(sock);
            return -1;
        }
        if (sr == W5100_SR_CLOSED) {
            net_close(sock);
            return -1;
        }
    }
    net_close(sock);
    return -1;
}

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

int
accept(int sock)
{
    int retries;
    uint8_t sr;

    retries = 1000;
    while (retries > 0) {
        sys_park(24);
        --retries;
        sr = (uint8_t)netstat(sock, NETSTAT_SR);
        if (sr == W5100_SR_ESTABLISHED) {
            return 0;
        }
        if (sr == W5100_SR_CLOSED || sr == W5100_SR_INIT) {
            net_close(sock);
            return -1;
        }
    }
    net_close(sock);
    return -1;
}

uint16_t
sys_getticks(void)
{
    return SYSCALL(SYS_GETTICKS, 0, 0, 0);
}
