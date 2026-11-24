#include "socket.h"
#include "syscall.h"
#include "socket_priv.h"

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
