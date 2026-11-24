#ifndef MUZIX_SOCKET_PRIV_H
#define MUZIX_SOCKET_PRIV_H

#include "ztypes.h"

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

/* Forward declarations for cross-module calls within the socket library. */
int socket(uint8_t proto, uint8_t reserved, uint8_t unused);
int connect(int sock, const uint8_t *ip4, uint16_t port);
int net_close(int sock);
int netstat(int sock, int what);

#endif /* MUZIX_SOCKET_PRIV_H */
