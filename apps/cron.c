/* host-test-not-portable: uses sys_ipc_sendrec, a Z80 syscall stub, and the
 * server-side message type shared with servers/cron/cron_server.c */
#include "../lib/syscall.h"
#include "../lib/server.h"
#include "../lib/libc.h"
#include "libc.h"

#define CRON_SERVER_SLOT 5

#define CRON_IPC_STATUS 0x6b
#define CRON_IPC_START  0x69
#define CRON_IPC_STOP   0x6a
#define CRON_IPC_LIST   0x67
#define CRON_IPC_ADD    0x65
#define CRON_IPC_DEL    0x66
#define CRON_WILDCARD   0xFF

static void out_str(const char *s) {
    while (*s) {
        char c = *s++;
        write(1, &c, 1);
    }
}

static int32_t cron_send_ipc(int32_t msg_type, int32_t arg1, int32_t arg2, int32_t arg3, const char *cmd) {
    muzix_server_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.m_type = (int16_t)msg_type;
    msg.m_source = CRON_SERVER_SLOT;
    msg.m1_i1 = (int16_t)arg1;
    msg.m1_i2 = (int16_t)arg2;
    msg.m1_i3 = (int16_t)arg3;
    if (cmd) {
        int i;
        /* m3_ca1 holds exactly 14 bytes; copy at most 13 chars + null terminator */
        for (i = 0; i < 13 && cmd[i]; i++) {
            msg.m3_ca1[i] = cmd[i];
        }
        msg.m3_ca1[i] = '\0';
    }
    return sys_ipc_sendrec(CRON_SERVER_SLOT, &msg);
}

static int32_t cron_send_simple(int32_t msg_type, int32_t arg1) {
    return cron_send_ipc(msg_type, arg1, 0, 0, 0);
}

static int32_t cron_send_empty(int32_t msg_type) {
    return cron_send_ipc(msg_type, 0, 0, 0, 0);
}

static int parse_cron_field(const char *s, int *val, int *interval, int max) {
    *interval = 0;
    if (s[0] == '*') {
        *val = CRON_WILDCARD;
        if (s[1] == '/') {
            int n = atoi(s + 2);
            if (n <= 0 || n > max) return -1;
            *interval = n;
        }
        return 0;
    }
    int n = atoi(s);
    if (n < 0 || n > max) return -1;
    *val = n;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        out_str("cron: usage: status|start|stop|list|add <min> <hour> <cmd>|del <slot>\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        int32_t s = cron_send_empty(CRON_IPC_STATUS);
        if (s < 0) out_str("cron: error\n");
        else if (s == 1) out_str("cron: running\n");
        else out_str("cron: stopped\n");
        return 0;
    }

    if (strcmp(argv[1], "start") == 0) {
        cron_send_empty(CRON_IPC_START);
        out_str("cron: started\n");
        return 0;
    }

    if (strcmp(argv[1], "stop") == 0) {
        cron_send_empty(CRON_IPC_STOP);
        out_str("cron: stopped\n");
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        cron_send_empty(CRON_IPC_LIST);
        return 0;
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc < 5) { out_str("cron: add <min> <hour> <cmd>\n"); return 1; }
        int min, hour, min_int, hour_int;
        if (parse_cron_field(argv[2], &min, &min_int, 59) < 0) {
            out_str("cron: invalid minutes\n");
            return 1;
        }
        if (parse_cron_field(argv[3], &hour, &hour_int, 23) < 0) {
            out_str("cron: invalid hour\n");
            return 1;
        }
        char cmdline[16];
        {
            int pos = 0;
            int i;
            for (i = 4; i < argc; i++) {
                const char *s = argv[i];
                while (*s) {
                    if (pos >= 13) {
                        out_str("cron: command too long (max 13 chars)\n");
                        return 1;
                    }
                    cmdline[pos++] = *s++;
                }
                if (i < argc - 1) {
                    if (pos >= 13) {
                        out_str("cron: command too long (max 13 chars)\n");
                        return 1;
                    }
                    cmdline[pos++] = ' ';
                }
            }
            cmdline[pos] = '\0';
        }
        int32_t r = cron_send_ipc(CRON_IPC_ADD, min, hour, min_int, cmdline);
        if (r < 0) { out_str("cron: add failed\n"); return 1; }
        out_str("cron: added at slot ");
        char buf[4];
        buf[0] = '0' + (char)r;
        buf[1] = '\n';
        buf[2] = '\0';
        out_str(buf);
        return 0;
    }

    if (strcmp(argv[1], "del") == 0) {
        if (argc < 3) { out_str("cron: del <slot>\n"); return 1; }
        int32_t r = cron_send_simple(CRON_IPC_DEL, atoi(argv[2]));
        if (r < 0) { out_str("cron: del failed\n"); return 1; }
        out_str("cron: deleted\n");
        return 0;
    }

    out_str("cron: unknown command\n");
    return 1;
}
