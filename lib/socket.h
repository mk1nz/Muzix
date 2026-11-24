#ifndef MUZIX_SOCKET_H
#define MUZIX_SOCKET_H

#include "ztypes.h"

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define NETSTAT_SR         0
#define NETSTAT_RX_AVAIL   1
#define NETSTAT_SnIR       2
#define NETSTAT_PEER_IP    3
#define NETSTAT_OPEN_COUNT 4
#define NETSTAT_CLOSE_RESULT 5
#define NETSTAT_CLOSE_ITERS 6
#define NETSTAT_CLOSE_FORCED 7
#define NETSTAT_CLOSE_HIST 8
#define NETSTAT_CLOSE_FORCED_COUNT 9

#define W5100_SR_CLOSED      0x00
#define W5100_SR_INIT        0x13
#define W5100_SR_LISTEN      0x14
#define W5100_SR_ESTABLISHED 0x17

int socket(uint8_t proto, uint8_t reserved, uint8_t unused);
int connect(int sock, const uint8_t *ip4, uint16_t port);
int send(int sock, const uint8_t *buf, uint16_t len);
int recv(int sock, uint8_t *buf, uint16_t maxlen);
int net_close(int sock);
int netstat(int sock, int what);
int socket_connect_blocking(int sock, const uint8_t *ip4, uint16_t port);
int listen(uint16_t port);
int net_listen_on_socket(int sock, uint16_t port);
int accept(int sock);

/* Get the peer (client) IP address for an established socket.
 * Returns 4 bytes in ip[] (network byte order) on success, -1 on failure. */
int net_peer_ip(int sock, uint8_t ip[4]);

/* Get current tick count (120 Hz) */
uint16_t sys_getticks(void);

#endif
