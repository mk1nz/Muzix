#include "../../lib/libc.h"
/*
 * W5100 Ethernet controller driver for Zeta SBC V2.
 *
 * W5100 SPI Protocol (per Wiznet W5100 datasheet):
 *   Byte 1: [R/W] [A14-A8]  (R/W: 0=write, 1=read, embedded in bit 7)
 *   Byte 2: [A7-A0]
 *   Byte 3+: Data
 *
 * W5100 Register Map (from WIZnet ioLibrary w5100.h):
 *   Common registers (0x0000-0x007F):
 *     0x0000: MR (Mode Register)
 *     0x0001-0x0004: GAR (Gateway Address, 4 bytes)
 *     0x0005-0x0008: SUBR (Subnet Mask, 4 bytes)
 *     0x0009-0x000E: SHAR (Source Hardware Address, 6 bytes)
 *     0x000F-0x0012: SIPR (Source IP Address, 4 bytes)
 *     0x001A: RMSR (Receive Memory Size)
 *     0x001B: TMSR (Transmit Memory Size)
 *
 *   Socket registers — socket N block at 0x0400 + N * 0x0100 (spacing = 0x0100):
 *     +0x00: Sn_MR (Mode)
 *     +0x01: Sn_CR (Command)
 *     +0x02: Sn_IR (Interrupt)
 *     +0x03: Sn_SR (Status)
 *     +0x04-0x05: Sn_PORT (Source Port, 2 bytes)
 *     +0x06-0x0B: Sn_DHAR (Destination MAC, 6 bytes)
 *     +0x0C-0x0F: Sn_DIPR (Destination IP, 4 bytes)
 *     +0x10-0x11: Sn_DPORT (Destination Port, 2 bytes)
 *     +0x12-0x13: Sn_MSSR (Maximum Segment Size, 2 bytes)
 *     +0x14: Sn_PROTO
 *     +0x15: Sn_TOS
 *     +0x16: Sn_TTL
 *     +0x20-0x21: Sn_TX_FSR (TX Free Size, 2 bytes, read-only)
 *     +0x22-0x23: Sn_TX_RD (TX Read Pointer, 2 bytes, read-only)
 *     +0x24-0x25: Sn_TX_WR (TX Write Pointer, 2 bytes, R/W)
 *     +0x26-0x27: Sn_RX_RSR (RX Received Size, 2 bytes, read-only)
 *     +0x28-0x29: Sn_RX_RD (RX Read Pointer, 2 bytes, R/W)
 *     +0x2A-0x2B: Sn_RX_WR (RX Write Pointer, 2 bytes, read-only)
 *
 *   Buffer addresses (flat 16-bit addressing):
 *     TX buffer: 0x4000 + socket * 2048 (with TMSR=0x55, 2KB per socket)
 *     RX buffer: 0x6000 + socket * 2048 (with RMSR=0x55, 2KB per socket)
 *
 *   Sn_CR command values (BIT-ENCODED, not sequential):
 *     OPEN=0x01, LISTEN=0x02, CONNECT=0x04, DISCON=0x08
 *     CLOSE=0x10, SEND=0x20, RECV=0x40
 *
 * Memory constraints:
 *   - Driver state: ~30 bytes in _DATA (fits in ~5KB free)
 *   - No 32-bit multiply (all addresses are 16-bit, no checksums needed)
 */
/* host-test-not-portable: talks to W5100 via SPI (z80_outb()/z80_inb()) and
 * reads the kernel tick via muzix_tick_now()/muzix_tick_expired(); neither
 * exists on a host. */
#include "w5100.h"
#include "spi_io.h"
#include "tick.h"
#include "trace.h"
#include "../../lib/i2c.h"

#undef W5100_SPI_CMD_WRITE
#undef W5100_SPI_CMD_READ

/* Common registers */
#define W5100_MR             0x0000
#define W5100_GAR            0x0001
#define W5100_SUBR           0x0005
#define W5100_SHAR           0x0009
#define W5100_SIPR           0x000F
#define W5100_RMSR           0x001A
#define W5100_TMSR           0x001B

/* Socket register block: 0x0400 + sock * 0x0100 (ioLibrary _WIZCHIP_SN_SIZE_ = 0x0100) */
#define W5100_SOCKET_BASE(s)  (0x0400 + (uint16_t)(s) * 0x0100)

/* Socket register offsets within a block */
#define W5100_Sn_MR(sock)       (W5100_SOCKET_BASE(sock) + 0x00)
#define W5100_Sn_CR(sock)       (W5100_SOCKET_BASE(sock) + 0x01)
#define W5100_Sn_IR(sock)       (W5100_SOCKET_BASE(sock) + 0x02)
#define W5100_Sn_SR(sock)       (W5100_SOCKET_BASE(sock) + 0x03)
#define W5100_Sn_TX_FSR(sock)   (W5100_SOCKET_BASE(sock) + 0x20)
#define W5100_Sn_TX_WR(sock)    (W5100_SOCKET_BASE(sock) + 0x24)
#define W5100_Sn_TX_RR(sock)    (W5100_SOCKET_BASE(sock) + 0x22)
#define W5100_Sn_RX_RSR(sock)   (W5100_SOCKET_BASE(sock) + 0x26)
#define W5100_Sn_RX_RD(sock)    (W5100_SOCKET_BASE(sock) + 0x28)
#define W5100_Sn_PORT(sock)     (W5100_SOCKET_BASE(sock) + 0x04)
#define W5100_Sn_DIPR(sock)     (W5100_SOCKET_BASE(sock) + 0x0C)
#define W5100_Sn_DPORT(sock)    (W5100_SOCKET_BASE(sock) + 0x10)

/* Buffer addresses — flat 16-bit addressing (ioLibrary _WIZCHIP_IO_TXBUF_=0x4000) */
#define W5100_SOCKET0_TXBUF   0x4000
#define W5100_SOCKET0_RXBUF   0x6000
#define W5100_SOCKET_BUF_SIZE  0x0800  /* 2KB per socket */

/* Sn_CR command values (bit-encoded, from w5100.h ioLibrary) */
#define W5100_CMD_OPEN    0x01
#define W5100_CMD_LISTEN  0x02
#define W5100_CMD_CONNECT 0x04
#define W5100_CMD_DISCON  0x08
#define W5100_CMD_CLOSE   0x10
#define W5100_CMD_SEND    0x20
#define W5100_CMD_RECV    0x40

/* Sn_IR bit values (write-one-to-clear on W5100)
 * Per datasheet: bit 2=RECV(0x04), bit 3=TIMEOUT(0x08), bit 4=SENDOK(0x10) */
#define W5100_Sn_IR_SEND_OK       0x10
#define W5100_Sn_IR_TIMEOUT       0x08  /* CONNECT/SEND timeout */

/* Socket modes */
#define W5100_MR_TCP  0x01
#define W5100_MR_UDP  0x02

/* Socket status values (from ioLibrary SOCK_* defines) */
#define SOCK_CLOSED       0x00
#define SOCK_INIT         0x13
#define SOCK_LISTEN       0x14
#define SOCK_SYN_SENT     0x15
#define SOCK_SYN_RECV     0x16
#define SOCK_ESTABLISHED  0x17
#define SOCK_FIN_WAIT     0x18
#define SOCK_CLOSING      0x1A
#define SOCK_TIME_WAIT    0x1B
#define SOCK_CLOSE_WAIT   0x1C
#define SOCK_LAST_ACK     0x1D
#define SOCK_UDP          0x22

static uint8_t g_w5100_socket_open;

static uint8_t g_w5100_close_last_result;
static uint8_t g_w5100_close_last_iters;
static uint8_t g_w5100_close_last_forced;

/* Гистограмма итераций до CLOSED в w5100_close().
 * Корзины: [0]=0-9, [1]=10-49, [2]=50-99, [3]=100-254, [4]=255 (таймаут). */
static uint8_t g_w5100_close_hist[5];

static uint8_t g_w5100_close_forced_count;

/* Diagnostic counters for wait-path analysis. Updated directly inside
 * w5100_wait_cmd_done() and w5100_wait_send_done() so they remain valid
 * even if HTTPD is trapped inside one of these busy-wait loops. */
static uint16_t g_w5100_wait_cmd_entries = 0;
static uint16_t g_w5100_wait_cmd_successes = 0;
static uint16_t g_w5100_wait_cmd_timeouts = 0;
static uint16_t g_w5100_wait_send_entries = 0;
static uint16_t g_w5100_wait_send_successes = 0;
static uint16_t g_w5100_wait_send_timeouts = 0;
static uint8_t  g_w5100_last_wait_sock = 0;
static uint8_t  g_w5100_last_wait_cr = 0;
static uint8_t  g_w5100_last_wait_ir = 0;
static uint8_t  g_w5100_last_wait_func = 0; /* 1 = wait_cmd_done, 2 = wait_send_done */

uint8_t w5100_socket_open_count(void) {
    /* Count set bits in g_w5100_socket_open */
    uint8_t v = g_w5100_socket_open;
    uint8_t c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

uint8_t w5100_close_last_result(void) { return g_w5100_close_last_result; }
uint8_t w5100_close_last_iters(void) { return g_w5100_close_last_iters; }
uint8_t w5100_close_last_forced(void) { return g_w5100_close_last_forced; }

uint8_t w5100_close_hist_bucket(uint8_t bucket) {
    if (bucket >= 5) return 0;
    return g_w5100_close_hist[bucket];
}

uint8_t w5100_close_forced_count(void) { return g_w5100_close_forced_count; }

static uint8_t g_w5100_mac[6];
static uint8_t g_w5100_ip[4];

static void
w5100_write_reg(uint16_t addr, uint8_t value) {
    muzix_zeta_spi_cs_low();
    muzix_zeta_spi_tx(W5100_SPI_WRITE_OPCODE);          /* 0xF0 */
    muzix_zeta_spi_tx((uint8_t)(addr >> 8));            /* addr high */
    muzix_zeta_spi_tx((uint8_t)(addr & 0xFF));          /* addr low */
    muzix_zeta_spi_tx(value);                            /* data */
    muzix_zeta_spi_cs_high();
}

static uint8_t
w5100_read_reg(uint16_t addr) {
    uint8_t value;
    muzix_zeta_spi_cs_low();
    muzix_zeta_spi_tx(W5100_SPI_READ_OPCODE);            /* 0x0F */
    muzix_zeta_spi_tx((uint8_t)(addr >> 8));            /* addr high */
    muzix_zeta_spi_tx((uint8_t)(addr & 0xFF));          /* addr low */
    value = muzix_zeta_spi_tx(0x00);                    /* dummy -> receive data */
    muzix_zeta_spi_cs_high();
    return value;
}

static void
w5100_write_buf(uint16_t addr, const uint8_t *buf, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        w5100_write_reg(addr + i, buf[i]);
    }
}

static void
w5100_read_buf(uint16_t addr, uint8_t *buf, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        buf[i] = w5100_read_reg(addr + i);
    }
}

static uint16_t
w5100_read_sn_tx_fsr(int sock) {
    uint16_t fsr1, fsr2;
    fsr1 = (w5100_read_reg(W5100_Sn_TX_FSR(sock)) << 8) |
           w5100_read_reg(W5100_Sn_TX_FSR(sock) + 1);
    fsr2 = (w5100_read_reg(W5100_Sn_TX_FSR(sock)) << 8) |
           w5100_read_reg(W5100_Sn_TX_FSR(sock) + 1);
    return fsr2;
}

static int
w5100_wait_cmd_done(int sock) {
    uint16_t cr_addr = W5100_Sn_CR(sock);
    uint16_t deadline = MUZIX_TICK_DEADLINE(1);  /* 1 second: commands complete in microseconds */

    g_w5100_wait_cmd_entries++;
    g_w5100_last_wait_sock = (uint8_t)sock;
    g_w5100_last_wait_cr = w5100_read_reg(cr_addr);
    g_w5100_last_wait_func = 1;

    while (w5100_read_reg(cr_addr) != 0 && !muzix_tick_expired(deadline)) {
        /* spin until Sn_CR clears or deadline expires */
    }
    if (!muzix_tick_expired(deadline)) {
        g_w5100_wait_cmd_successes++;
        return 0;
    }
    g_w5100_wait_cmd_timeouts++;
    return -1;
}

static int
w5100_wait_send_done(int sock) {
    uint16_t deadline = MUZIX_TICK_DEADLINE(5);  /* 5 seconds: SEND completion < 1 second typically */
    uint8_t ir;

    g_w5100_wait_send_entries++;
    g_w5100_last_wait_sock = (uint8_t)sock;
    g_w5100_last_wait_ir = w5100_read_reg(W5100_Sn_IR(sock));
    g_w5100_last_wait_func = 2;

    while (!muzix_tick_expired(deadline)) {
        ir = w5100_read_reg(W5100_Sn_IR(sock));
        g_w5100_last_wait_ir = ir;
        if (ir & W5100_Sn_IR_SEND_OK) {
            w5100_write_reg(W5100_Sn_IR(sock), W5100_Sn_IR_SEND_OK);
            g_w5100_wait_send_successes++;
            return 0;
        }
        if (ir & W5100_Sn_IR_TIMEOUT) {
            w5100_write_reg(W5100_Sn_IR(sock), W5100_Sn_IR_TIMEOUT);
            g_w5100_wait_send_timeouts++;
            return -1;
        }
    }
    g_w5100_wait_send_timeouts++;
    return -1;
}

int w5100_init(const uint8_t *mac, const uint8_t *ip,
               const uint8_t *subnet, const uint8_t *gateway)
{
    uint8_t i;
    muzix_zeta_spi_init();

    for (i = 0; i < 6; i++) g_w5100_mac[i] = mac[i];
    for (i = 0; i < 4; i++) g_w5100_ip[i] = ip[i];

    w5100_write_reg(W5100_MR, 0x80);
    {
        uint16_t deadline = MUZIX_TICK_DEADLINE(2);  /* 2 seconds: reset completes in milliseconds */
        while ((w5100_read_reg(W5100_MR) & 0x80) != 0 && !muzix_tick_expired(deadline)) {
            /* spin until MR bit 7 (reset) clears or deadline expires */
        }
        if ((w5100_read_reg(W5100_MR) & 0x80) != 0) {
            muzix_zeta_spi_deinit();
            return -1;
        }
    }

    w5100_write_buf(W5100_SHAR, mac, 6);
    w5100_write_buf(W5100_GAR, gateway, 4);
    w5100_write_buf(W5100_SUBR, subnet, 4);
    w5100_write_buf(W5100_SIPR, ip, 4);

    w5100_write_reg(W5100_TMSR, 0x55);
    w5100_write_reg(W5100_RMSR, 0x55);
    w5100_write_reg(W5100_MR, 0x00);

    g_w5100_socket_open = 0;
    return 0;
}

/* Default source port for client sockets (ephemeral range) */

int
w5100_open_socket(uint8_t sock, uint8_t protocol, uint16_t port) {
    uint8_t mr_val = (protocol == W5100_PROTO_TCP) ? W5100_MR_TCP : W5100_MR_UDP;

    w5100_write_reg(W5100_Sn_MR(sock), mr_val);
    w5100_write_reg(W5100_Sn_PORT(sock), (uint8_t)(port >> 8));
    w5100_write_reg(W5100_Sn_PORT(sock) + 1, (uint8_t)port);
    w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_OPEN);
    if (w5100_wait_cmd_done(sock) != 0) return -1;

    {
        uint16_t deadline = MUZIX_TICK_DEADLINE(5);  /* 5 seconds: socket INIT typically < 100 ms */
        while (w5100_read_reg(W5100_Sn_SR(sock)) != SOCK_INIT && !muzix_tick_expired(deadline)) {
            /* spin until socket enters INIT state or deadline expires */
        }
        if (w5100_read_reg(W5100_Sn_SR(sock)) == SOCK_INIT) {
            g_w5100_socket_open |= (1 << sock);
            return 0;
        }
    }

    w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_CLOSE);
    w5100_wait_cmd_done(sock);
    return -1;
}

int
w5100_socket_open(uint8_t protocol) {
    uint8_t sock;
    for (sock = 0; sock < 4; sock++) {
        if (!(g_w5100_socket_open & (1 << sock))) break;
    }
    if (sock >= 4) {
        /* All socket slots are marked in use. Find one that's not
         * ESTABLISHED and not software-owned, then reclaim it. */
        for (sock = 0; sock < 4; sock++) {
            if (g_w5100_socket_open & (1 << sock)) continue; /* Still owned */
            uint8_t sr = w5100_read_reg(W5100_Sn_SR(sock));
            if (sr != W5100_SR_ESTABLISHED) {
                w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_CLOSE);
                w5100_wait_cmd_done(sock);
                break;
            }
        }
        if (sock >= 4) return -1;
    }

    if (w5100_open_socket(sock, protocol, W5100_DEFAULT_SOURCE_PORT) != 0) return -1;

    return sock;
}

int
w5100_listen(int sock, uint16_t port) {

    if (sock < 0 || sock >= 4 ||
        !(g_w5100_socket_open & (1 << sock)) ||
        w5100_read_reg(W5100_Sn_SR(sock)) != SOCK_INIT) {
        return -1;
    }

    w5100_write_reg(W5100_Sn_PORT(sock), (uint8_t)(port >> 8));
    w5100_write_reg(W5100_Sn_PORT(sock) + 1, (uint8_t)port);
    w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_LISTEN);
    if (w5100_wait_cmd_done(sock) != 0) return -1;

    {
        uint16_t deadline = MUZIX_TICK_DEADLINE(5);  /* 5 seconds: socket LISTEN typically < 100 ms */
        while (w5100_read_reg(W5100_Sn_SR(sock)) != SOCK_LISTEN && !muzix_tick_expired(deadline)) {
            /* spin until socket enters LISTEN state or deadline expires */
        }
        if (w5100_read_reg(W5100_Sn_SR(sock)) != SOCK_LISTEN) return -1;
    }

    return 0;
}

int
w5100_connect(int sock, uint16_t remote_port, const uint8_t *remote_ip) {
    w5100_write_buf(W5100_Sn_DIPR(sock), remote_ip, 4);
    w5100_write_reg(W5100_Sn_DPORT(sock), (uint8_t)(remote_port >> 8));
    w5100_write_reg(W5100_Sn_DPORT(sock) + 1, (uint8_t)(remote_port & 0xFF));
    w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_CONNECT);
    if (w5100_wait_cmd_done(sock) != 0) return -1;
    return 0;
}

uint8_t
w5100_get_status(int sock) {
    return w5100_read_reg(W5100_Sn_SR(sock));
}

/* Get the peer (client) IP address from Sn_DIPR for an established socket.
 * Copies 4 bytes into ip[] in network byte order. Returns 0 on success,
 * -1 on failure. */
int w5100_get_peer_ip(int sock, uint8_t ip[4])
{
    uint8_t sr = w5100_read_reg(W5100_Sn_SR(sock));
    if (sr != W5100_SR_ESTABLISHED && sr != W5100_SR_CLOSE_WAIT) return -1;
    /* Read each byte of Sn_DIPR individually using explicit register
     * addresses. The W5100 stores the destination IP in Sn_DIPR[0..3] in
     * network byte order (most significant byte first). */
    uint16_t dipr = W5100_Sn_DIPR(sock);
    ip[0] = w5100_read_reg(dipr + 0);
    ip[1] = w5100_read_reg(dipr + 1);
    ip[2] = w5100_read_reg(dipr + 2);
    ip[3] = w5100_read_reg(dipr + 3);
    return 0;
}

uint8_t
w5100_get_interrupt(int sock) {
    return w5100_read_reg(W5100_Sn_IR(sock));
}

void
w5100_clear_interrupt(int sock, uint8_t mask) {
    w5100_write_reg(W5100_Sn_IR(sock), mask);
}

int
w5100_close(int sock) {
    uint8_t sr = w5100_read_reg(W5100_Sn_SR(sock));

    /* If the socket is already closed, just release the bookkeeping.
     * Issuing CMD_CLOSE on an already-closed socket is unnecessary. */
    if (sr == W5100_SR_CLOSED) {
        g_w5100_socket_open &= ~(1 << sock);
        w5100_clear_interrupt(sock, 0xFF); /* Clear all interrupts */
        return 0;
    }

    /* For sockets in ESTABLISHED or CLOSE_WAIT state, perform a graceful
     * TCP disconnect (CMD_DISCON) which transmits FIN to the remote peer.
     * CMD_CLOSE immediately transitions Sn_SR to SOCK_CLOSED without sending
     * FIN, which leaves the remote peer hanging waiting for EOF forever.
     * For other states (INIT, LISTEN), CMD_CLOSE is appropriate since there
     * is no active TCP connection to tear down gracefully. */
    if (sr == W5100_SR_ESTABLISHED || sr == W5100_SR_CLOSE_WAIT) {
        w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_DISCON);
    } else {
        w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_CLOSE);
    }
    if (w5100_wait_cmd_done(sock) != 0) {
        /* Command did not complete; do not release the socket. */
        return -1;
    }

    /* Poll Sn_SR until CLOSED or timeout.  Sn_CR clearing only means the
     * command was accepted, not that the TCP disconnect completed. */
    {
        uint16_t deadline = MUZIX_TICK_DEADLINE(10);  /* 10 seconds: TCP close handshake can take seconds */
        uint8_t forced = 0;
        while (w5100_read_reg(W5100_Sn_SR(sock)) != W5100_SR_CLOSED && !muzix_tick_expired(deadline)) {
            /* spin until socket reaches CLOSED or deadline expires */
        }
        g_w5100_close_last_iters = 0;  /* iteration count no longer meaningful with time-based deadline */
        if (muzix_tick_expired(deadline)) {
            /* Hardware did not complete the close handshake.  Issue a
             * forced CMD_CLOSE so the W5100 aborts the socket and returns
             * it to CLOSED, rather than leaving it half-open forever. */
            w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_CLOSE);
            (void)w5100_wait_cmd_done(sock);
            {
                uint16_t deadline2 = MUZIX_TICK_DEADLINE(5);  /* 5 seconds: forced close should be quick */
                while (w5100_read_reg(W5100_Sn_SR(sock)) != W5100_SR_CLOSED && !muzix_tick_expired(deadline2)) {
                    /* spin until forced close completes or deadline expires */
                }
            }
            forced = 1;
        }
        g_w5100_close_last_forced = forced;
        if (muzix_tick_expired(deadline)) {
            g_w5100_close_last_result = 0xFF; /* -1 */
            g_w5100_close_hist[4]++;
            g_w5100_close_forced_count++;
            /* Hardware close did not complete.  Keep the ownership bit so the
             * socket is not re-allocated while its hardware state is
             * uncertain.  The caller may retry w5100_close() later. */
            return -1;
        }
        /* Записать в гистограмму */
        {
            uint8_t iters = g_w5100_close_last_iters;
            uint8_t bucket;
            if (iters < 10) bucket = 0;
            else if (iters < 50) bucket = 1;
            else if (iters < 100) bucket = 2;
            else if (iters < 255) bucket = 3;
            else bucket = 4;
            g_w5100_close_hist[bucket]++;
        }
        if (forced) {
            g_w5100_close_forced_count++;
        }
    }

    g_w5100_socket_open &= ~(1 << sock);
    w5100_clear_interrupt(sock, 0xFF); /* Clear all interrupts after close */
    g_w5100_close_last_result = 0;
    return 0;
}

int
w5100_send(int sock, const uint8_t *buf, uint16_t len) {
    uint16_t free_space;
    uint16_t tx_ptr;
    uint16_t buf_size = W5100_SOCKET_BUF_SIZE;
    uint16_t offset;
    uint16_t first_chunk;
    uint16_t tx_base = W5100_SOCKET0_TXBUF + (uint16_t)sock * W5100_SOCKET_BUF_SIZE;

    /* Clear stale SEND_OK and TIMEOUT before new SEND operation */
    w5100_clear_interrupt(sock, W5100_SN_IR_SENDOK | W5100_SN_IR_TIMEOUT);

    free_space = w5100_read_sn_tx_fsr(sock);
    if (free_space < len) len = free_space;
    if (len == 0) return 0;

    tx_ptr = (w5100_read_reg(W5100_Sn_TX_WR(sock)) << 8) |
             w5100_read_reg(W5100_Sn_TX_WR(sock) + 1);

    offset = tx_ptr & (buf_size - 1);
    if (offset + len > buf_size) {
        first_chunk = buf_size - offset;
        w5100_write_buf(tx_base + offset, buf, first_chunk);
        w5100_write_buf(tx_base, buf + first_chunk, len - first_chunk);
    } else {
        w5100_write_buf(tx_base + offset, buf, len);
    }

    tx_ptr += len;
    w5100_write_reg(W5100_Sn_TX_WR(sock), (uint8_t)(tx_ptr >> 8));
    w5100_write_reg(W5100_Sn_TX_WR(sock) + 1, (uint8_t)(tx_ptr & 0xFF));
    w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_SEND);
    if (w5100_wait_cmd_done(sock) != 0) {
        /* CMD_SEND was written but not acknowledged. Socket state unknown.
         * Do NOT rollback TX_WR - data may be in MAC. Force close socket. */
        w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_CLOSE);
        (void)w5100_wait_cmd_done(sock);
        return -1;
    }
    if (w5100_wait_send_done(sock) != 0) {
        /* SEND timeout or error - TX_WR already advanced, data may be in MAC.
         * Do NOT rollback - caller must handle retry/close. */
        return -1;
    }

    return len;
}

static uint16_t
w5100_read_sn_rx_rsr(int sock) {
    uint16_t r1, r2;
    r1 = (w5100_read_reg(W5100_Sn_RX_RSR(sock)) << 8) |
         w5100_read_reg(W5100_Sn_RX_RSR(sock) + 1);
    r2 = (w5100_read_reg(W5100_Sn_RX_RSR(sock)) << 8) |
         w5100_read_reg(W5100_Sn_RX_RSR(sock) + 1);
    return r2;
}

uint16_t
w5100_recv_available(int sock) {
    return w5100_read_sn_rx_rsr(sock);
}

int
w5100_recv(int sock, uint8_t *buf, uint16_t maxlen) {
    uint16_t available = w5100_recv_available(sock);
    uint16_t len = (maxlen < available) ? maxlen : available;
    uint16_t buf_size = W5100_SOCKET_BUF_SIZE;
    uint16_t rx_ptr;
    uint16_t offset;
    uint16_t first_chunk;
    uint16_t rx_base = W5100_SOCKET0_RXBUF + (uint16_t)sock * W5100_SOCKET_BUF_SIZE;

    if (len == 0) {
        /* No data available.  Distinguish "would block" (socket still
         * open, data may arrive later) from EOF (peer closed). */
        uint8_t sr = w5100_read_reg(W5100_Sn_SR(sock));
        if (sr == W5100_SR_ESTABLISHED || sr == W5100_SR_CLOSE_WAIT)
            return -1;  /* Would block: try again later */
        return 0;  /* EOF: peer closed */
    }

    rx_ptr = (w5100_read_reg(W5100_Sn_RX_RD(sock)) << 8) |
             w5100_read_reg(W5100_Sn_RX_RD(sock) + 1);

    offset = rx_ptr & (buf_size - 1);
    if (offset + len > buf_size) {
        first_chunk = buf_size - offset;
        w5100_read_buf(rx_base + offset, buf, first_chunk);
        w5100_read_buf(rx_base, buf + first_chunk, len - first_chunk);
    } else {
        w5100_read_buf(rx_base + offset, buf, len);
    }

    rx_ptr += len;
    w5100_write_reg(W5100_Sn_RX_RD(sock), (uint8_t)(rx_ptr >> 8));
    w5100_write_reg(W5100_Sn_RX_RD(sock) + 1, (uint8_t)(rx_ptr & 0xFF));
    w5100_write_reg(W5100_Sn_CR(sock), W5100_CMD_RECV);
    if (w5100_wait_cmd_done(sock) != 0) return -1;

    return len;
}

void
w5100_get_info(uint8_t *mac, uint8_t *ip) {
    uint8_t i;
    for (i = 0; i < 6; i++) mac[i] = g_w5100_mac[i];
    for (i = 0; i < 4; i++) ip[i] = g_w5100_ip[i];
}
