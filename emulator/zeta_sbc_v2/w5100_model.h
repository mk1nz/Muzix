#ifndef W5100_MODEL_H
#define W5100_MODEL_H

#include "ztypes.h"

#define W5100_SPI_MODE 0

#define W5100_SPI_WRITE_OPCODE  0xF0
#define W5100_SPI_READ_OPCODE   0x0F

#define W5100_MR           0x0000
#define W5100_GATEWAY      0x0001
#define W5100_GAR          0x0001
#define W5100_SUBNET       0x0005
#define W5100_SHAR0        0x0009
#define W5100_PHYCFGR      0x0020
#define W5100_VERSIONR     0x0022

#define W5100_SOCKET_COUNT 4
#define W5100_SOCKET_BASE(s)  (0x0400 + (uint16_t)(s) * 0x0100)
#define W5100_Sn_MR(sock)       (W5100_SOCKET_BASE(sock) + 0x00)
#define W5100_Sn_CR(sock)       (W5100_SOCKET_BASE(sock) + 0x01)
#define W5100_Sn_IR(sock)       (W5100_SOCKET_BASE(sock) + 0x02)
#define W5100_Sn_SR(sock)       (W5100_SOCKET_BASE(sock) + 0x03)
#define W5100_Sn_PORT(sock)     (W5100_SOCKET_BASE(sock) + 0x04)
#define W5100_Sn_DHAR(sock)     (W5100_SOCKET_BASE(sock) + 0x06)
#define W5100_Sn_DIPR(sock)     (W5100_SOCKET_BASE(sock) + 0x0C)
#define W5100_Sn_DPORT(sock)    (W5100_SOCKET_BASE(sock) + 0x10)
#define W5100_Sn_TX_FSR(sock)   (W5100_SOCKET_BASE(sock) + 0x20)
#define W5100_Sn_TX_RR(sock)    (W5100_SOCKET_BASE(sock) + 0x22)
#define W5100_Sn_TX_WR(sock)    (W5100_SOCKET_BASE(sock) + 0x24)
#define W5100_Sn_RX_RSR(sock)   (W5100_SOCKET_BASE(sock) + 0x26)
#define W5100_Sn_Rx_RSR(sock)   (W5100_SOCKET_BASE(sock) + 0x26)
#define W5100_Sn_RX_RD(sock)    (W5100_SOCKET_BASE(sock) + 0x28)
#define W5100_Sn_Rx_RD(sock)    (W5100_SOCKET_BASE(sock) + 0x28)
#define W5100_Sn_RX_WR(sock)    (W5100_SOCKET_BASE(sock) + 0x2A)
#define W5100_Sn_Rx_WR(sock)    (W5100_SOCKET_BASE(sock) + 0x2A)
#define W5100_Sn_Tx_WR(sock)    (W5100_SOCKET_BASE(sock) + 0x24)

#define W5100_SOCK0_TXBUF    0x4000
#define W5100_SOCK0_RXBUF    0x6000
#define W5100_SOCKET_BUF_SIZE 0x0800

#define W5100_CMD_OPEN    0x01
#define W5100_CMD_LISTEN  0x02
#define W5100_CMD_CONNECT 0x04
#define W5100_CMD_DISCON  0x08
#define W5100_CMD_CLOSE   0x10
#define W5100_CMD_SEND    0x20
#define W5100_CMD_RECV    0x40

#define W5100_MR_TCP  0x01
#define W5100_MR_UDP  0x02

#define W5100_SN_IR_SENDOK    0x10
#define W5100_SN_IR_TIMEOUT   0x08
#define W5100_SN_IR_RECV      0x04
#define W5100_SN_IR_DISCON    0x02
#define W5100_SN_IR_CON       0x01

#define W5100_SR_CLOSED       0x00
#define W5100_SR_INIT         0x13
#define W5100_SR_LISTEN       0x14
#define W5100_SR_SYN_SENT     0x15
#define W5100_SR_SYN_RCVD     0x16
#define W5100_SR_ESTABLISHED  0x17
#define W5100_SR_FIN_WAIT     0x18
#define W5100_SR_CLOSING      0x1A
#define W5100_SR_TIME_WAIT    0x1B
#define W5100_SR_CLOSE_WAIT   0x1C
#define W5100_SR_LAST_ACK     0x1D

typedef enum {
    SPI_STATE_IDLE = 0,
    SPI_STATE_OPCODE,     /* Receiving opcode byte (0xF0=write, 0x0F=read) */
    SPI_STATE_ADDR_H,     /* Receiving address high byte */
    SPI_STATE_ADDR_L,     /* Receiving address low byte */
    SPI_STATE_DATA        /* Data transfer phase */
} spi_state_t;

typedef struct {
    uint8_t spi_clk_state;
    uint8_t spi_mosi_state;
    uint8_t spi_cs_state;
    uint8_t spi_prev_clk;
    uint8_t spi_prev_cs;
    uint8_t spi_shift_reg;
    uint8_t spi_tx_reg;
    uint8_t spi_rx_reg;
    uint8_t spi_last_rx;      /* Completed received byte (read MISO) */
    uint8_t spi_bit_count;
    spi_state_t spi_state;
    uint16_t spi_addr;
    uint8_t spi_rw;
    uint8_t spi_byte_done;

    uint8_t regs[0x8000];
    uint8_t socket_sr[4];
    uint8_t socket_ir[4];
    uint8_t socket_mr[4];
    uint16_t socket_port[4];
    uint32_t socket_dip[4];
    uint16_t socket_dport[4];
uint16_t tx_write_ptr[4];
    uint16_t tx_rr_ptr[4];
    uint16_t host_fifo_head[4];  /* Read position in host FIFO */
    uint16_t host_fifo_tail[4];  /* Write position in host FIFO */
    uint16_t host_fifo_len[4];   /* Bytes pending in host FIFO */
    uint16_t tx_drain_delay_ticks[4]; /* Configurable delay for SEND completion (ticks) */
    uint16_t tx_drain_timer[4];       /* Countdown timer for delayed completion */
    uint8_t send_pending[4];          /* SEND command issued but not completed */
    uint16_t tx_sent_wr[4];           /* TX_WR value at SEND issuance */
    uint16_t host_fifo_data[4][2048]; /* Host FIFO for pending SEND data */
    uint16_t rx_write_ptr[4];
    uint16_t rx_read_ptr[4];
    uint8_t mock_data_ready;
    uint16_t mock_data_len;
    /* Incoming injection support */
    uint8_t incoming_enabled;
    uint16_t incoming_listen_port[4]; /* per socket listen port if in listen */
    /* Close-state ticks: number of poll cycles transient close states
     * (FIN_WAIT, TIME_WAIT, LAST_ACK) persist before expiring to CLOSED.
     * 0 = immediate (does not model real W5100 handshake time). */
    uint16_t close_ticks;
    uint16_t close_timer[4]; /* per-socket close-state countdown */
} w5100_state_t;

void w5100_model_init(w5100_state_t *state);
void w5100_model_reset(w5100_state_t *state);
void w5100_spi_update(w5100_state_t *state, uint8_t port_c_val);
uint8_t w5100_spi_read_miso(w5100_state_t *state);

uint8_t w5100_read_reg(w5100_state_t *state);
void w5100_write_reg(w5100_state_t *state, uint8_t val);

/* Test/integration API for incoming connections and buffer inspection */
void w5100_model_trigger_incoming(w5100_state_t *state, int sock, const uint8_t *data, uint16_t len);
int w5100_model_get_incoming(w5100_state_t *state, int sock, uint8_t *buf, uint16_t maxlen);
int w5100_model_get_sent(w5100_state_t *state, int sock, uint8_t *buf, uint16_t maxlen);
void w5100_model_poll_incoming(w5100_state_t *state);
void w5100_model_inject_connection(w5100_state_t *state, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port, const uint8_t *data, uint16_t len);
void w5100_model_set_close_ticks(uint16_t ticks);
/* Test hook: directly set TX pointers for testing wraparound scenarios */
void w5100_model_test_set_tx_ptrs(w5100_state_t *state, int sock, uint16_t tx_wr, uint16_t tx_rr);

#endif
