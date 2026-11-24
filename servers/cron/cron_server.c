#include "server.h"
#include "../lib/libc.h"
#include "../lib/syscall.h"
#include "../kernel/cron.h"
#include "../platform/zeta-v2/uart_io.h"
#include "libc.h"

#define CRON_MAX_JOBS 8

static muzix_server_message_t g_reply;

static uint8_t g_cron_running = 1;
static uint8_t g_cron_job_count = 0;
static uint8_t g_cron_last_minute = 0xFF;
static uint8_t g_cron_last_hour = 0xFF;
static uint8_t g_cron_job_minutes[CRON_MAX_JOBS];
static uint8_t g_cron_job_hours[CRON_MAX_JOBS];
static uint8_t g_cron_job_min_int[CRON_MAX_JOBS];
static char g_cron_job_cmd[CRON_MAX_JOBS][16];

static int cron_server_init(void) {
    memset(&g_reply, 0, sizeof(g_reply));
    g_cron_running = 1;
    g_cron_job_count = 0;
    g_cron_last_minute = 0xFF;
    g_cron_last_hour = 0xFF;
    for (int i = 0; i < CRON_MAX_JOBS; i++) {
        g_cron_job_minutes[i] = 0;
        g_cron_job_hours[i] = 0;
        g_cron_job_min_int[i] = 0;
        g_cron_job_cmd[i][0] = '\0';
    }
    return 0;
}

static void cron_server_cleanup(void) {
    g_cron_running = 0;
}

static void cron_uart_putc(char c) {
    if (c == '\n') {
        muzix_zeta_uart_send((uint8_t)'\r', 0);
    }
    muzix_zeta_uart_send((uint8_t)c, 0);
}

static void cron_uart_puts(const char *s) {
    while (*s) {
        cron_uart_putc(*s++);
    }
}

static int cron_debug_output(const char *s) {
    cron_uart_puts(s);
    return 0;
}

static int handle_status(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = g_cron_running;
    return 0;
}

static int handle_start(const void *msg) {
    (void)msg;
    g_cron_running = 1;
    g_reply.m1_i1 = 0;
    return 0;
}

static int handle_stop(const void *msg) {
    (void)msg;
    g_cron_running = 0;
    g_reply.m1_i1 = 0;
    return 0;
}

static void format_num(char *buf, int *pos, uint8_t val) {
    uint8_t tens = 0;
    uint8_t rem = val;
    while (rem >= 10) { rem -= 10; tens++; }
    buf[(*pos)++] = '0' + tens;
    buf[(*pos)++] = '0' + rem;
}

static void format_field(char *buf, int *pos, uint8_t val, uint8_t interval, int is_hour) {
    if (val == CRON_WILDCARD) {
        buf[(*pos)++] = '*';
        if (interval > 0) {
            buf[(*pos)++] = '/';
            buf[(*pos)++] = '0' + (interval < 10 ? interval : 1);
            if (interval >= 10) {
                buf[(*pos)++] = '0' + (interval - 10);
            }
        }
    } else {
        format_num(buf, pos, val);
        if (interval > 0) {
            buf[(*pos)++] = '/';
            buf[(*pos)++] = '0' + (interval < 10 ? interval : 1);
            if (interval >= 10) {
                buf[(*pos)++] = '0' + (interval - 10);
            }
        }
    }
}

static int handle_list(const void *msg) {
    (void)msg;
    for (uint8_t i = 0; i < g_cron_job_count; i++) {
        char buf[48];
        int pos = 0;
        buf[pos++] = 's'; buf[pos++] = 'l'; buf[pos++] = 'o'; buf[pos++] = 't';
        buf[pos++] = ' ';
        buf[pos++] = '0' + (i < 10 ? i : (i - 10) + 'A');
        buf[pos++] = ':'; buf[pos++] = ' ';
        /* min hour cmd */
        format_field(buf, &pos, g_cron_job_minutes[i], g_cron_job_min_int[i], 0);
        buf[pos++] = ' ';
        format_field(buf, &pos, g_cron_job_hours[i], 0, 1);
        buf[pos++] = ' ';
        buf[pos++] = 'c'; buf[pos++] = 'm'; buf[pos++] = 'd'; buf[pos++] = ':'; buf[pos++] = ' ';
        for (int j = 0; g_cron_job_cmd[i][j] && pos < 44; j++) {
            buf[pos++] = g_cron_job_cmd[i][j];
        }
        buf[pos++] = '\n';
        buf[pos] = '\0';
        cron_debug_output(buf);
    }
    if (g_cron_job_count == 0) {
        cron_debug_output("cron: no jobs\n");
    }
    g_reply.m1_i1 = g_cron_job_count;
    return 0;
}

static int handle_add(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint8_t minutes = (uint8_t)m->m1_i1;
    uint8_t hours = (uint8_t)m->m1_i2;
    uint8_t min_interval = (uint8_t)m->m1_i3;

    if (minutes != CRON_WILDCARD && minutes > 59) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    if (hours != CRON_WILDCARD && hours > 23) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    if (g_cron_job_count >= CRON_MAX_JOBS) {
        g_reply.m1_i1 = -1;
        return -1;
    }

    int slot = g_cron_job_count;
    g_cron_job_minutes[slot] = minutes;
    g_cron_job_hours[slot] = hours;
    g_cron_job_min_int[slot] = min_interval;
    /* Copy command string from m3_ca1 - client enforces max 14 chars */
    {
        const char *cmd = m->m3_ca1;
        int i;
        for (i = 0; i < 14 && cmd[i]; i++) {
            g_cron_job_cmd[slot][i] = cmd[i];
        }
        g_cron_job_cmd[slot][i] = '\0';
    }
    g_cron_job_count++;
    g_reply.m1_i1 = slot;
    return 0;
}

static int handle_del(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint8_t slot = (uint8_t)m->m1_i1;

    if (slot >= g_cron_job_count) {
        g_reply.m1_i1 = -1;
        return -1;
    }

    for (uint8_t i = slot; i < g_cron_job_count - 1; i++) {
        g_cron_job_minutes[i] = g_cron_job_minutes[i + 1];
        g_cron_job_hours[i] = g_cron_job_hours[i + 1];
        g_cron_job_min_int[i] = g_cron_job_min_int[i + 1];
        memcpy(g_cron_job_cmd[i], g_cron_job_cmd[i + 1], 16);
    }
    g_cron_job_count--;
    g_reply.m1_i1 = 0;
    return 0;
}

static int should_launch(uint8_t now_min, uint8_t now_hour,
                         uint8_t job_min, uint8_t job_hour,
                         uint8_t job_int, uint8_t last_min,
                         uint8_t last_hour) {
    if (job_hour != CRON_WILDCARD && job_hour != now_hour) return 0;
    if (job_min == CRON_WILDCARD) {
        if (job_int > 0) {
            uint8_t r = now_min;
            while (r >= job_int) r -= job_int;
            if (r == 0 && now_min != last_min) return 1;
            return 0;
        }
        /* Wildcard with no interval: every minute */
        if (now_min != last_min) return 1;
        return 0;
    }
    if (job_int > 0) {
        if (now_min >= job_min && now_min != last_min) {
            uint8_t r = now_min - job_min;
            while (r >= job_int) r -= job_int;
            if (r == 0) return 1;
        }
        return 0;
    }
    if (now_min == job_min && now_min != last_min) return 1;
    return 0;
}

static void handle_tick(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint8_t now_minute = (uint8_t)m->m1_i1;
    uint8_t now_hour = (uint8_t)m->m1_i2;

    if (!g_cron_running) return;

    for (uint8_t i = 0; i < g_cron_job_count; i++) {
        if (should_launch(now_minute, now_hour, g_cron_job_minutes[i],
                          g_cron_job_hours[i], g_cron_job_min_int[i],
                          g_cron_last_minute, g_cron_last_hour)) {
            int32_t pid = sys_fork();
            if (pid == 0) {
                const char *cmd = g_cron_job_cmd[i];
                char cmdline[16];
                char *argv[8];
                int argc = 0;
                int j;
                for (j = 0; j < (int)sizeof(cmdline) - 1 && cmd[j]; j++) {
                    cmdline[j] = cmd[j];
                }
                cmdline[j] = '\0';
                char *p = cmdline;
                while (*p && argc < 7) {
                    while (*p == ' ') p++;
                    if (*p == '\0') break;
                    argv[argc++] = p;
                    while (*p && *p != ' ') p++;
                    if (*p) *p++ = '\0';
                }
                argv[argc] = (char *)0;
                if (argc > 0) {
                    char pathbuf[32];
                    const char *prefixes[] = { "", "/sbin/", "/bin/", NULL };
                    int launched = 0;
                    for (j = 0; prefixes[j] != NULL; j++) {
                        const char *pp = prefixes[j];
                        char *pb = pathbuf;
                        while (*pp && (pb - pathbuf) < (int)sizeof(pathbuf) - 1) *pb++ = *pp++;
                        const char *cs = argv[0];
                        while (*cs && (pb - pathbuf) < (int)sizeof(pathbuf) - 1) *pb++ = *cs++;
                        *pb = '\0';
                        if (sys_exec(pathbuf, argv) == 0) { launched = 1; break; }
                    }
                    if (!launched) {
                        cron_uart_puts("E:exec fail\r\n");
                    }
                }
                sys_exit(0);
            }
        }
    }
    g_cron_last_minute = now_minute;
    g_cron_last_hour = now_hour;
}

static int cron_server_handle(const void *msg, void *reply) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    muzix_server_message_t *r = (muzix_server_message_t *)reply;
    int result;

    memset(&g_reply, 0, sizeof(g_reply));
    memset(r, 0, sizeof(*r));

    result = -1;
    switch (m->m_type) {
        case CRON_IPC_TICK:
            handle_tick(msg);
            g_reply.m1_i1 = 0;
            result = 0;
            break;
        case CRON_IPC_STATUS:
            result = handle_status(msg);
            break;
        case CRON_IPC_START:
            result = handle_start(msg);
            break;
        case CRON_IPC_STOP:
            result = handle_stop(msg);
            break;
        case CRON_IPC_LIST:
            result = handle_list(msg);
            break;
        case CRON_IPC_ADD:
            result = handle_add(msg);
            break;
        case CRON_IPC_DEL:
            result = handle_del(msg);
            break;
        default:
            g_reply.m1_i1 = -1;
            result = -1;
            break;
    }

    if (result != 0) {
        g_reply.m1_i1 = -1;
    }

    *r = g_reply;
    r->m_source = m->m_source;
    r->m_type = m->m_type;
    return result;
}

static muzix_server_ops_t g_cron_ops = {
    .init = cron_server_init,
    .handle_message = cron_server_handle,
    .cleanup = cron_server_cleanup,
};

int main(void) {
    muzix_server_t server;
    if (muzix_server_init(&server, MUZIX_SERVER_TYPE_CRON, 5, &g_cron_ops) != 0) return 1;
    return muzix_server_run(&server);
}
