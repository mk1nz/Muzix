#include "socket.h"
#include "syscall.h"
#include "socket_priv.h"

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
