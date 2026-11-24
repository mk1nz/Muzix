/*
 * Kernel-side socket service handlers for W5100 network syscalls.
 *
 * These handle SYS_SOCKET (56), SYS_NETCTL (57), SYS_NETSTAT (58),
 * SYS_NETINFO (59). The W5100 driver (platform/zeta-v2/w5100.c) handles
 * hardware; this layer handles user↔kernel data transfer with 256-byte
 * chunking (MUZIX_ZETA_COPY_STAGING limit) and cooperative scheduler
 * integration.
 *
 * Key constraints:
 * - All user address args are sign-extended by the syscall trap; cast to
 *   (uint16_t) before use.
 * - Kernel→user copies use muzix_copy_user_to_current()
 * - User→kernel copies use muzix_copy_user_from_current()
 * - Blocking I/O returns -11 to userspace (EAGAIN/park-and-retry pattern)
 * - kbuf[256] fits in the ~1.2KB kernel stack headroom
 */
#include "syscalls.h"
#include "socket_service.h"
#include "socket_manager.h"
#include "proc_table.h"
#include "../platform/zeta-v2/w5100.h"
#include "../platform/zeta-v2/spi_io.h"

#define MUZIX_READ_AGAIN  (-11)

static const uint8_t g_default_mac[6] = {0x00, 0x11, 0x25, 0x00, 0x00, 0x01};
static const uint8_t g_default_ip[4]  = {192, 168, 0, 50};
static const uint8_t g_default_subnet[4] = {255, 255, 255, 0};
static const uint8_t g_default_gw[4] = {192, 168, 0, 1};

static uint8_t g_w5100_initialized = 0;

int
muzix_w5100_ensure_init(void) {
    if (!g_w5100_initialized) {
        if (w5100_init(g_default_mac, g_default_ip, g_default_subnet,
                       g_default_gw) != 0) {
            return -1;
        }
        g_w5100_initialized = 1;
    }
    return 0;
}

int
muzix_handle_netctl(muzix_userspace_syscall_context_t *ctx,
                    int sock, int op, uint16_t len, uint16_t data_ptr) {
    if (muzix_w5100_ensure_init() != 0) return -1;

    if (op == 0) {
        /* CONNECT: len = remote_port, data_ptr = ip4 ptr in userspace */
        uint8_t addr_buf[6];
        uint16_t port = len;
        if (muzix_copy_user_from_current(ctx, data_ptr, addr_buf, 6) != 0) {
            return -1;
        }
        w5100_connect(sock, port, addr_buf);
        uint8_t sr = w5100_get_status(sock);
        if (sr == W5100_SR_ESTABLISHED) return 0;
        if (sr == W5100_SR_CLOSED) return -1;
        return MUZIX_READ_AGAIN;
    }

    if (op == 1) {
        /* SEND: len = byte count, data_ptr = user data buffer */
        uint8_t kbuf[256];
        uint16_t offset = 0;
        uint16_t chunk;

        while (offset < len) {
            chunk = len - offset;
            if (chunk > 256) chunk = 256;
            if (muzix_copy_user_from_current(ctx, data_ptr + offset, kbuf, chunk) != 0) {
                return -1;
            }
            int sent = w5100_send(sock, kbuf, chunk);
            if (sent < 0) return -1;          /* Hardware error / timeout */
            if (sent == 0) break;             /* TX full - backpressure */
            offset += (uint16_t)sent;
            if (sent < (int)chunk) break;     /* Partial send (should not happen with our chunking) */
        }
        return offset;
    }

    if (op == 2) {
        /* RECV: len = maxlen, data_ptr = userspce output buffer */
        uint8_t kbuf[256];
        uint16_t offset = 0;
        uint16_t chunk;
        int got;
        int blocked = 0;

        while (offset < len) {
            chunk = len - offset;
            if (chunk > 256) chunk = 256;
            got = w5100_recv(sock, kbuf, chunk);
            if (got < 0) {
                blocked = 1;
                break;
            }
            if (got == 0) break;
            if (muzix_copy_user_to_current(ctx, data_ptr + offset, kbuf, (uint16_t)got) != 0) {
                return -1;
            }
            offset += (uint16_t)got;
            if (got < (int)chunk) break;
        }
        if (blocked && offset == 0) return -1;
        return offset;
    }

    if (op == 3) {
        /* CLOSE */
        if (w5100_close(sock) < 0) {
            /* Hardware close not confirmed: do not drop the manager entry,
             * so the socket number is not reused while the hardware state is
             * uncertain.  The caller may retry. */
            return -1;
        }
        /* Tell the socket manager the physical socket is free again, so the
         * number can be reused by socket().  Without this, net_close() drops
         * the hardware socket but the manager still counts it as allocated,
         * and socket() eventually runs out of physical sockets. */
        muzix_socket_manager_drop_socket(&g_muzix_socket_manager, sock);
        return 0;
    }

    if (op == 4) {
        /* LISTEN: len = port, data_ptr = unused */
        uint16_t port = len;
        return w5100_listen(sock, port);
    }

    return -1;
}

int
muzix_handle_netstat(muzix_userspace_syscall_context_t *ctx,
                     int sock, int what, uint16_t data_ptr) {
    if (muzix_w5100_ensure_init() != 0) return -1;

    if (what == 0) {
        /* Socket status (Sn_SR) */
        return w5100_get_status(sock);
    }

    if (what == 1) {
        /* Bytes available to read */
        return w5100_recv_available(sock);
    }

    if (what == 2) {
        /* Socket interrupt register (Sn_IR) */
        return w5100_get_interrupt(sock);
    }

    if (what == 3) {
        /* Peer (client) IP address from Sn_DIPR.
         * Copies 4 bytes to the userspace buffer at data_ptr,
         * or -1 on failure. */
        uint8_t ip[4];
        if (w5100_get_peer_ip(sock, ip) != 0) {
            return -1;
        }
        if (muzix_copy_user_to_current(ctx, (uint16_t)data_ptr, ip, 4) != 0) {
            return -1;
        }
        return 0;
    }

    if (what == 4) {
        /* popcount(g_w5100_socket_open) - number of open W5100 sockets */
        return w5100_socket_open_count();
    }

    if (what == 5) {
        /* Result of last w5100_close(): 0 = success, 0xFF = -1 (timeout) */
        return w5100_close_last_result();
    }

    if (what == 6) {
        /* Iterations used by last w5100_close() before CLOSED (0-255) */
        return w5100_close_last_iters();
    }

    if (what == 7) {
        /* Forced CMD_CLOSE flag from last w5100_close() (0 or 1) */
        return w5100_close_last_forced();
    }

    if (what == 8) {
        /* Histogram bucket for close iterations.
         * arg3 = bucket index (0-4): 0=0-9, 1=10-49, 2=50-99, 3=100-254, 4=255(timeout) */
        return w5100_close_hist_bucket((uint8_t)data_ptr);
    }

    if (what == 9) {
        /* Total count of forced CMD_CLOSE calls */
        return w5100_close_forced_count();
    }

    return -1;
}

int
muzix_handle_netinfo(muzix_userspace_syscall_context_t *ctx,
                     uint16_t data_ptr) {
    uint8_t info[10];  /* 6-byte MAC + 4-byte IP */
    if (muzix_w5100_ensure_init() != 0) return -1;
    w5100_get_info(info, info + 6);
    if (muzix_copy_user_to_current(ctx, data_ptr, info, 10) != 0) {
        return -1;
    }
    return 10;
}
