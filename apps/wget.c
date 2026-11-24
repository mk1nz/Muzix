/*
 * host-test-not-portable: uses socket()/send()/recv() which are syscall
 * wrappers that trap to the Z80 kernel; not runnable on host without the
 * full syscall dispatcher + W5100 emulator.
 */
#include "../lib/socket.h"
#include "../lib/syscall.h"
#include "../lib/libc.h"
#include "../lib/tty_ioctl.h"
#include "libc.h"

static uint8_t server_ip[4] = {192, 168, 1, 100};
static uint16_t server_port = 80;
static const char *http_path = "/";

static int
parse_ip(const char *s, uint8_t *ip) {
    int i, val;
    const char *p = s;
    for (i = 0; i < 4; i++) {
        val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        if (val < 0 || val > 255) return -1;
        ip[i] = (uint8_t)val;
        if (i < 3 && *p == '.') p++;
    }
    return 0;
}

int
main(int argc, char **argv) {
    if (argc >= 2) {
        if (parse_ip(argv[1], server_ip) < 0) {
            write(1, "Invalid IP\n", 11);
            sys_exit(1);
        }
    }
    if (argc >= 3) {
        server_port = (uint16_t)atoi(argv[2]);
    }
    if (argc >= 4) {
        http_path = argv[3];
    }

    int sock = socket(SOCK_STREAM, 0, 0);
    if (sock < 0) {
        write(1, "socket failed\n", 14);
        sys_exit(1);
    }

    if (socket_connect_blocking(sock, server_ip, server_port) < 0) {
        write(1, "connect failed\n", 15);
        sys_exit(1);
    }

    /* Build and send HTTP GET request (fits in 256-byte staging) */
    char request[172];
    char tmp[4];
    strcpy(request, "GET ");
    strcat(request, http_path);
    strcat(request, " HTTP/1.0\r\nHost: ");
    itoa(server_ip[0], tmp, 10);
    strcat(request, tmp);
    strcat(request, ".");
    itoa(server_ip[1], tmp, 10);
    strcat(request, tmp);
    strcat(request, ".");
    itoa(server_ip[2], tmp, 10);
    strcat(request, tmp);
    strcat(request, ".");
    itoa(server_ip[3], tmp, 10);
    strcat(request, tmp);
    strcat(request, "\r\nUser-Agent: Muzix-wget/1.0 (Z80)\r\n\r\n");
    int req_len = strlen(request);
    send(sock, request, req_len);

    /* Receive response */
    char resp[256];
    int n;
    while ((n = recv(sock, resp, 256)) > 0) {
        write(1, resp, n);
    }

    write(1, "\r\n", 2);

    net_close(sock);
    return 0;
}
