/*
 * cgi_index - Index CGI program for Muzix httpd
 *
 * SDCC -mz80 code generation issues worked around here:
 *
 * 1. entry.name[di] indexed array access generates off-by-one Z80 code.
 *    Workaround: is_skip_entry() takes entry.name as const char* parameter.
 *
 * 2. Multiple send() calls with runtime parameters per loop iteration break
 *    SDCC's register allocation. Workaround: build the entire HTML response
 *    in a local buffer using byte copies, then send once at the end.
 */
#include "ztypes.h"
#include "../lib/libc.h"
#include "../lib/socket.h"

/* Document root served by the httpd - must match DOC_ROOT in apps/httpd.c
 * so the directory index lists the same tree that /doc URLs resolve to. */
#define DOC_ROOT "/doc"

static int g_cgi_sock;

static int cgi_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Check if a directory name is empty, ".", or "..". */
static int is_skip_entry(const char *name) {
    if (name[0] == 0) return 1;
    if (name[0] == '.' && name[1] == 0) return 1;
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) return 1;
    return 0;
}

/* Copy a NUL-terminated string using pointer dereference.
 * Returns bytes copied (excluding NUL). */
static uint16_t copy_str(char *dst, const char *src) {
    uint16_t n = 0;
    while (*src) {
        *dst++ = (char)*src++;
        n++;
    }
    *dst = 0;
    return n;
}

int main(int argc, char **argv) {
    (void)argc;
    int sock;
    int fd;
    int index;
    int ret;
    struct dirent entry;
    char html[8192];
    uint16_t html_len;
    char *p;

    sock = atoi(argv[1]);
    g_cgi_sock = sock;

    /* Build the entire HTML response in a buffer, then send once. */
    p = html;
    html_len = 0;

    /* Headers */
    {
        const char *s = "HTTP/1.0 200 OK\r\n"
                        "Server: Muzix-cgi/1.0 (Z80)\r\n"
                        "Content-Type: text/html\r\n"
                        "Connection: close\r\n"
                        "\r\n";
        while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }
    }

    /* Body - Sunset Cartridge 80s design (light cream theme) */
    {
        const char *s =
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<title>Muzix HTTP Server</title>\n"
"<style>\n"
"body{background:#fff8e1;color:#5d4037;font-family:'Courier New',monospace;margin:0;padding:20px;min-height:100vh}\n"
".container{max-width:768px;margin:40px auto;padding:30px;background:rgba(255,248,225,.9);border:4px double #ff6b35;box-shadow:0 0 30px rgba(255,107,53,.3),inset 0 0 20px rgba(255,255,255,.4);position:relative}\n"
"h1{color:#ef476f;text-align:center;text-transform:uppercase;letter-spacing:6px;text-shadow:1px 1px #ff6b35;margin-bottom:30px;font-size:30px}\n"
".retro-header{color:#ff6b35;font-size:20px;text-align:center;letter-spacing:4px;text-shadow:1px 1px #ffd166;margin-bottom:25px}\n"
".menu{text-align:center;margin:20px 0}\n"
".menu a{color:#ef476f;text-decoration:none;text-shadow:1px 1px #ffd166}\n"
".menu a:hover{color:#000;text-decoration:underline}\n"
"table{width:100%;border-collapse:collapse;margin-top:25px}\n"
"th{background:#ffd166;color:#3e2723;padding:12px;text-align:left;font-weight:bold;text-transform:uppercase;letter-spacing:2px;border-bottom:2px solid #ff6b35}\n"
"td{padding:10px;border-bottom:1px solid #ffe0b2}\n"
"tr:hover{background:#fff3e0}\n"
".file-link{color:#5d4037;text-decoration:none}\n"
".file-link:hover{color:#ef476f;text-decoration:underline}\n"
".footer{color:#8d6e63;font-size:11px;margin-top:35px;text-align:center;border-top:1px solid #ffe0b2;padding-top:15px;letter-spacing:2px;text-transform:uppercase}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"<div class=\"retro-header\">=== MUZIX HTTP SERVER ===</div>\n"
"<div class=\"menu\">\n"
"[ <a class=\"file-link\" href=\"/cgi-bin/hello\">CGI HELLO</a> ]  [ <a class=\"file-link\" href=\"/\">REFRESH</a> ]\n"
"</div>\n"
"<h1>Directory Index - /doc</h1>\n"
"<table>\n"
"<tr><th>Name</th></tr>\n";
        while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }
    }

    fd = open(DOC_ROOT, 0);

    if (fd >= 0) {
        index = 0;
        while (1) {
            ret = readdir(fd, index, &entry);

            if (ret != 0) break;

            if (is_skip_entry(entry.name)) {
                index++;
                continue;
            }

            /* Build <tr><td><a class="file-link" href="/NAME">NAME</a></td></tr>\n */
            {
                const char *s = "<tr><td><a class=\"file-link\" href=\"/";
                while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }

                const char *np = entry.name;
                while (*np && html_len < sizeof(html) - 1) { *p++ = (char)*np++; html_len++; }

                s = "\">";
                while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }

                np = entry.name;
                while (*np && html_len < sizeof(html) - 1) { *p++ = (char)*np++; html_len++; }

                s = "</a></td></tr>\n";
                while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }
            }

            index++;
        }
        close(fd);
    } else {
        const char *s = "<tr><td>Cannot open document root directory.</td></tr>\n";
        while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }
    }

    /* Body suffix - uptime instead of static text */
    {
        int32_t seconds;
        struct rtc_time boot;
        int32_t days = 0;
        int hour = 0, minute = 0, second = 0;
        char days_buf[6];
        char *dp;

        if (uptime(&seconds, &boot) == 0 && seconds >= 0) {
            while (seconds >= 86400) { seconds -= 86400; days++; }
            while (seconds >= 3600) { seconds -= 3600; hour++; }
            while (seconds >= 60) { seconds -= 60; minute++; }
            second = (int)seconds;
        }

        /* </table>\n</div>\n<div class="footer">\n */
        {
            const char *s = "</table>\n</div>\n<div class=\"footer\">\n";
            while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }
        }

        /* "MUZIX Z80 OPERATING SYSTEM - " */
        {
            const char *u = "MUZIX Z80 OPERATING SYSTEM - ";
            while (*u) { *p++ = *u++; html_len++; }
        }

        /* "up " */
        {
            const char *u = "up ";
            while (*u) { *p++ = *u++; html_len++; }
        }

        /* days - itoa() avoids the broken manual digit extraction below */
        {
            itoa((int)days, days_buf, 10);
            dp = days_buf;
            while (*dp && html_len < sizeof(html) - 1) { *p++ = *dp++; html_len++; }
        }

        /* " days, " */
        {
            const char *d = " days, ";
            while (*d) { *p++ = *d++; html_len++; }
        }

        /* HH:MM:SS - repeated subtraction instead of / and %, which on SDCC
         * Z80 call division helpers whose result lands in HL while the compiled
         * caller reads DE, silently producing wrong digits.  Same pattern as
         * apps/uptime.c out2(). */
        {
            int h_tens = 0;
            while (hour >= 10) { hour -= 10; h_tens++; }
            if (html_len < sizeof(html) - 1) { *p++ = (char)('0' + h_tens); html_len++; }
            if (html_len < sizeof(html) - 1) { *p++ = (char)('0' + hour); html_len++; }
            if (html_len < sizeof(html) - 1) { *p++ = ':'; html_len++; }

            int m_tens = 0;
            while (minute >= 10) { minute -= 10; m_tens++; }
            if (html_len < sizeof(html) - 1) { *p++ = (char)('0' + m_tens); html_len++; }
            if (html_len < sizeof(html) - 1) { *p++ = (char)('0' + minute); html_len++; }
            if (html_len < sizeof(html) - 1) { *p++ = ':'; html_len++; }

            int s_tens = 0;
            while (second >= 10) { second -= 10; s_tens++; }
            if (html_len < sizeof(html) - 1) { *p++ = (char)('0' + s_tens); html_len++; }
            if (html_len < sizeof(html) - 1) { *p++ = (char)('0' + second); html_len++; }
        }

        /* </div>\n</body>\n</html>\n */
        {
            const char *s = "</div>\n</body>\n</html>\n";
            while (*s && html_len < sizeof(html) - 1) { *p++ = *s++; html_len++; }
        }
    }

    /* NUL-terminate and send, retrying on partial sends */
    if (html_len > sizeof(html) - 1) {
        html_len = sizeof(html) - 1;
    }
    if (html_len < sizeof(html)) {
        *p = 0;
    }
    {
        int sent_total = 0;
        while (sent_total < html_len) {
            int n = send(g_cgi_sock, (const uint8_t *)(html + sent_total),
                         (uint16_t)(html_len - sent_total));
            if (n <= 0) break;
            sent_total += n;
        }
    }

    /* The parent httpd closes the socket after wait() reaps this child.
     * Do not close here - the child exiting is the signal to the parent. */
    exit(0);
    return 0;
}
