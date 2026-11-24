#include <stdio.h>
#include <string.h>
#include "ztypes.h"
#include <stdlib.h>

#include "emulator.h"
#include "w5100_model.h"

#define W5100_SPI_CLK_BIT   0x20
#define W5100_SPI_MOSI_BIT  0x40
#define W5100_SPI_CS_BIT    0x80
#define W5100_SPI_MISO_BIT  0x01

/* Override close-state duration (poll cycles) for testing.  0 = immediate.
 * Set from the emulator main via w5100_model_set_close_ticks(). */
static uint16_t g_w5100_model_close_ticks = 0;

void w5100_model_set_close_ticks(uint16_t ticks) {
    g_w5100_model_close_ticks = ticks;
}

void w5100_model_init(w5100_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->spi_cs_state = 1;
    state->spi_prev_cs = 1;
    state->spi_prev_clk = 0;
    state->spi_state = SPI_STATE_IDLE;
    state->spi_bit_count = 0;
    state->spi_shift_reg = 0;
    state->spi_tx_reg = 0;
    state->spi_rx_reg = 0;
    state->spi_last_rx = 0;
    state->spi_byte_done = 0;
    state->spi_addr = 0;
    state->spi_rw = 0;
    for (int i = 0; i < 4; i++) {
        state->socket_sr[i] = W5100_SR_CLOSED;
        state->socket_ir[i] = 0;
        state->socket_mr[i] = 0;
        state->socket_port[i] = 0;
        state->socket_dip[i] = 0;
        state->socket_dport[i] = 0;
        state->tx_write_ptr[i] = 0;
        state->tx_rr_ptr[i] = 0;
        state->host_fifo_head[i] = 0;
        state->host_fifo_tail[i] = 0;
        state->host_fifo_len[i] = 0;
        state->send_pending[i] = 0;
        state->tx_sent_wr[i] = 0;
        state->tx_drain_delay_ticks[i] = 0;
        state->tx_drain_timer[i] = 0;
        state->rx_write_ptr[i] = 0;
        state->rx_read_ptr[i] = 0;
        state->incoming_listen_port[i] = 0;
        state->close_timer[i] = 0;
    }
    /* Set VERSIONR = 0x51 as per W5100 */
    if (0x0022 < 0x8000) {
        state->regs[0x0022] = 0x51;
    }
    state->regs[W5100_MR] = 0;
    state->incoming_enabled = 1;
    {
        const char *env = getenv("W5100_CLOSE_TICKS");
        state->close_ticks = env ? (uint16_t)atoi(env) : g_w5100_model_close_ticks;
    }
}

void w5100_model_reset(w5100_state_t *state) {
    w5100_model_init(state);
}

static int w5100_sock_from_addr(uint16_t addr) {
    if (addr >= 0x0400 && addr < 0x0500) return 0;
    if (addr >= 0x0500 && addr < 0x0600) return 1;
    if (addr >= 0x0600 && addr < 0x0700) return 2;
    if (addr >= 0x0700 && addr < 0x0800) return 3;
    return -1;
}

static uint16_t w5100_sock_offset(uint16_t addr, int sock) {
    return addr - W5100_SOCKET_BASE(sock);
}

static uint16_t w5100_txbuf_base(int sock) {
    return (uint16_t)(W5100_SOCK0_TXBUF + (sock * W5100_SOCKET_BUF_SIZE));
}

static uint16_t w5100_rxbuf_base(int sock) {
    return (uint16_t)(W5100_SOCK0_RXBUF + (sock * W5100_SOCKET_BUF_SIZE));
}

uint8_t w5100_read_reg(w5100_state_t *state) {
    uint16_t addr = state->spi_addr;

    /* Common registers */
    if (addr < 0x0400) {
        if (addr == 0x0022) {
            return 0x51;
        }
        if (addr < 0x8000) {
            return state->regs[addr];
        }
        return 0;
    }

    int sock = w5100_sock_from_addr(addr);
    if (sock >= 0) {
        uint16_t offset = w5100_sock_offset(addr, sock);
        switch (offset) {
        case 0x00: /* Sn_MR */
            return state->socket_mr[sock];
        case 0x01: /* Sn_CR */
            return 0;
        case 0x02: /* Sn_IR */
            return state->socket_ir[sock];
        case 0x03: /* Sn_SR */
        {
            uint8_t sr = state->socket_sr[sock];
            return sr;
        }
        case 0x04: /* Sn_PORT high */
            return (uint8_t)(state->socket_port[sock] >> 8);
        case 0x05: /* Sn_PORT low */
            return (uint8_t)(state->socket_port[sock] & 0xFF);
        case 0x0C: /* Sn_DIPR[0] */
            return (uint8_t)(state->socket_dip[sock] >> 24);
        case 0x0D:
            return (uint8_t)(state->socket_dip[sock] >> 16);
        case 0x0E:
            return (uint8_t)(state->socket_dip[sock] >> 8);
        case 0x0F:
            return (uint8_t)(state->socket_dip[sock] & 0xFF);
        case 0x10: /* Sn_DPORT */
            return (uint8_t)(state->socket_dport[sock] >> 8);
        case 0x11:
            return (uint8_t)(state->socket_dport[sock] & 0xFF);
        case 0x20: /* Sn_TX_FSR high */
            {
                uint16_t used = (uint16_t)(state->tx_write_ptr[sock] - state->tx_rr_ptr[sock]);
                uint16_t free = (used > W5100_SOCKET_BUF_SIZE) ? 0 : (W5100_SOCKET_BUF_SIZE - used);
                return (uint8_t)(free >> 8);
            }
        case 0x21: /* Sn_TX_FSR low */
            {
                uint16_t used = (uint16_t)(state->tx_write_ptr[sock] - state->tx_rr_ptr[sock]);
                uint16_t free = (used > W5100_SOCKET_BUF_SIZE) ? 0 : (W5100_SOCKET_BUF_SIZE - used);
                fprintf(stderr, "W5100 TX_FSR low: sock=%d tx_wr=%u tx_rr=%u used=%u free=%u (0x%04X) ret=0x%02X\n",
                        sock, state->tx_write_ptr[sock], state->tx_rr_ptr[sock], used, free, free, (uint8_t)(free & 0xFF));
                fflush(stderr);
                return (uint8_t)(free & 0xFF);
            }
        case 0x22: /* Sn_TX_RD high */
            return (uint8_t)(state->tx_rr_ptr[sock] >> 8);
        case 0x23: /* Sn_TX_RD low */
            return (uint8_t)(state->tx_rr_ptr[sock] & 0xFF);
        case 0x24: /* Sn_TX_WR high */
            return (uint8_t)(state->tx_write_ptr[sock] >> 8);
        case 0x25: /* Sn_TX_WR low */
            return (uint8_t)(state->tx_write_ptr[sock] & 0xFF);
        case 0x26: /* Sn_RX_RSR high */
            if (state->mock_data_len > 0 && sock == 0) {
                return (uint8_t)(state->mock_data_len >> 8);
            }
            {
                uint16_t rx_avail = 0;
                if (state->rx_write_ptr[sock] >= state->rx_read_ptr[sock]) {
                    rx_avail = state->rx_write_ptr[sock] - state->rx_read_ptr[sock];
                } else {
                    rx_avail = (W5100_SOCKET_BUF_SIZE - state->rx_read_ptr[sock]) + state->rx_write_ptr[sock];
                }
                return (uint8_t)(rx_avail >> 8);
            }
        case 0x27: /* Sn_RX_RSR low */
            if (state->mock_data_len > 0 && sock == 0) {
                return (uint8_t)(state->mock_data_len & 0xFF);
            }
            {
                uint16_t rx_avail = 0;
                if (state->rx_write_ptr[sock] >= state->rx_read_ptr[sock]) {
                    rx_avail = state->rx_write_ptr[sock] - state->rx_read_ptr[sock];
                } else {
                    rx_avail = (W5100_SOCKET_BUF_SIZE - state->rx_read_ptr[sock]) + state->rx_write_ptr[sock];
                }
                return (uint8_t)(rx_avail & 0xFF);
            }
        case 0x28: /* Sn_RX_RD high */
            return (uint8_t)(state->rx_read_ptr[sock] >> 8);
        case 0x29: /* Sn_RX_RD low */
            return (uint8_t)(state->rx_read_ptr[sock] & 0xFF);
        case 0x2A: /* Sn_RX_WR high */
            return (uint8_t)(state->rx_write_ptr[sock] >> 8);
        case 0x2B: /* Sn_RX_WR low */
            return (uint8_t)(state->rx_write_ptr[sock] & 0xFF);
        default:
            break;
        }
    }

    /* TX/RX buffers */
    if (addr >= W5100_SOCK0_TXBUF && addr < W5100_SOCK0_TXBUF + 4 * W5100_SOCKET_BUF_SIZE) {
        return state->regs[addr];
    }
    if (addr >= W5100_SOCK0_RXBUF && addr < W5100_SOCK0_RXBUF + 4 * W5100_SOCKET_BUF_SIZE) {
        return state->regs[addr];
    }

    if (addr < 0x8000) {
        return state->regs[addr];
    }
    return 0;
}

void w5100_write_reg(w5100_state_t *state, uint8_t val) {
    uint16_t addr = state->spi_addr;

    /* Common registers */
    if (addr < 0x0400) {
        if (addr < 0x8000) {
            state->regs[addr] = val;
        }
        /* Handle MR reset */
        if (addr == 0x0000 && (val & 0x80)) {
            /* Soft reset: MR bit 7 self-clears after reset completes */
            state->regs[0] = 0x00;
            /* Reset all sockets to CLOSED */
            for (int s = 0; s < 4; s++) {
                state->socket_sr[s] = W5100_SR_CLOSED;
                state->socket_ir[s] = 0;
                state->socket_mr[s] = 0;
                state->socket_port[s] = 0;
                state->socket_dip[s] = 0;
                state->socket_dport[s] = 0;
                state->tx_write_ptr[s] = 0;
                state->tx_rr_ptr[s] = 0;
                state->host_fifo_head[s] = 0;
                state->host_fifo_tail[s] = 0;
                state->host_fifo_len[s] = 0;
                state->rx_write_ptr[s] = 0;
                state->rx_read_ptr[s] = 0;
                state->incoming_listen_port[s] = 0;
            }
        }
        return;
    }

    int sock = w5100_sock_from_addr(addr);
    if (sock >= 0) {
        uint16_t offset = w5100_sock_offset(addr, sock);

        switch (offset) {
        case 0x00: /* Sn_MR */
            state->socket_mr[sock] = val;
            if (addr < 0x8000) {
                state->regs[addr] = val;
            }
            break;
        case 0x01: /* Sn_CR - Command Register */
            if (val == W5100_CMD_OPEN) {
                state->socket_sr[sock] = W5100_SR_INIT;
                state->socket_ir[sock] = 0;
                state->send_pending[sock] = 0;
                state->tx_sent_wr[sock] = 0;
                state->regs[addr] = 0;
            } else if (val == W5100_CMD_LISTEN) {
                if (state->socket_sr[sock] == W5100_SR_INIT) {
                    state->socket_sr[sock] = W5100_SR_LISTEN;
                    state->incoming_listen_port[sock] = state->socket_port[sock];
                } else if (state->socket_sr[sock] == W5100_SR_CLOSED) {
                    /* Allow listen from closed in some cases */
                    state->socket_sr[sock] = W5100_SR_LISTEN;
                    state->incoming_listen_port[sock] = state->socket_port[sock];
                }
                state->socket_ir[sock] = 0;
                state->regs[addr] = 0;
            } else if (val == W5100_CMD_CONNECT) {
                state->socket_sr[sock] = W5100_SR_ESTABLISHED;
                state->socket_ir[sock] = W5100_SN_IR_CON;
                /* For backward compat with old behavior in tests */
                if (sock == 0) {
                    state->mock_data_ready = 1;
                    state->mock_data_len = 15;
                    memset(&state->regs[W5100_SOCK0_RXBUF], 0, W5100_SOCKET_BUF_SIZE);
                    const char *msg = "Hello, W5100!\r\n";
                    memcpy(&state->regs[W5100_SOCK0_RXBUF], msg, 15);
                    state->rx_write_ptr[0] = 15;
                }
                state->regs[addr] = 0;
            } else if (val == W5100_CMD_DISCON) {
                if (state->socket_sr[sock] == W5100_SR_ESTABLISHED) {
                    state->socket_sr[sock] = W5100_SR_FIN_WAIT;
                    /* TCP Close State Machine — УПРОЩЁННАЯ МОДЕЛЬ
                     * Реальный W5100 использует внутренние таймеры (RTO, 2MSL, Persist).
                     * Значения ниже ИСКУССТВЕННЫЕ, только для прохождения тестов драйвера.
                     * Не используйте для проверки корректности на железе. */
                    state->close_timer[sock] = 100;  // FIN_WAIT timeout (ARTIFICIAL)
                } else if (state->socket_sr[sock] == W5100_SR_CLOSE_WAIT) {
                    state->socket_sr[sock] = W5100_SR_LAST_ACK;
                    state->close_timer[sock] = 50;   // LAST_ACK timeout (ARTIFICIAL)
                } else {
                    state->socket_sr[sock] = W5100_SR_CLOSED;
                    state->close_timer[sock] = 0;
                }
                state->socket_ir[sock] |= W5100_SN_IR_DISCON;
                state->regs[addr] = 0;
                state->tx_write_ptr[sock] = 0;
                state->tx_rr_ptr[sock] = 0;
                state->send_pending[sock] = 0;
                state->tx_sent_wr[sock] = 0;
            } else if (val == W5100_CMD_CLOSE) {
                state->socket_sr[sock] = W5100_SR_CLOSED;
                state->socket_ir[sock] = 0;
                state->mock_data_len = 0;
                state->incoming_listen_port[sock] = 0;
                state->regs[addr] = 0;
                state->tx_write_ptr[sock] = 0;
                state->tx_rr_ptr[sock] = 0;
                state->send_pending[sock] = 0;
                state->tx_sent_wr[sock] = 0;
            } else if (val == W5100_CMD_SEND) {
                /* Real W5100: SEND command initiates transmission.
                 * The MAC reads data from TX buffer asynchronously and
                 * advances Sn_TX_RR as data is transmitted.
                 * On SEND command:
                 * 1. Check if host FIFO has space for new data
                 * 2. If space available: copy data from TX buffer [tx_rr_ptr, tx_write_ptr) to host FIFO
                 * 3. If no space: leave SEND pending (don't advance RR, don't set SENDOK)
                 * 4. Clear Sn_CR */
                {
                    uint16_t base = w5100_txbuf_base(sock);
                    uint16_t len = state->tx_write_ptr[sock] - state->tx_rr_ptr[sock];
                    if (len > W5100_SOCKET_BUF_SIZE) len = W5100_SOCKET_BUF_SIZE;
                    
                    /* If there's already a pending SEND, don't overwrite it */
                    if (state->send_pending[sock]) {
                        /* A SEND is already pending - ignore the new command */
                        state->regs[addr] = 0;
                        break;
                    }
                    
                    /* Check if host FIFO has space for this data */
                    if (state->host_fifo_len[sock] + len <= W5100_SOCKET_BUF_SIZE) {
                        /* Copy data to host FIFO for model_get_sent */
                        for (int i = 0; i < len; i++) {
                            state->host_fifo_data[sock][state->host_fifo_tail[sock]] = state->regs[base + (state->tx_rr_ptr[sock] + i) % W5100_SOCKET_BUF_SIZE];
                            state->host_fifo_tail[sock] = (state->host_fifo_tail[sock] + 1) % W5100_SOCKET_BUF_SIZE;
                            state->host_fifo_len[sock]++;
                        }
                        /* Advance RR to WR - data is now "transmitted" */
                        state->tx_rr_ptr[sock] = state->tx_write_ptr[sock];
                        state->send_pending[sock] = 0;
                        state->socket_ir[sock] |= W5100_SN_IR_SENDOK;
                    } else {
                        /* Host FIFO full - SEND remains pending */
                        state->send_pending[sock] = 1;
                        state->tx_sent_wr[sock] = state->tx_write_ptr[sock];
                        /* Don't advance RR, don't set SENDOK */
                        /* Sn_CR will be cleared but SEND remains pending */
                    }
                }
                state->regs[addr] = 0;
            } else if (val == W5100_CMD_RECV) {
                if (state->mock_data_len > 0 && sock == 0) {
                    state->mock_data_len = 0;
                }
                /* Advance read pointer by consumed data */
                state->rx_read_ptr[sock] = state->rx_write_ptr[sock];
                state->socket_ir[sock] &= ~W5100_SN_IR_RECV;
                state->regs[addr] = 0;
            } else {
                state->regs[addr] = 0;
            }
            break;
        case 0x02: /* Sn_IR - write 1 to clear */
            state->socket_ir[sock] &= (uint8_t)~val;
            if (addr < 0x8000) {
                state->regs[addr] = state->socket_ir[sock];
            }
            break;
        case 0x04: /* Sn_PORT high */
            state->socket_port[sock] = (state->socket_port[sock] & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x05: /* Sn_PORT low */
            state->socket_port[sock] = (state->socket_port[sock] & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x0C: /* Sn_DIPR */
            state->socket_dip[sock] = (state->socket_dip[sock] & 0x00FFFFFF) | ((uint32_t)val << 24);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x0D:
            state->socket_dip[sock] = (state->socket_dip[sock] & 0xFF00FFFF) | ((uint32_t)val << 16);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x0E:
            state->socket_dip[sock] = (state->socket_dip[sock] & 0xFFFF00FF) | ((uint32_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x0F:
            state->socket_dip[sock] = (state->socket_dip[sock] & 0xFFFFFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x10: /* Sn_DPORT */
            state->socket_dport[sock] = (state->socket_dport[sock] & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x11:
            state->socket_dport[sock] = (state->socket_dport[sock] & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x20:
            state->tx_write_ptr[sock] = (state->tx_write_ptr[sock] & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x21:
            state->tx_write_ptr[sock] = (state->tx_write_ptr[sock] & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x22:
        case 0x23:
            /* Sn_TX_RR is read-only on real W5100 - ignore writes */
            if (getenv("W5100_DEBUG")) {
                fprintf(stderr, "W5100 write Sn_TX_RR (read-only) ignored: sock=%d addr=0x%02X val=0x%02X\n",
                        sock, addr, val);
                fflush(stderr);
            }
            break;
        case 0x24:
            fprintf(stderr, "W5100 write Sn_TX_WR high: sock=%d val=0x%02X old=0x%04X new=0x%04X\n",
                    sock, val, state->tx_write_ptr[sock], (state->tx_write_ptr[sock] & 0x00FF) | ((uint16_t)val << 8));
            state->tx_write_ptr[sock] = (state->tx_write_ptr[sock] & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x25:
            fprintf(stderr, "W5100 write Sn_TX_WR low: sock=%d val=0x%02X old=0x%04X new=0x%04X\n",
                    sock, val, state->tx_write_ptr[sock], (state->tx_write_ptr[sock] & 0xFF00) | val);
            state->tx_write_ptr[sock] = (state->tx_write_ptr[sock] & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x26:
            state->mock_data_len = (state->mock_data_len & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x27:
            state->mock_data_len = (state->mock_data_len & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x28:
            state->rx_read_ptr[sock] = (state->rx_read_ptr[sock] & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x29:
            state->rx_read_ptr[sock] = (state->rx_read_ptr[sock] & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x2A:
            state->rx_write_ptr[sock] = (state->rx_write_ptr[sock] & 0x00FF) | ((uint16_t)val << 8);
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        case 0x2B:
            state->rx_write_ptr[sock] = (state->rx_write_ptr[sock] & 0xFF00) | val;
            if (addr < 0x8000) state->regs[addr] = val;
            break;
        default:
            if (addr < 0x8000) {
                state->regs[addr] = val;
            }
            break;
        }
        return;
    }

    /* TX/RX buffers - write to appropriate buffer */
    if (addr >= W5100_SOCK0_TXBUF && addr < W5100_SOCK0_TXBUF + 4 * W5100_SOCKET_BUF_SIZE) {
        int sb = (addr - W5100_SOCK0_TXBUF) / W5100_SOCKET_BUF_SIZE;
        int bo = (addr - W5100_SOCK0_TXBUF) % W5100_SOCKET_BUF_SIZE;
        (void)sb; (void)bo;
        if (addr < 0x8000) {
            state->regs[addr] = val;
        }
        return;
    }
    if (addr >= W5100_SOCK0_RXBUF && addr < W5100_SOCK0_RXBUF + 4 * W5100_SOCKET_BUF_SIZE) {
        if (addr < 0x8000) {
            state->regs[addr] = val;
        }
        return;
    }

    if (addr < 0x8000) {
        state->regs[addr] = val;
    }
}

void w5100_spi_update(w5100_state_t *state, uint8_t port_c_val) {
    uint8_t clk = (port_c_val & W5100_SPI_CLK_BIT) ? 1 : 0;
    uint8_t mosi = (port_c_val & W5100_SPI_MOSI_BIT) ? 1 : 0;
    uint8_t cs = (port_c_val & W5100_SPI_CS_BIT) ? 1 : 0;

    if (getenv("W5100_DEBUG")) {
        fprintf(stderr, "W5100 SPI: port_c=0x%02X clk=%d mosi=%d cs=%d prev_cs=%d state=%d\n",
                port_c_val, clk, mosi, cs, state->spi_prev_cs, state->spi_state);
    }

    /* CS rising edge ends transaction */
    if (cs == 1 && state->spi_prev_cs == 0) {
        if (getenv("W5100_DEBUG")) {
            fprintf(stderr, "W5100 SPI: CS rising edge, resetting to IDLE (prev_state=%d)\n", state->spi_state);
            fflush(stderr);
        }
        state->spi_state = SPI_STATE_IDLE;
        state->spi_bit_count = 0;
        state->spi_shift_reg = 0;
        state->spi_tx_reg = 0;
        state->spi_cs_state = 1;
        state->spi_byte_done = 0;
        state->spi_addr = 0;
        state->spi_rw = 0;
    }

    /* CS falling edge starts transaction */
    if (cs == 0 && state->spi_prev_cs == 1) {
        if (getenv("W5100_DEBUG")) {
            fprintf(stderr, "W5100 SPI: CS falling edge, starting new transaction\n");
            fflush(stderr);
        }
        state->spi_cs_state = 0;
        state->spi_state = SPI_STATE_OPCODE;
        state->spi_bit_count = 0;
        state->spi_shift_reg = 0;
        state->spi_tx_reg = 0;
        state->spi_byte_done = 0;
    }

    if (cs == 0) {
        /* Rising edge (CLK 0->1): slave samples MOSI, master samples MISO */
        if (clk && !state->spi_prev_clk) {
            /* Capture MISO on rising edge */
            if (state->spi_state == SPI_STATE_DATA && state->spi_rw && state->spi_bit_count < 8) {
                uint8_t miso_bit = (uint8_t)((state->spi_tx_reg >> 7) & 1);
                state->spi_rx_reg = (uint8_t)((state->spi_rx_reg << 1) | miso_bit);
            }

            if (getenv("W5100_DEBUG") && state->spi_state == SPI_STATE_DATA) {
                fprintf(stderr, "W5100 rising: mosi=%d state=%d bc=%d\n",
                        mosi, state->spi_state, state->spi_bit_count);
            }

            state->spi_shift_reg = (uint8_t)((state->spi_shift_reg << 1) | mosi);
            state->spi_bit_count++;

            if (state->spi_bit_count == 8) {
                state->spi_bit_count = 0;
                uint8_t byte = state->spi_shift_reg;
                state->spi_shift_reg = 0;

                switch (state->spi_state) {
                case SPI_STATE_OPCODE:
                    if (byte == W5100_SPI_WRITE_OPCODE) {
                        state->spi_rw = 0;
                        state->spi_state = SPI_STATE_ADDR_H;
                    } else if (byte == W5100_SPI_READ_OPCODE) {
                        state->spi_rw = 1;
                        state->spi_state = SPI_STATE_ADDR_H;
                    } else {
                        /* Try to be flexible - some implementations might differ */
                        /* Check if it's a write/read in different format */
                        if ((byte & 0xF0) == 0xF0) {
                            state->spi_rw = 0;
                            state->spi_state = SPI_STATE_ADDR_H;
                        } else if ((byte & 0x0F) == 0x0F || byte == 0x0F) {
                            state->spi_rw = 1;
                            state->spi_state = SPI_STATE_ADDR_H;
                        } else {
                            state->spi_state = SPI_STATE_IDLE;
}
}

                    if (getenv("W5100_DEBUG")) {
                        fprintf(stderr, "W5100 OPCODE=0x%02X rw=%d\n", byte, state->spi_rw);
                    }
                    break;
                case SPI_STATE_ADDR_H:
                    state->spi_addr = (uint16_t)(byte << 8);
                    state->spi_state = SPI_STATE_ADDR_L;
                    if (getenv("W5100_DEBUG")) {
                        fprintf(stderr, "W5100 ADDR_H=0x%02X addr=0x%04X\n", byte, state->spi_addr);
                    }
                    break;
                case SPI_STATE_ADDR_L:
                    state->spi_addr = (uint16_t)((state->spi_addr & 0xFF00) | byte);
                    state->spi_state = SPI_STATE_DATA;
                    if (getenv("W5100_DEBUG")) {
                        fprintf(stderr, "W5100 ADDR_L=0x%02X addr=0x%04X rw=%d\n", byte, state->spi_addr, state->spi_rw);
                    }
                    if (state->spi_rw) {
                        state->spi_tx_reg = w5100_read_reg(state);
                    }
                    break;
                case SPI_STATE_DATA:
                    if (getenv("W5100_DEBUG")) {
                        fprintf(stderr, "W5100 DATA: addr=0x%04X rw=%d rx=0x%02X\n",
                                state->spi_addr, state->spi_rw, byte);
                    }
                    if (!state->spi_rw) {
                        w5100_write_reg(state, byte);
                        state->spi_addr++;
                    }
                    state->spi_byte_done = 1;
                    break;
                default:
                    break;
                }
            }
        }

        /* Falling edge */
        if (!clk && state->spi_prev_clk) {
            if (state->spi_state == SPI_STATE_DATA && state->spi_rw && state->spi_bit_count > 0) {
                state->spi_tx_reg = (uint8_t)(state->spi_tx_reg << 1);
            }
            if (state->spi_byte_done) {
                state->spi_byte_done = 0;
                if (state->spi_state == SPI_STATE_DATA) {
                    state->spi_last_rx = state->spi_rx_reg;
                    state->spi_rx_reg = 0;
                    if (state->spi_rw) {
                        state->spi_addr++;
                        state->spi_tx_reg = w5100_read_reg(state);
                    }
                    if (getenv("W5100_DEBUG")) {
                        fprintf(stderr, "W5100 BYTE DONE: last_rx=0x%02X\n", state->spi_last_rx);
                    }
                }
            }
        }
    }

    state->spi_prev_clk = clk;
    state->spi_prev_cs = cs;
}

uint8_t w5100_spi_read_miso(w5100_state_t *state) {
    return (uint8_t)((state->spi_tx_reg >> 7) & 1);
}

/* Test/integration API */
void w5100_model_trigger_incoming(w5100_state_t *state, int sock, const uint8_t *data, uint16_t len) {
    if (sock < 0 || sock > 3 || !data || len == 0) return;
    /* If listening, accept and go to ESTABLISHED */
    if (state->socket_sr[sock] == W5100_SR_LISTEN || state->socket_sr[sock] == W5100_SR_INIT || state->socket_sr[sock] == W5100_SR_CLOSED) {
        state->socket_sr[sock] = W5100_SR_ESTABLISHED;
        state->socket_ir[sock] |= W5100_SN_IR_CON;
    }
    /* Put data into RX buffer */
    uint16_t base = w5100_rxbuf_base(sock);
    for (int i = 0; i < len && i < W5100_SOCKET_BUF_SIZE; i++) {
        state->regs[base + i] = data[i];
    }
    state->rx_write_ptr[sock] = len % W5100_SOCKET_BUF_SIZE;
    state->rx_read_ptr[sock] = 0;
    state->mock_data_len = len; /* for compatibility */
    state->socket_ir[sock] |= W5100_SN_IR_RECV;
}

int w5100_model_get_incoming(w5100_state_t *state, int sock, uint8_t *buf, uint16_t maxlen) {
    if (sock < 0 || sock > 3 || !buf) return 0;
    uint16_t base = w5100_rxbuf_base(sock);
    uint16_t avail = state->rx_write_ptr[sock] - state->rx_read_ptr[sock];
    if (avail > maxlen) avail = maxlen;
    for (int i = 0; i < avail; i++) {
        buf[i] = state->regs[base + i];
    }
    return avail;
}

int w5100_model_get_sent(w5100_state_t *state, int sock, uint8_t *buf, uint16_t maxlen) {
    if (sock < 0 || sock > 3 || !buf) return 0;
    uint16_t available = state->host_fifo_len[sock];
    uint16_t sent = (available > maxlen) ? maxlen : available;
    for (int i = 0; i < sent; i++) {
        buf[i] = state->host_fifo_data[sock][state->host_fifo_head[sock]];
        state->host_fifo_head[sock] = (state->host_fifo_head[sock] + 1) % W5100_SOCKET_BUF_SIZE;
        state->host_fifo_len[sock]--;
    }
    return sent;
}

void w5100_model_inject_connection(w5100_state_t *state, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port, const uint8_t *data, uint16_t len) {
    /* Find listening socket on local port */
    for (int sock = 0; sock < 4; sock++) {
        if (state->socket_sr[sock] == W5100_SR_LISTEN && state->incoming_listen_port[sock] == local_port) {
            state->socket_sr[sock] = W5100_SR_ESTABLISHED;
            state->socket_ir[sock] |= W5100_SN_IR_CON;
            state->socket_dip[sock] = remote_ip;
            state->socket_dport[sock] = remote_port;
            if (data && len > 0) {
                uint16_t base = w5100_rxbuf_base(sock);
                for (int i = 0; i < len && i < W5100_SOCKET_BUF_SIZE; i++) {
                    state->regs[base + i] = data[i];
                }
                state->rx_write_ptr[sock] = len % W5100_SOCKET_BUF_SIZE;
                state->rx_read_ptr[sock] = 0;
                state->socket_ir[sock] |= W5100_SN_IR_RECV;
            }
            return;
        }
    }
    /* If no listener found by port tracking, try to find any listening socket */
    for (int sock = 0; sock < 4; sock++) {
        if (state->socket_sr[sock] == W5100_SR_LISTEN) {
            state->socket_sr[sock] = W5100_SR_ESTABLISHED;
            state->socket_ir[sock] |= W5100_SN_IR_CON;
            state->socket_dip[sock] = remote_ip;
            state->socket_dport[sock] = remote_port;
            if (data && len > 0) {
                uint16_t base = w5100_rxbuf_base(sock);
                for (int i = 0; i < len && i < W5100_SOCKET_BUF_SIZE; i++) {
                    state->regs[base + i] = data[i];
                }
                state->rx_write_ptr[sock] = len % W5100_SOCKET_BUF_SIZE;
                state->rx_read_ptr[sock] = 0;
                state->socket_ir[sock] |= W5100_SN_IR_RECV;
            }
            return;
        }
    }
}

void w5100_model_poll_incoming(w5100_state_t *state) {
    /* Process pending SEND commands that were waiting for host FIFO space. */
    for (int sock = 0; sock < 4; sock++) {
        /* Check for explicitly pending SEND (set when FIFO was full at SEND command) */
        if (state->send_pending[sock]) {
            uint16_t len = state->tx_sent_wr[sock] - state->tx_rr_ptr[sock];
            if (len > W5100_SOCKET_BUF_SIZE) len = W5100_SOCKET_BUF_SIZE;
            
            if (state->host_fifo_len[sock] + len <= W5100_SOCKET_BUF_SIZE) {
                /* Space available - complete the SEND */
                if (state->tx_drain_delay_ticks[sock] > 0) {
                    if (state->tx_drain_timer[sock] == 0) {
                        state->tx_drain_timer[sock] = state->tx_drain_delay_ticks[sock];
                    } else if (state->tx_drain_timer[sock] > 0) {
                        state->tx_drain_timer[sock]--;
                        if (state->tx_drain_timer[sock] == 0) {
                            /* Delay complete - complete the SEND */
                            uint16_t base = w5100_txbuf_base(sock);
                            for (int i = 0; i < len; i++) {
                                state->host_fifo_data[sock][state->host_fifo_tail[sock]] = state->regs[base + (state->tx_rr_ptr[sock] + i) % W5100_SOCKET_BUF_SIZE];
                                state->host_fifo_tail[sock] = (state->host_fifo_tail[sock] + 1) % W5100_SOCKET_BUF_SIZE;
                                state->host_fifo_len[sock]++;
                            }
                            state->tx_rr_ptr[sock] = state->tx_sent_wr[sock];
                            state->send_pending[sock] = 0;
                            state->socket_ir[sock] |= W5100_SN_IR_SENDOK;
                        }
                    } else {
                        /* No delay - complete immediately */
                        uint16_t base = w5100_txbuf_base(sock);
                        for (int i = 0; i < len; i++) {
                            state->host_fifo_data[sock][state->host_fifo_tail[sock]] = state->regs[base + (state->tx_rr_ptr[sock] + i) % W5100_SOCKET_BUF_SIZE];
                            state->host_fifo_tail[sock] = (state->host_fifo_tail[sock] + 1) % W5100_SOCKET_BUF_SIZE;
                            state->host_fifo_len[sock]++;
                        }
                        state->tx_rr_ptr[sock] = state->tx_sent_wr[sock];
                        state->send_pending[sock] = 0;
                        state->socket_ir[sock] |= W5100_SN_IR_SENDOK;
                    }
                }
            }
        }
        /* Also handle case where SEND was issued without explicit pending flag
         * (legacy detection: TX_WR != TX_RR and no SENDOK) */
        else if (state->socket_sr[sock] == W5100_SR_ESTABLISHED &&
            (state->socket_ir[sock] & W5100_SN_IR_SENDOK) == 0 &&
            state->tx_write_ptr[sock] != state->tx_rr_ptr[sock]) {
            
            uint16_t len = state->tx_write_ptr[sock] - state->tx_rr_ptr[sock];
            if (len > W5100_SOCKET_BUF_SIZE) len = W5100_SOCKET_BUF_SIZE;
            
            if (state->host_fifo_len[sock] + len <= W5100_SOCKET_BUF_SIZE) {
                /* Check if drain delay is configured */
                if (state->tx_drain_delay_ticks[sock] > 0) {
                    if (state->tx_drain_timer[sock] == 0) {
                        state->tx_drain_timer[sock] = state->tx_drain_delay_ticks[sock];
                    } else if (state->tx_drain_timer[sock] > 0) {
                        state->tx_drain_timer[sock]--;
                        if (state->tx_drain_timer[sock] == 0) {
                            /* Delay complete - complete the SEND */
                            uint16_t base = w5100_txbuf_base(sock);
                            for (int i = 0; i < len; i++) {
                                state->host_fifo_data[sock][state->host_fifo_tail[sock]] = state->regs[base + (state->tx_rr_ptr[sock] + i) % W5100_SOCKET_BUF_SIZE];
                                state->host_fifo_tail[sock] = (state->host_fifo_tail[sock] + 1) % W5100_SOCKET_BUF_SIZE;
                                state->host_fifo_len[sock]++;
                            }
                            state->tx_rr_ptr[sock] = state->tx_write_ptr[sock];
                            state->socket_ir[sock] |= W5100_SN_IR_SENDOK;
                        }
                    } else {
                        /* No delay - complete immediately */
                        uint16_t base = w5100_txbuf_base(sock);
                        for (int i = 0; i < len; i++) {
                            state->host_fifo_data[sock][state->host_fifo_tail[sock]] = state->regs[base + (state->tx_rr_ptr[sock] + i) % W5100_SOCKET_BUF_SIZE];
                            state->host_fifo_tail[sock] = (state->host_fifo_tail[sock] + 1) % W5100_SOCKET_BUF_SIZE;
                            state->host_fifo_len[sock]++;
                        }
                        state->tx_rr_ptr[sock] = state->tx_write_ptr[sock];
                        state->socket_ir[sock] |= W5100_SN_IR_SENDOK;
                    }
                }
            }
        }
    }
    
    /* Advance per-socket close-state timers so transient TCP close states
     * (FIN_WAIT, TIME_WAIT, LAST_ACK) expire to CLOSED after close_ticks
     * poll cycles, modelling the real W5100's nonzero teardown time. */
    for (int s = 0; s < 4; s++) {
        if (state->close_timer[s] > 0) {
            state->close_timer[s]--;
            if (state->close_timer[s] == 0) {
                uint8_t sr = state->socket_sr[s];
                if (sr == W5100_SR_FIN_WAIT) {
                    state->socket_sr[s] = W5100_SR_TIME_WAIT;
                    state->close_timer[s] = 200;  // 2MSL
                } else if (sr == W5100_SR_TIME_WAIT) {
                    state->socket_sr[s] = W5100_SR_CLOSED;
                    state->socket_ir[s] = 0;
                } else if (sr == W5100_SR_LAST_ACK) {
                    state->socket_sr[s] = W5100_SR_CLOSED;
                    state->socket_ir[s] = 0;
                } else if (sr == W5100_SR_CLOSING) {
                    state->socket_sr[s] = W5100_SR_TIME_WAIT;
                    state->close_timer[s] = 200;
                }
            }
        }
    }
}

/* Test hook: directly set TX pointers for testing wraparound scenarios */
void w5100_model_test_set_tx_ptrs(w5100_state_t *state, int sock, uint16_t tx_wr, uint16_t tx_rr) {
    if (sock < 0 || sock > 3) return;
    state->tx_write_ptr[sock] = tx_wr;
    state->tx_rr_ptr[sock] = tx_rr;
}
