/*
 * httpd - HTTP server for Muzix supporting multiple concurrent connections.
 *
 * Adapted from commit 42b68a6 to use the streaming HTTP parser
 * (apps/http_parser.{h,c}).
 *
 * Document root: /doc  (filesystem subtree for web documents)
 * CGI: /cgi-bin/index handles "/" directory index requests;
 *      /cgi-bin/hello handles "/cgi-bin/hello" requests.
 *
 * CGI programs are invoked via fork/exec.  The child receives the socket
 * descriptor as argv[1] and the User-Agent as argv[2], and writes the
 * complete HTTP response directly to the socket via send().  The child
 * exits without closing the socket; the parent waits for the child via
 * wait() and then closes the socket with net_close().  This ensures the
 * W5100 socket is closed by the same process that accepted the connection,
 * so the remote host receives FIN and the TCP connection terminates
 * cleanly.  W5100 sockets have no reference counting - if the child closed
 * the socket, the parent's state tracking would be out of sync with the
 * hardware.
 * Document root: /doc  (filesystem subtree for web documents)
 * CGI: /cgi-bin/index handles "/" directory index requests;
 *      /cgi-bin/hello handles "/cgi-bin/hello" requests.
 *
 * CGI programs are invoked via fork/exec.  The child receives the socket
 * descriptor as argv[1] and the User-Agent as argv[2], and writes the
 * complete HTTP response directly to the socket via send().  The child
 * exits without closing the socket; the parent waits for the child via
 * wait() and then closes the socket with net_close().  This ensures the
 * W5100 socket is closed by the same process that accepted the connection,
 * so the remote host receives FIN and the TCP connection terminates
 * cleanly.  W5100 sockets have no reference counting - if the child closed
 * the socket, the parent's state tracking would be out of sync with the
 * hardware.
 *
 * Request timeouts (ticks, 120 Hz):
 *  - First byte:        600   (5 s)
 *  - Header completion: 9600  (80 s)
 *  - Total deadline:    18000 (150 s)
 *
 * httpd links libc.lib, socket.lib and syscall.lib via -k/-l so the linker
 * pulls only the modules it actually references (currently none from libc.lib,
 * 3 from socket.lib and 3 from syscall.lib).  The raw sys_* variants are
 * declared here as forward declarations rather than including syscall.h, to
 * avoid pulling in the entire syscall module when only a few wrappers are used.
 */
#include "../lib/socket.h"
#include "../lib/syscall.h"
#include "http_parser.h"
#include "ztypes.h"

struct stat {
    uint16_t st_mode;
    uint16_t st_uid;
    uint32_t st_size;
    uint16_t st_gid;
    uint8_t  st_links;
    uint16_t st_zone[7];
    uint16_t st_indirect_zone;
    uint8_t  st_used;
    uint8_t  st_pad[3];
};

/* Forward declarations for libc functions normally provided by libc.rel.
 * We avoid linking libc.rel to keep the httpd image under 16 KiB. */
int32_t sys_open(const char *pathname, int32_t flags);
int32_t sys_close(int32_t fd);
int32_t sys_read(int32_t fd, void *buf, int32_t count);
int32_t sys_stat(const char *pathname, void *statbuf);
int32_t sys_fork(void);
int32_t sys_exec(const char *path, const char * const *argv);
int32_t sys_wait(int32_t *status);
int32_t sys_time(int32_t *value);
void sys_exit(int32_t status);
int32_t sys_park(int32_t ticks);
int32_t sys_check_signal(void);

int32_t
sys_check_signal(void)
{
    return SYSCALL(SYS_CHECK_SIGNAL, 0, 0, 0);
}

#define NUM_SOCKETS 3
#define LISTEN_PORT 80
#define DOC_ROOT "/doc"

#define SOCK_FREE 0
#define SOCK_LISTEN 1
#define SOCK_ACTIVE 2

/* Request deadlines in ticks (120 Hz). */
#define DEADLINE_FIRST_BYTE 600   /* 5 s */
#define DEADLINE_HEADERS   9600   /* 80 s */
#define DEADLINE_TOTAL     18000  /* 150 s */

/* HTTP status lines, stored once as static objects so each status code is
 * referenced by pointer rather than duplicated as a string literal at every
 * call site. */
static const char *http_status_200_ok        = "HTTP/1.0 200 OK\r\n";
static const char *http_status_400_bad_req   = "HTTP/1.0 400 Bad Request\r\n";
static const char *http_status_404_not_found = "HTTP/1.0 404 Not Found\r\n";
static const char *http_status_414_uri_long  = "HTTP/1.0 414 Request URI Too Long\r\n";
static const char *http_status_431_fields    = "HTTP/1.0 431 Request Header Fields Too Large\r\n";
static const char *http_status_500           = "HTTP/1.0 500 Internal Server Error\r\n";
static const char *http_status_501_not_impl  = "HTTP/1.0 501 Not Implemented\r\n";

/* ---- String functions (static, avoid libc.lib) ----
 *
 * http_strlen, http_strcmp, http_strncmp, http_strcpy, http_strrchr and
 * http_itoa are defined as static functions here rather than called from
 * libc.lib.  Replacing them with libc calls would add external references
 * (_strlen, _strcmp, ...) that pull whole modules from libc.lib, and was
 * measured at +57 bytes .bin / +61 bytes _CODE with no functional benefit:
 * SDCC compiles these as small static functions and generates direct calls
 * to _http_strlen etc., not to external _strlen.  http_itoa in particular
 * differs from libc itoa: it avoids pulling strcpy.rel as a dependency
 * (libc itoa calls strcpy), and does not handle negative numbers (not
 * needed here).  libc itoa also uses repeated subtraction, not division,
 * so no __divuint/__moduint are involved.
 */
static int http_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int http_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int http_strncmp(const char *a, const char *b, int n) {
    while (n-- > 0 && *a && *a == *b) { a++; b++; }
    if (n < 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

static char *http_strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static const char *http_strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if ((unsigned char)*s == (unsigned char)c) last = s;
        s++;
    }
    if ((unsigned char)c == 0) return s;
    return last;
}

static void http_itoa(uint16_t n, char *buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[6];
    int i = 0;
    static const uint16_t place[5] = {10000, 1000, 100, 10, 1};
    int started = 0;
    for (int p = 0; p < 5; p++) {
        uint16_t pl = place[p];
        char c = '0';
        while (n >= pl) {
            n -= pl;
            c++;
        }
        if (started || c != '0' || p == 4) {
            tmp[i++] = c;
            started = 1;
        }
    }
    for (int j = 0; j < i; j++) buf[j] = tmp[j];
    buf[i] = '\0';
}

/* ---- Minimal stdout (avoids libc puts; one sys_write per byte) ----
 *
 * stdout_puts writes one byte at a time via sys_write.  This is simple and
 * pulls no libc modules, but is inefficient for long strings.  A line buffer
 * (like libc's putchar/puts) would batch writes but would pull stdout.rel
 * into every binary that links libc.lib - including ones that never print.
 * Keeping it here means httpd pays the per-byte cost only when it actually
 * sends data.
 */

static uint8_t g_sock_ids[NUM_SOCKETS];
static uint8_t g_sock_states[NUM_SOCKETS];
static uint8_t g_sock_active[NUM_SOCKETS];

static uint16_t g_slot_start[NUM_SOCKETS];
static uint16_t g_slot_first_byte[NUM_SOCKETS];
static uint8_t  g_slot_headers_done[NUM_SOCKETS];

/* Streaming HTTP parser state per socket. */
static http_parser_t g_parsers[NUM_SOCKETS];

/* Shared read buffer for recv() - used sequentially for all sockets. */
static uint8_t g_http_read_buf[256];

/* Reset a slot to LISTEN with a fresh W5100 socket immediately after a
 * request has been handled.  This keeps a socket in LISTEN at all times so
 * new connections can be accepted without waiting for the next loop pass.
 *
 * Returns 0 on success (slot reset and a new listener opened), -1 if the
 * socket close was not confirmed by the hardware.  On -1 the slot state is
 * left unchanged: g_sock_ids[i] still holds the socket ID, the allocator
 * still considers it busy, and the caller may retry on a later pass. */
static int http_reset_slot(int i) {
    uint8_t socket_id = g_sock_ids[i];

    if (socket_id != 0xFF) {
        if (net_close(socket_id) < 0) {
            /* Close not confirmed.  Keep the socket ID and ownership so the
             * slot can be retried on a later pass. */
            return -1;
        }
    }

    /* Only after a confirmed close is the ID released to the manager. */
    g_sock_ids[i] = 0xFF;
    g_sock_states[i] = SOCK_FREE;
    g_sock_active[i] = 0;
    g_slot_start[i] = 0;
    g_slot_first_byte[i] = 0;
    g_slot_headers_done[i] = 0;
    http_parser_init(&g_parsers[i]);

    int ns = socket(SOCK_STREAM, 0, 0);
    if (ns >= 0) {
        g_sock_ids[i] = (uint8_t)ns;
        g_sock_states[i] = SOCK_LISTEN;
        g_sock_active[i] = 0;
        g_slot_start[i] = 0;
        g_slot_first_byte[i] = 0;
        g_slot_headers_done[i] = 0;
        http_parser_init(&g_parsers[i]);
        if (net_listen_on_socket(ns, LISTEN_PORT) < 0) {
            net_close(ns);
            g_sock_ids[i] = 0xFF;
            g_sock_states[i] = SOCK_FREE;
        }
    } else {
        g_sock_ids[i] = 0xFF;
        g_sock_states[i] = SOCK_FREE;
    }

    return 0;
}

static int
http_send_all(int sock, const char *buf, int len) {
    int offset = 0;
    int sent;
    uint16_t start_tick;

    /* 10 second absolute deadline for sending (1200 ticks at 120 Hz) */
    start_tick = sys_getticks();

    while (offset < len) {
        sent = send(sock, (const uint8_t *)(buf + offset), len - offset);
        if (sent > 0) {
            offset += sent;
            continue;
        }
        if (sent == 0) {
            /* TX full - backpressure: yield to scheduler for 1 tick (8.3 ms) */
            if ((uint16_t)(sys_getticks() - start_tick) >= 1200) {
                return -1; /* absolute deadline exceeded */
            }
            sys_park(1);
            continue;
        }
        /* sent < 0: hardware error / timeout */
        return -1;
    }
    return offset;
}

static int
http_send_string(int sock, const char *s) {
    return http_send_all(sock, s, http_strlen(s));
}

/* Map a URL path to a local filesystem path.
 * Returns 0 on success (fills in local_path), -1 if the path escapes
 * the document root (path traversal attempt).
 *
 * DOC_ROOT is prefixed to the URL path.  If the URL already names the
 * document root (/doc or /doc/...), that leading segment is not doubled:
 * /doc/readme.txt maps to /doc/readme.txt, and /doc maps to /doc itself.
 * A separator is always inserted between DOC_ROOT and the remaining path
 * component. */
static int http_map_path(const char *url_path, char *local_path, int path_sz) {
  const char *src = url_path;
  char *dst = local_path;
  int remaining = path_sz;
  int doc_len;

  doc_len = http_strlen(DOC_ROOT);
  if (doc_len >= path_sz) return -1;

  /* Skip leading slashes in the URL path. */
  while (*src == '/') src++;

  /* If the URL already references the document root by name (/doc or
   * /doc/...), strip that leading segment so it is not doubled when
   * DOC_ROOT is prepended. */
  if (http_strncmp(src, "doc", 3) == 0 && (src[3] == '/' || src[3] == '\0')) {
    src += 3;
    while (*src == '/') src++;
  }

  http_strcpy(local_path, DOC_ROOT);
  dst = local_path + doc_len;
  remaining -= doc_len;

  /* Insert a separator between DOC_ROOT and the URL path component. */
  if (*src) {
    if (remaining < 1) return -1;
    *dst++ = '/';
    remaining--;
  }

  while (*src) {
    while (*src == '/') {
      if (remaining < 1) return -1;
      *dst++ = '/';
      remaining--;
      src++;
    }
    if (*src == '\0') break;
    if (src[0] == '.' && src[1] == '.' && (src[2] == '/' || src[2] == '\0')) {
      return -1;
    }
    while (*src && *src != '/') {
      if (remaining < 1) return -1;
      *dst++ = *src++;
      remaining--;
    }
  }
  *dst = '\0';
  return 0;
}

/* Determine MIME type from file extension. */
static const char *http_mime_type(const char *path) {
  const char *ext = http_strrchr(path, '.');
  if (!ext) return "text/plain";
  if (http_strcmp(ext, ".html") == 0 || http_strcmp(ext, ".htm") == 0)
    return "text/html";
  if (http_strcmp(ext, ".txt") == 0) return "text/plain";
  if (http_strcmp(ext, ".md") == 0) return "text/plain";
  if (http_strcmp(ext, ".css") == 0) return "text/css";
  if (http_strcmp(ext, ".js") == 0) return "application/javascript";
  return "text/plain";
}

/* Send an HTTP error response with a small HTML body so the browser
 * displays the status text instead of its own default error page.
 * status_line is "HTTP/1.0 CODE REASON\r\n". */
static void http_send_error(int sock, const char *status_line)
{
    /* Extract "CODE REASON" from "HTTP/1.0 CODE REASON\r\n". */
    const char *reason = status_line + 9;       /* skip "HTTP/1.0 " */
    char body[96];
    int blen = 0;
    const char *head = "<html><body><h1>";
    const char *tail = "</h1></body></html>";
    const char *p;
    char len_buf[8];

    for (p = head; *p; p++) body[blen++] = (char)*p;
    while (*reason && *reason != '\r' && *reason != '\n') {
        body[blen++] = (char)*reason++;
    }
    for (p = tail; *p; p++) body[blen++] = (char)*p;
    body[blen] = '\0';

    http_send_string(sock, status_line);
    http_send_string(sock, "Content-Type: text/html\r\n");
    http_itoa((uint16_t)blen, len_buf);
    http_send_string(sock, "Content-Length: ");
    http_send_string(sock, len_buf);
    http_send_string(sock, "\r\n\r\n");
    http_send_all(sock, body, blen);
}

/* Serve a static file from the document root. */
static void http_serve_file(int sock, const char *local_path) {
  struct stat st;
  int fd = sys_open(local_path, 0);
  if (fd < 0) {
    http_send_error(sock, http_status_404_not_found);
    return;
  }

  if (sys_stat(local_path, &st) != 0) {
    sys_close(fd);
    http_send_error(sock, http_status_500);
    return;
  }

  char len_buf[16];
  http_itoa((uint16_t)st.st_size, len_buf);

  if (http_send_string(sock, http_status_200_ok) < 0) goto cleanup;
  if (http_send_string(sock, "Server: Muzix-httpd/1.0 (Z80)\r\n") < 0) goto cleanup;
  if (http_send_string(sock, "Content-Type: ") < 0) goto cleanup;
  if (http_send_string(sock, http_mime_type(local_path)) < 0) goto cleanup;
  if (http_send_string(sock, "\r\n") < 0) goto cleanup;
  if (http_send_string(sock, "Content-Length: ") < 0) goto cleanup;
  if (http_send_string(sock, len_buf) < 0) goto cleanup;
  if (http_send_string(sock, "\r\n") < 0) goto cleanup;
  if (http_send_string(sock, "Connection: close\r\n") < 0) goto cleanup;
  if (http_send_string(sock, "\r\n") < 0) goto cleanup;

  /* Send file contents in chunks */
  uint8_t readbuf[256];
  int got;
  while ((got = sys_read(fd, readbuf, sizeof(readbuf))) > 0) {
    if (http_send_all(sock, (const char *)readbuf, got) < 0) {
      break;
    }
  }
cleanup:
  sys_close(fd);
}

/* Execute a CGI program: fork, child execs with socket number and
 * User-Agent as arguments.  The child owns the socket and sends the
 * response directly, then closes it.  The parent waits for the child
 * and then closes the socket. */
static void http_exec_cgi(int sock, const char *cgi_path,
                          const char *user_agent) {
  int pid = sys_fork();

  if (pid < 0) {
    /* Fork failed: serve an error instead */
    http_send_error(sock, http_status_500);
    return;
  }

  if (pid == 0) {
    /* Child process: exec the CGI program.
     * argv[0] = cgi path, argv[1] = socket number string,
     * argv[2] = User-Agent string */
    char sock_str[8];
    http_itoa((uint16_t)sock, sock_str);

    char *argv[4];
    argv[0] = (char *)cgi_path;
    argv[1] = sock_str;
    argv[2] = (char *)user_agent;
    argv[3] = NULL;

    sys_exec(cgi_path, (const char *const *)argv);

    /* exec failed - report 500 to the client, then exit so the parent's
     * sys_wait() returns and http_reset_slot() can reuse this socket. */
    http_send_error(sock, http_status_500);
    sys_exit(1);
    /* Never reached: sys_exit() does not return, but keep the
     * compiler happy. */
    while (1) {
    }
}

  /* Parent: wait for the child to exit before the socket is closed by
   * http_reset_slot().  sys_wait() returns 0 after a park/resume without
   * reaping, so zero is not completion - call again.  The only negative
   * return reachable after the fork is the status-copy failure in
   * muzix_handle_wait() (kernel/syscalls_ipc.c:841-848), and by then the
   * child has already exited and been reaped: the scan at
   * kernel/syscalls_ipc.c:829-851 found the zombie, cleared its exited
   * flag at line 838, and only then attempted the copy at line 843.  So
   * on this path the CGI child is gone, the exit status is lost, and the
   * parent can answer the client and return to the caller, which resets
   * the socket. */
  {
    int status;
    int pid;

    do {
        pid = sys_wait(&status);
    } while (pid == 0);

    if (pid < 0) {
        /* sys_wait() returned -1 after the CGI child had already exited
         * and been reaped; the status copy to the caller's buffer failed.
         * The child is gone, so there is no race with the socket reset,
         * and the caller closes and reopens the socket after return. */
        http_send_error(sock, http_status_500);
    }
  }
  return;
}

/* Close every socket owned by httpd and release the slots.  Called when a
 * signal is pending so the W5100 sockets are released cleanly instead of
 * being left in a hung state on the hardware.  Returns the number of
 * sockets closed. */
static int http_close_all_sockets(void)
{
    int i;
    int closed = 0;

    for (i = 0; i < NUM_SOCKETS; i++) {
        if (g_sock_ids[i] != 0xFF) {
            if (net_close(g_sock_ids[i]) >= 0) {
                closed++;
            }
            g_sock_ids[i] = 0xFF;
            g_sock_states[i] = SOCK_FREE;
            g_sock_active[i] = 0;
            g_slot_start[i] = 0;
            g_slot_first_byte[i] = 0;
            g_slot_headers_done[i] = 0;
            http_parser_init(&g_parsers[i]);
        }
    }
    return closed;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  int i;
  uint16_t now;

  for (i = 0; i < NUM_SOCKETS; i++) {
    g_sock_ids[i] = 0;
    g_sock_states[i] = SOCK_FREE;
    g_sock_active[i] = 0;
    g_slot_start[i] = 0;
    g_slot_first_byte[i] = 0;
    g_slot_headers_done[i] = 0;
    http_parser_init(&g_parsers[i]);
  }

  /* Create listening sockets on all available slots */
  for (i = 0; i < NUM_SOCKETS; i++) {
      int s = socket(SOCK_STREAM, 0, 0);
    if (s >= 0) {
      g_sock_ids[i] = (uint8_t)s;
      g_sock_active[i] = 0;
      g_slot_start[i] = 0;
      g_slot_first_byte[i] = 0;
      g_slot_headers_done[i] = 0;
      http_parser_init(&g_parsers[i]);
      if (net_listen_on_socket(s, LISTEN_PORT) < 0) {
        net_close(s);
        g_sock_ids[i] = 0xFF;
        g_sock_states[i] = SOCK_FREE;
      } else {
        g_sock_states[i] = SOCK_LISTEN;
      }
    } else {
      g_sock_ids[i] = 0xFF;
      g_sock_states[i] = SOCK_FREE;
    }
  }

  while (1) {
    now = sys_getticks();

    /* If a signal is pending (kill sent us SIGTERM/SIGKILL/etc.), close all
     * sockets cleanly so the W5100 hardware is not left with hung sockets,
     * then exit.  This catches running processes that never park between
     * requests, so a kill takes effect at the next main-loop pass rather
     * than only after a sys_park(). */
    {
        int pending = sys_check_signal();
        if (pending != 0) {
            int signal = 1;
            while ((pending & 1) == 0) {
                pending >>= 1;
                signal++;
            }
            http_close_all_sockets();
            sys_exit(128 + signal);
        }
    }

    for (i = 0; i < NUM_SOCKETS; i++) {
      uint8_t st = g_sock_states[i];

      /* Cleanup: if slot is SOCK_FREE but has an associated socket that
       * is CLOSED (CGI child finished, host disconnected), re-open it
       * as a listener so the socket is not leaked. */
      if (st == SOCK_FREE && g_sock_ids[i] != 0xFF) {
        net_close(g_sock_ids[i]);
        int ns = socket(SOCK_STREAM, 0, 0);
        if (ns >= 0) {
          g_sock_ids[i] = (uint8_t)ns;
          g_sock_states[i] = SOCK_LISTEN;
          g_sock_active[i] = 0;
          g_slot_start[i] = 0;
          g_slot_first_byte[i] = 0;
          g_slot_headers_done[i] = 0;
          http_parser_init(&g_parsers[i]);
          net_listen_on_socket(ns, LISTEN_PORT);
        } else {
          g_sock_ids[i] = 0xFF;
          g_sock_states[i] = SOCK_FREE;
        }
        continue;
      }

      if (st == SOCK_LISTEN) {
        int sr = netstat(g_sock_ids[i], 0);
        if (sr == 0x17 || sr == 0x1c) { /* ESTABLISHED or CLOSE_WAIT */
          /* New connection arrived on listening slot */
          g_sock_states[i] = SOCK_ACTIVE;
          g_sock_active[i] = 0;
          g_slot_start[i] = (uint32_t)now;
          g_slot_first_byte[i] = 0;
          g_slot_headers_done[i] = 0;
          http_parser_init(&g_parsers[i]);
        } else if (sr == 0x00 || sr == 0x13) { /* CLOSED or INIT */
          /* Reopen this socket as listener */
          net_close(g_sock_ids[i]);
          int ns = socket(SOCK_STREAM, 0, 0);
          if (ns >= 0) {
            g_sock_ids[i] = (uint8_t)ns;
            g_sock_states[i] = SOCK_LISTEN;
            g_sock_active[i] = 0;
            g_slot_start[i] = 0;
            g_slot_first_byte[i] = 0;
            g_slot_headers_done[i] = 0;
            http_parser_init(&g_parsers[i]);
            net_listen_on_socket(ns, LISTEN_PORT);
          } else {
            g_sock_ids[i] = 0xFF;
            g_sock_states[i] = SOCK_FREE;
          }
        }
      } else if (st == SOCK_ACTIVE) {
        int sock = g_sock_ids[i];
        int sr = netstat(sock, 0);

        /* ---- Absolute deadlines (ticks from sys_getticks()) ---- */

        /* Total deadline: no connection may occupy a slot forever. */
        if ((uint16_t)(sys_getticks() - g_slot_start[i]) >= DEADLINE_TOTAL) {
          if (http_reset_slot(i) < 0) {
            continue;
          }
          continue;
        }

        /* First-byte deadline: client must send something within 5 s. */
        if (g_slot_first_byte[i] == 0 &&
            (uint16_t)(sys_getticks() - g_slot_start[i]) >= DEADLINE_FIRST_BYTE) {
          if (http_reset_slot(i) < 0) {
            continue;
          }
          continue;
        }

        /* Header-completion deadline: client must send \r\n\r\n within
         * 80 s of the first byte. */
        if (g_slot_first_byte[i] != 0 && !g_slot_headers_done[i] &&
            (uint16_t)(sys_getticks() - g_slot_first_byte[i]) >= DEADLINE_HEADERS) {
          if (http_reset_slot(i) < 0) {
            continue;
          }
          continue;
        }

        if (sr == 0x17 || sr == 0x1c) { /* ESTABLISHED or CLOSE_WAIT */
          int got = recv(sock, g_http_read_buf, sizeof(g_http_read_buf));
          if (got > 0) {
            if (g_slot_first_byte[i] == 0)
              g_slot_first_byte[i] = sys_getticks();

            /* Feed received data into the streaming parser. */
            http_parse_result_t pr =
                http_parser_feed(&g_parsers[i], g_http_read_buf, (uint16_t)got);

            if (pr == HTTP_PARSE_COMPLETE) {
              g_slot_headers_done[i] = 1;

              http_method_t method = http_parser_method(&g_parsers[i]);
              const char *path = http_parser_uri(&g_parsers[i]);
              const char *user_agent = http_parser_user_agent(&g_parsers[i]);

              if (method == HTTP_METHOD_GET || method == HTTP_METHOD_HEAD) {
                /* Root or directory index -> CGI */
                if (path[0] == '/' &&
                    (path[1] == '\0' || http_strcmp(path, "/index") == 0 ||
                     http_strcmp(path, "/index.html") == 0)) {
                  struct stat st;
                  if (sys_stat("/cgi-bin/index", &st) != 0) {
                    http_send_error(sock, http_status_404_not_found);
                    if (http_reset_slot(i) < 0) {
                      continue;
                    }
                    continue;
                  }
                  http_exec_cgi(sock, "/cgi-bin/index", user_agent);
                  if (http_reset_slot(i) < 0) {
                    continue;
                  }
                  continue;
                }
                /* CGI bin */
                if (http_strncmp(path, "/cgi-bin/", 9) == 0) {
                  char cgi_path[64] = {0};
                  int len = http_strlen(path);
                  if (len < (int)sizeof(cgi_path)) {
                    http_strcpy(cgi_path, path);
                    struct stat st;
                    if (sys_stat(cgi_path, &st) != 0) {
    http_send_error(sock, http_status_404_not_found);
                      if (http_reset_slot(i) < 0) {
                        continue;
                      }
                      continue;
                    }
                    http_exec_cgi(sock, cgi_path, user_agent);
                    if (http_reset_slot(i) < 0) {
                      continue;
                    }
                    continue;
                  }
                }
                /* Static file from document root */
                {
                  char local_path[128];
                  if (http_map_path(path, local_path, sizeof(local_path)) == 0 &&
                      local_path[0] != '\0') {
                    http_serve_file(sock, local_path);
                  } else {
                    http_send_error(sock, http_status_404_not_found);
                  }
                  if (http_reset_slot(i) < 0) {
                    continue;
                  }
                  continue;
                }
              } else {
                http_send_error(sock, http_status_501_not_impl);
                if (http_reset_slot(i) < 0) {
                  continue;
                }
                continue;
              }
            } else if (pr < 0) {
              /* Parse error */
              int error_code = http_parser_error_code(&g_parsers[i]);
              const char *status_line;
              switch (error_code) {
                case 414: status_line = http_status_414_uri_long; break;
                case 431: status_line = http_status_431_fields; break;
                case 501: status_line = http_status_501_not_impl; break;
                default:  status_line = http_status_400_bad_req; break;
              }
              http_send_error(sock, status_line);
              if (http_reset_slot(i) < 0) {
                continue;
              }
              continue;
            } else {
              /* HTTP_PARSE_OK: incomplete request, need more data.
               * Keep the parser state and wait for the next recv(). */
            }
          } else if (got == 0) {
            /* Peer closed the connection (FIN received). */
            if (http_reset_slot(i) < 0) {
              continue;
            }
            continue;
          } else {
            /* got < 0: separate WOULD_BLOCK (-1) from other recv errors. */
            if (got == -1) {
                if (sr == 0x1c) {
                    if (http_reset_slot(i) < 0) {
                        continue;
                    }
                }
            } else {
                /* Other negative recv result: genuine error. */
                if (http_reset_slot(i) < 0) {
                    continue;
                }
            }
            continue;
          }
        } else {
          /* Non-ESTABLISHED/CLOSE_WAIT status on an active slot. */
          if (http_reset_slot(i) < 0) {
            continue;
          }
        }
      }
    }
    sys_park(30);

    /* If a signal is pending (kill sent us SIGTERM/SIGKILL/etc.), close all
     * sockets cleanly so the W5100 hardware is not left with hung sockets,
     * then exit.  The kernel will deliver the signal at the next syscall
     * entry anyway; we get a chance to clean up first here. */
    {
        int pending = sys_check_signal();
        if (pending != 0) {
            int signal = 1;
            while ((pending & 1) == 0) {
                pending >>= 1;
                signal++;
            }
            http_close_all_sockets();
            sys_exit(128 + signal);
        }
    }
}
  return 0;
}

/* exit: C function mangled to symbol _exit by SDCC.
 * crt0.s calls _exit when main() returns; httpd never returns,
 * but the symbol must exist for the link without libc.rel. */
void exit(int32_t status) {
    (void)status;
    while (1) {
    }
}
