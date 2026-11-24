/*
 * cgi_hello - Hello CGI program for Muzix httpd
 *
 * Invoked by httpd when the URL path is "/cgi-bin/hello".
 * Receives: argv[0] = program path, argv[1] = socket descriptor string,
 *           argv[2] = User-Agent string
 * Sends an HTML page showing the client's User-Agent and IP address.
 */
#include "ztypes.h"
#include "../lib/libc.h"
#include "../lib/socket.h"

/* Minimal string length */
static int cgi_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Send a null-terminated string on the socket */
static void cgi_send_str(int sock, const char *s) {
    send(sock, (const uint8_t *)s, cgi_strlen(s));
}

int main(int argc, char **argv) {
    (void)argc;
    int sock;
    uint8_t ip[4];
    char tmp[4];
    char body[256];
    char *p;
    uint16_t body_len;
    int ret;

    sock = atoi(argv[1]);

    /* Get the client IP address from the socket */
    ret = net_peer_ip(sock, ip);

    /* Build the HTML body */
    p = body;
    body_len = 0;

    /* <h1>Hello from CGI!</h1> */
    {
        const char *s = "<h1>Hello from CGI!</h1>\n";
        while (*s && body_len < sizeof(body) - 1) { *p++ = *s++; body_len++; }
    }

    /* <p>User-Agent: VALUE</p> */
    {
        const char *s = "<p>User-Agent: ";
        while (*s && body_len < sizeof(body) - 1) { *p++ = *s++; body_len++; }
        if (argv[2]) {
            const char *ua = argv[2];
            while (*ua && body_len < sizeof(body) - 1) { *p++ = *ua++; body_len++; }
        }
        s = "</p>\n";
        while (*s && body_len < sizeof(body) - 1) { *p++ = *s++; body_len++; }
    }

    /* <p>Client IP: a.b.c.d</p> */
    {
        const char *s = "<p>Client IP: ";
        while (*s && body_len < sizeof(body) - 1) { *p++ = *s++; body_len++; }
        if (ret == 0) {
            itoa(ip[0], tmp, 10);
            {
                char *t = tmp;
                while (*t && body_len < sizeof(body) - 1) { *p++ = *t++; body_len++; }
            }
            if (body_len < sizeof(body) - 1) { *p++ = '.'; body_len++; }
            itoa(ip[1], tmp, 10);
            {
                char *t = tmp;
                while (*t && body_len < sizeof(body) - 1) { *p++ = *t++; body_len++; }
            }
            if (body_len < sizeof(body) - 1) { *p++ = '.'; body_len++; }
            itoa(ip[2], tmp, 10);
            {
                char *t = tmp;
                while (*t && body_len < sizeof(body) - 1) { *p++ = *t++; body_len++; }
            }
            if (body_len < sizeof(body) - 1) { *p++ = '.'; body_len++; }
            itoa(ip[3], tmp, 10);
            {
                char *t = tmp;
                while (*t && body_len < sizeof(body) - 1) { *p++ = *t++; body_len++; }
            }
        } else {
            const char *s2 = "unknown";
            while (*s2 && body_len < sizeof(body) - 1) { *p++ = *s2++; body_len++; }
        }
        s = "</p>\n";
        while (*s && body_len < sizeof(body) - 1) { *p++ = *s++; body_len++; }
    }

    if (body_len < sizeof(body)) {
        *p = 0;
    }

    /* ---- Send response ---- */
    send(sock, (const uint8_t *)"HTTP/1.0 200 OK\r\n", 17);
    send(sock, (const uint8_t *)"Server: Muzix-cgi/1.0 (Z80)\r\n", 29);
    send(sock, (const uint8_t *)"Content-Type: text/html\r\n", 25);
    {
        char len_buf[8];
        itoa(body_len, len_buf, 10);
        send(sock, (const uint8_t *)"Content-Length: ", 16);
        send(sock, (const uint8_t *)len_buf, cgi_strlen(len_buf));
        send(sock, (const uint8_t *)"\r\n", 2);
    }
    send(sock, (const uint8_t *)"Connection: close\r\n", 19);
    send(sock, (const uint8_t *)"\r\n", 2);

    /* Send body, retrying on partial sends */
    {
        int sent_total = 0;
        while (sent_total < body_len) {
            int n = send(sock, (const uint8_t *)(body + sent_total),
                         (uint16_t)(body_len - sent_total));
            if (n <= 0) break;
            sent_total += n;
        }
    }

    /* ---- Exit ----
     * The parent httpd closes the socket after wait() reaps this child.
     * Do not close here - the child exiting is the signal to the parent. */
    exit(0);
    return 0;
}
