/*
 * Host test for the W5100 SPI Ethernet model.
 *
 * Drives w5100_model.c via the SPI state machine, simulating the bit-bang
 * protocol that w5100.c speaks on Port C.
 *
 * W5100 SPI protocol (per Wiznet W5100 datasheet section 6.3.2):
 *   Byte 1: Opcode (0xF0=write, 0x0F=read)
 *   Byte 2: Address high [A15-A8]
 *   Byte 3: Address low [A7-A0]
 *   Byte 4+: Data (for reads: dummy byte, MISO returns data simultaneously)
 *
 * Linux driver reference: #define W5100_SPI_WRITE_OPCODE 0xf0
 *                         #define W5100_SPI_READ_OPCODE 0x0f
 */

#include "libc.h"
#include "libc.h"
#include "ztypes.h"
#include "libc.h"

#include "w5100_model.h"
#include "../../lib/socket.h"
#include "../../lib/syscall.h"

static int32_t captured_syscall_num;
static int32_t captured_arg1;
static int32_t captured_arg2;

static int32_t capture_syscall(int32_t syscall_num, int32_t arg1,
                               int32_t arg2, int32_t arg3) {
    (void)arg3;
    captured_syscall_num = syscall_num;
    captured_arg1 = arg1;
    captured_arg2 = arg2;
    return 123;
}

static void spi_cs_high(w5100_state_t *state) {
    w5100_spi_update(state, 0x80);
}

static void spi_cs_low(w5100_state_t *state) {
    w5100_spi_update(state, 0x00);
}

static void spi_send_byte(w5100_state_t *state, uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (byte >> (7 - i)) & 1;
        uint8_t port_c = bit ? 0x40 : 0x00;
        w5100_spi_update(state, port_c);           /* CLK low */
        w5100_spi_update(state, port_c | 0x20);    /* CLK high (rising edge) */
        w5100_spi_update(state, port_c);            /* CLK low (falling edge) */
    }
}

static void spi_write_reg(w5100_state_t *state, uint16_t addr, uint8_t val) {
    spi_cs_low(state);
    spi_send_byte(state, W5100_SPI_WRITE_OPCODE);  /* 0xF0 = write */
    spi_send_byte(state, (uint8_t)(addr >> 8));    /* addr high */
    spi_send_byte(state, (uint8_t)(addr & 0xFF));   /* addr low */
    spi_send_byte(state, val);                       /* data */
    spi_cs_high(state);
}

static uint8_t spi_read_reg(w5100_state_t *state, uint16_t addr) {
    spi_cs_low(state);
    spi_send_byte(state, W5100_SPI_READ_OPCODE);   /* 0x0F = read */
    spi_send_byte(state, (uint8_t)(addr >> 8));     /* addr high */
    spi_send_byte(state, (uint8_t)(addr & 0xFF));    /* addr low */
    spi_send_byte(state, 0x00);                       /* dummy - MISO returns data */
    spi_cs_high(state);
    return state->spi_last_rx;
}

int main(void) {
    w5100_state_t state;

    printf("=== W5100 Model Host Test ===\n");

    muzix_set_syscall_entry(capture_syscall);
    assert(netstat(3, NETSTAT_RX_AVAIL) == 123);
    assert(captured_syscall_num == SYS_NETSTAT);
    assert(captured_arg1 == 0x0300);
    assert(captured_arg2 == NETSTAT_RX_AVAIL);
    muzix_set_syscall_entry(NULL);
    printf("[PASS] NETSTAT encodes socket ID in arg1 high byte\n");

    w5100_model_init(&state);
    assert(state.spi_state == SPI_STATE_IDLE);
    assert(state.spi_cs_state == 1);
    printf("[PASS] w5100_model_init\n");

    w5100_model_reset(&state);
    assert(state.spi_state == SPI_STATE_IDLE);
    printf("[PASS] w5100_model_reset\n");

    spi_cs_high(&state);
    assert(state.spi_state == SPI_STATE_IDLE);
    printf("[PASS] SPI idle with CS high\n");

    spi_cs_low(&state);
    assert(state.spi_cs_state == 0);
    assert(state.spi_state == SPI_STATE_OPCODE);
    printf("[PASS] CS falling edge detected, state=OPCODE\n");

    /* Send W5100 SPI READ opcode (0x0F) */
    spi_send_byte(&state, 0x0F);  /* W5100 SPI READ opcode */
    assert(state.spi_rw == 1);
    assert(state.spi_state == SPI_STATE_ADDR_H);
    printf("[PASS] SPI opcode parsed: READ (0x0F), state=ADDR_H\n");

    spi_send_byte(&state, 0x00);  /* addr high */
    assert(state.spi_state == SPI_STATE_ADDR_L);
    printf("[PASS] SPI addr high received, state=ADDR_L\n");

    spi_send_byte(&state, 0x20);  /* addr low */
    assert(state.spi_state == SPI_STATE_DATA);
    assert(state.spi_addr == 0x0020);
    printf("[PASS] SPI address complete, addr=0x%04X, state=DATA\n", state.spi_addr);

    uint8_t miso = w5100_spi_read_miso(&state);
    printf("[PASS] SPI MISO read returned 0x%02X\n", miso);

    spi_cs_high(&state);
    assert(state.spi_state == SPI_STATE_IDLE);
    printf("[PASS] CS rising edge resets to IDLE\n");

    /* Test register write: MR (0x0000) = 0x5A */
    w5100_model_init(&state);
    spi_write_reg(&state, 0x0000, 0x5A);
    assert(state.regs[0x0000] == 0x5A);
    printf("[PASS] SPI write MR: 0x%02X\n", state.regs[0x0000]);

    /* Test register read: MR should return 0x5A */
    uint8_t val = spi_read_reg(&state, 0x0000);
    assert(val == 0x5A);
    printf("[PASS] SPI read MR: 0x%02X\n", val);

    /* Test socket CONNECT - ESTABLISHED */
    w5100_model_init(&state);
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_OPEN);
    assert(state.socket_sr[0] == W5100_SR_INIT);
    printf("[PASS] Socket OPEN: SR=0x%02X\n", state.socket_sr[0]);

    w5100_model_init(&state);
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_OPEN);
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_CONNECT);
    assert(state.socket_sr[0] == W5100_SR_ESTABLISHED);
    assert(state.mock_data_ready == 1);
    printf("[PASS] Socket CONNECT: SR=0x%02X, data ready=%d\n",
           state.socket_sr[0], state.mock_data_ready);

    /* Test socket SEND - Sn_IR.SENDOK should be set */
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_SEND);
    assert(state.socket_ir[0] & W5100_SN_IR_SENDOK);
    printf("[PASS] Socket SEND: Sn_IR=0x%02X (SENDOK set)\n", state.socket_ir[0]);

    /* Test Sn_IR write-one-to-clear: write SENDOK bit to clear it */
    uint8_t ir_val = spi_read_reg(&state, W5100_Sn_IR(0));
    assert(ir_val & W5100_SN_IR_SENDOK);
    spi_write_reg(&state, W5100_Sn_IR(0), W5100_SN_IR_SENDOK);
    ir_val = spi_read_reg(&state, W5100_Sn_IR(0));
    assert((ir_val & W5100_SN_IR_SENDOK) == 0);
    printf("[PASS] Sn_IR write-one-to-clear: after clear, Ir=0x%02X\n", ir_val);

    /* Test socket CLOSE */
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_CLOSE);
    assert(state.socket_sr[0] == W5100_SR_CLOSED);
    /* Per datasheet: CLOSE resets TX_WR and TX_RR to 0 */
    assert(spi_read_reg(&state, W5100_Sn_TX_WR(0)) == 0x00);
    assert(spi_read_reg(&state, W5100_Sn_TX_WR(0) + 1) == 0x00);
    assert(spi_read_reg(&state, W5100_Sn_TX_RR(0)) == 0x00);
    assert(spi_read_reg(&state, W5100_Sn_TX_RR(0) + 1) == 0x00);
    printf("[PASS] Socket CLOSE: SR=0x%02X, TX_WR=0, TX_RR=0\n", state.socket_sr[0]);

    /* Test socket OPEN: per datasheet, OPEN initializes TX_WR and TX_RR to 0 */
    w5100_model_init(&state);
    for (int sock = 0; sock < W5100_SOCKET_COUNT; sock++) {
        spi_write_reg(&state, W5100_Sn_CR(sock), W5100_CMD_OPEN);
        assert(spi_read_reg(&state, W5100_Sn_SR(sock)) == W5100_SR_INIT);
        assert(spi_read_reg(&state, W5100_Sn_TX_WR(sock)) == 0x00);
        assert(spi_read_reg(&state, W5100_Sn_TX_WR(sock) + 1) == 0x00);
        assert(spi_read_reg(&state, W5100_Sn_TX_RR(sock)) == 0x00);
        assert(spi_read_reg(&state, W5100_Sn_TX_RR(sock) + 1) == 0x00);
    }
    printf("[PASS] Socket OPEN: SR=INIT, TX_WR=0, TX_RR=0\n");

    static const uint8_t response[] = "HTTP/1.0 200 OK\r\n\r\n";
    for (size_t i = 0; i < sizeof(response) - 1; i++) {
        spi_write_reg(&state,
                      W5100_SOCK0_TXBUF + 2 * W5100_SOCKET_BUF_SIZE + (uint16_t)i,
                      response[i]);
    }
    spi_write_reg(&state, W5100_Sn_TX_WR(2), 0);
    spi_write_reg(&state, W5100_Sn_TX_WR(2) + 1,
                  (uint8_t)(sizeof(response) - 1));
    spi_write_reg(&state, W5100_Sn_CR(2), W5100_CMD_SEND);

    /* Check TX_FSR after SEND: with correct W5100 semantics, SEND advances
     * Sn_TX_RR to Sn_TX_WR upon completion, so the TX buffer is fully free again.
     * FSR = 2048 (0x0800). */
    uint8_t fsr_high = spi_read_reg(&state, W5100_Sn_TX_FSR(2));
    uint8_t fsr_low = spi_read_reg(&state, W5100_Sn_TX_FSR(2) + 1);
    printf("TX_FSR high: 0x%02X, low: 0x%02X\n", fsr_high, fsr_low);
    fflush(stdout);
    assert(fsr_high == 0x08);
    assert(fsr_low == 0x00);

    uint8_t captured[32];
    int captured_len = w5100_model_get_sent(&state, 2, captured, sizeof(captured));
    assert(captured_len == (int)sizeof(response) - 1);
    assert(memcmp(captured, response, sizeof(response) - 1) == 0);
    printf("[PASS] Socket 2 SEND captures its TX buffer\n");

    /* Sn_TX_WR: separate high/low byte write with read-back */
    w5100_model_init(&state);
    spi_write_reg(&state, W5100_Sn_TX_WR(0), 0x12);
    assert(spi_read_reg(&state, W5100_Sn_TX_WR(0)) == 0x12);
    assert(spi_read_reg(&state, W5100_Sn_TX_WR(0) + 1) == 0x00);
    spi_write_reg(&state, W5100_Sn_TX_WR(0) + 1, 0x34);
    assert(spi_read_reg(&state, W5100_Sn_TX_WR(0)) == 0x12);
    assert(spi_read_reg(&state, W5100_Sn_TX_WR(0) + 1) == 0x34);
    assert(((uint16_t)spi_read_reg(&state, W5100_Sn_TX_WR(0)) << 8) |
           spi_read_reg(&state, W5100_Sn_TX_WR(0) + 1) == 0x1234);

    /* Sn_TX_RR is read-only on real W5100 - writes are ignored */
    w5100_model_init(&state);
    spi_write_reg(&state, W5100_Sn_TX_RR(0), 0x56);
    spi_write_reg(&state, W5100_Sn_TX_RR(0) + 1, 0x78);
    /* Reads should return 0 since writes are ignored */
    assert(spi_read_reg(&state, W5100_Sn_TX_RR(0)) == 0x00);
    assert(spi_read_reg(&state, W5100_Sn_TX_RR(0) + 1) == 0x00);
    /* Use test hook to set RR for testing */
    w5100_model_test_set_tx_ptrs(&state, 0, 0x1234, 0x5678);
    assert(((uint16_t)spi_read_reg(&state, W5100_Sn_TX_RR(0)) << 8) |
           spi_read_reg(&state, W5100_Sn_TX_RR(0) + 1) == 0x5678);
    printf("[PASS] Sn_TX_WR write/read, Sn_TX_RR read-only\n");

    /* Sn_TX_FSR: write 100 bytes, expect FSR = 2048 - 100 = 1948 */
    w5100_model_init(&state);
    for (int i = 0; i < 100; i++) {
        spi_write_reg(&state, W5100_SOCK0_TXBUF + i, (uint8_t)i);
    }
    spi_write_reg(&state, W5100_Sn_TX_WR(0), 0);
    spi_write_reg(&state, W5100_Sn_TX_WR(0) + 1, 100);
    assert(spi_read_reg(&state, W5100_Sn_TX_FSR(0)) == (uint8_t)((2048 - 100) >> 8));
    assert(spi_read_reg(&state, W5100_Sn_TX_FSR(0) + 1) == (uint8_t)((2048 - 100) & 0xFF));
    printf("[PASS] Sn_TX_FSR = %d after writing 100 bytes\n", 2048 - 100);

    /* Repeated SEND: SENDOK set on each command */
    w5100_model_init(&state);
    for (int i = 0; i < 50; i++) spi_write_reg(&state, W5100_SOCK0_TXBUF + i, (uint8_t)i);
    spi_write_reg(&state, W5100_Sn_TX_WR(0), 0);
    spi_write_reg(&state, W5100_Sn_TX_WR(0) + 1, 50);
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_SEND);
    assert(state.socket_ir[0] & W5100_SN_IR_SENDOK);
    for (int i = 50; i < 100; i++) spi_write_reg(&state, W5100_SOCK0_TXBUF + i, (uint8_t)i);
    spi_write_reg(&state, W5100_Sn_TX_WR(0), 0);
    spi_write_reg(&state, W5100_Sn_TX_WR(0) + 1, 100);
    spi_write_reg(&state, W5100_Sn_CR(0), W5100_CMD_SEND);
    assert(state.socket_ir[0] & W5100_SN_IR_SENDOK);
    printf("[PASS] Repeated SEND sets SENDOK each time\n");

    /* TX buffer wraparound: fill all 2048 bytes, FSR = 0 */
    w5100_model_init(&state);
    for (int i = 0; i < W5100_SOCKET_BUF_SIZE; i++) {
        spi_write_reg(&state, W5100_SOCK0_TXBUF + i, (uint8_t)i);
    }
    spi_write_reg(&state, W5100_Sn_TX_WR(0), 0x08);
    spi_write_reg(&state, W5100_Sn_TX_WR(0) + 1, 0x00);
    assert(spi_read_reg(&state, W5100_Sn_TX_FSR(0)) == 0x00);
    assert(spi_read_reg(&state, W5100_Sn_TX_FSR(0) + 1) == 0x00);
    printf("[PASS] TX buffer wraparound: FSR = 0 when full\n");

    /* Incoming data is delivered to the selected socket and consumed via RX_RD. */
    w5100_model_init(&state);
    static const uint8_t request[] = "GET / HTTP/1.0\r\n\r\n";
    spi_write_reg(&state, W5100_Sn_CR(3), W5100_CMD_OPEN);
    spi_write_reg(&state, W5100_Sn_CR(3), W5100_CMD_LISTEN);
    w5100_model_trigger_incoming(&state, 3, request, sizeof(request) - 1);
    assert(spi_read_reg(&state, W5100_Sn_SR(3)) == W5100_SR_ESTABLISHED);
    assert(spi_read_reg(&state, W5100_Sn_Rx_RSR(3)) == 0);
    assert(spi_read_reg(&state, W5100_Sn_Rx_RSR(3) + 1) ==
           sizeof(request) - 1);
    captured_len = w5100_model_get_incoming(&state, 3, captured, sizeof(captured));
    assert(captured_len == (int)sizeof(request) - 1);
    assert(memcmp(captured, request, sizeof(request) - 1) == 0);
    spi_write_reg(&state, W5100_Sn_Rx_RD(3), 0);
    spi_write_reg(&state, W5100_Sn_Rx_RD(3) + 1,
                  (uint8_t)(sizeof(request) - 1));
    spi_write_reg(&state, W5100_Sn_CR(3), W5100_CMD_RECV);
    assert(spi_read_reg(&state, W5100_Sn_Rx_RSR(3)) == 0);
    assert(spi_read_reg(&state, W5100_Sn_Rx_RSR(3) + 1) == 0);
    printf("[PASS] Socket 3 accepts and consumes independent RX data\n");

    printf("\nAll W5100 model tests PASSED\n");
    return 0;
}
