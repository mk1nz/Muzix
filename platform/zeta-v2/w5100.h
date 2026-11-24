#ifndef MUZIX_W5100_H
#define MUZIX_W5100_H

#include "ztypes.h"

#define W5100_PROTO_TCP 1
#define W5100_PROTO_UDP 2

#define W5100_SPI_WRITE_OPCODE  0xF0
#define W5100_SPI_READ_OPCODE   0x0F

#define W5100_SR_CLOSED       0x00
#define W5100_SR_INIT         0x13
#define W5100_SR_LISTEN       0x14
#define W5100_SR_ESTABLISHED  0x17
#define W5100_SR_FIN_WAIT     0x18
#define W5100_SR_CLOSING      0x1A
#define W5100_SR_TIME_WAIT    0x1B
#define W5100_SR_CLOSE_WAIT   0x1C
#define W5100_SR_LAST_ACK     0x1D

#define W5100_SN_IR_SENDOK    0x10
#define W5100_SN_IR_TIMEOUT   0x08
#define W5100_SN_IR_RECV      0x04
#define W5100_SN_IR_DISCON    0x02
#define W5100_SN_IR_CON       0x01

/* Default source port for client sockets (ephemeral range) */
#define W5100_DEFAULT_SOURCE_PORT  0x1F90  /* 8080 decimal */

int w5100_init(const uint8_t *mac, const uint8_t *ip,
               const uint8_t *subnet, const uint8_t *gateway);
int w5100_open_socket(uint8_t sock, uint8_t protocol, uint16_t port);
int w5100_socket_open(uint8_t protocol);
int w5100_listen(int sock, uint16_t port);
int w5100_connect(int sock, uint16_t port, const uint8_t *ip4);
uint8_t w5100_get_status(int sock);
uint8_t w5100_get_interrupt(int sock);
void w5100_clear_interrupt(int sock, uint8_t mask);
int w5100_close(int sock);
uint8_t w5100_socket_open_count(void);
uint8_t w5100_close_last_result(void);
uint8_t w5100_close_last_iters(void);
uint8_t w5100_close_last_forced(void);
uint8_t w5100_close_hist_bucket(uint8_t bucket);
uint8_t w5100_close_forced_count(void);
int w5100_get_peer_ip(int sock, uint8_t ip[4]);
int w5100_send(int sock, const uint8_t *buf, uint16_t len);
uint16_t w5100_recv_available(int sock);
int w5100_recv(int sock, uint8_t *buf, uint16_t maxlen);
void w5100_get_info(uint8_t *mac, uint8_t *ip);

#endif
