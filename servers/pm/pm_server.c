#include "server.h"
#include "syscall.h"
#include "proc_info.h"
#include <string.h>

#define MUZIX_PROC_TABLE_MAX 8

static muzix_server_message_t g_reply;

/* PM Server - userspace process management using kernel syscalls */

static int pm_server_init(void) {
    return 0;
}

static void pm_server_cleanup(void) {
}

static int handle_fork(const void *msg) {
    (void)msg;
    /* sys_fork takes no args, returns child pid */
    int32_t pid = sys_fork();
    g_reply.m1_i1 = pid;
    return 0;
}

static int handle_wait(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    int32_t status = 0;
    int32_t pid = sys_wait(&status);
    if (pid < 0) {
        g_reply.m1_i1 = -1;
        return 0;
    }
    if (m->m1_p1) {
        if (sys_ipc_srv_copy_to_client(m->m1_p1, &status, sizeof(status)) != 0) return -1;
    }
    g_reply.m1_i1 = pid;
    return 0;
}

static int handle_exit(const void *msg) {
    (void)msg;
    /* Do NOT call sys_exit() here.  Exit is the kernel's own SYS_EXIT
     * interception (kernel/syscalls_ipc.c, muzix_handle_exit()): a client that
     * wants to die goes through sys_exit() and the kernel answers it there,
     * so a request that still reaches this handler did not come from that
     * path.  Answering it with sys_exit() would run the kernel's exit on THIS
     * server - the process manager killing itself on a client's behalf, on a
     * status the client chose.  Reply an error instead and leave the server
     * running; the kernel-side path is the only legitimate exit route. */
    g_reply.m1_i1 = -1;
    return 0;
}

static int handle_getpid(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = sys_getpid();
    return 0;
}

static int handle_getppid(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = sys_getppid();
    return 0;
}

static int handle_getrtc(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    if (!m->m1_p1) return -1;
    int32_t rc = sys_getrtc((void *)m->m1_p1);
    if (rc == 0 && m->m1_i2 && m->m1_p2) {
        rc = sys_getrtc_since((void *)m->m1_p1, (void *)m->m1_p2);
    }
    g_reply.m1_i1 = rc == 0 ? 0 : -1;
    return 0;
}

static int handle_setrtc(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    if (!m->m1_p1) return -1;
    int32_t rc = sys_setrtc((const void *)m->m1_p1);
    g_reply.m1_i1 = rc == 0 ? 0 : -1;
    return 0;
}

static int handle_times(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    struct proc_info info;
    int32_t rc = sys_proctab(&info, 0);
    if (rc == 0 && m->m1_p1) {
        if (sys_ipc_srv_copy_to_client(m->m1_p1, &info, sizeof(info)) != 0) return -1;
    }
    g_reply.m1_i1 = rc;
    return 0;
}

static int handle_proctab(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    struct proc_info info;
    int32_t slot = m->m1_i1;
    if (slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) return -1;
    int32_t rc = sys_proctab(&info, slot);
    if (rc == 0 && m->m1_p1) {
        if (sys_ipc_srv_copy_to_client(m->m1_p1, &info, sizeof(info)) != 0) return -1;
    }
    g_reply.m1_i1 = rc;
    return 0;
}

static int handle_load(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint32_t windows[3];
    int32_t rc = sys_load(windows);
    if (rc == 0 && m->m1_p1) {
        if (sys_ipc_srv_copy_to_client(m->m1_p1, windows, sizeof(windows)) != 0) return -1;
    }
    g_reply.m1_i1 = rc;
    return 0;
}

static int handle_getuid(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = sys_getuid();
    return 0;
}

static int handle_getgid(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = sys_getgid();
    return 0;
}

static int handle_setuid(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    g_reply.m1_i1 = sys_setuid(m->m1_i1);
    return 0;
}

static int handle_setgid(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    g_reply.m1_i1 = sys_setgid(m->m1_i1);
    return 0;
}

static int handle_umask(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    g_reply.m1_i1 = sys_umask(m->m1_i1);
    return 0;
}

static int handle_signal(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    g_reply.m1_i1 = sys_signal(m->m1_i1, m->m1_i2);
    return 0;
}

static int pm_server_handle(const void *msg, void *reply) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    muzix_server_message_t *r = (muzix_server_message_t *)reply;
    memset(r, 0, sizeof(*r));
    r->m_source = m->m_source;
    r->m_type = m->m_type;

    int result = -1;
    switch (m->m_type) {
        case 1:  result = handle_fork(msg); break;      /* SYS_FORK */
        case 3:  result = handle_exit(msg); break;      /* SYS_EXIT */
        case 4:  result = handle_wait(msg); break;      /* SYS_WAIT */
        case 5:  result = handle_getpid(msg); break;    /* SYS_GETPID */
        case 6:  result = handle_getppid(msg); break;   /* SYS_GETPPID */
        case 9:  result = handle_getrtc(msg); break;    /* SYS_GETRTC */
        case 15: result = handle_setrtc(msg); break;    /* SYS_SETRTC */
        case 8:  result = handle_times(msg); break;     /* SYS_TIMES */
        case 16: result = handle_proctab(msg); break;   /* SYS_PROCTAB */
        case 48: result = handle_load(msg); break;      /* SYS_LOAD */
        case 42: result = handle_getuid(msg); break;    /* SYS_GETUID */
        case 43: result = handle_getgid(msg); break;    /* SYS_GETGID */
        case 44: result = handle_setuid(msg); break;    /* SYS_SETUID */
        case 45: result = handle_setgid(msg); break;    /* SYS_SETGID */
        case 60: result = handle_umask(msg); break;     /* SYS_UMASK */
        case 31: result = handle_signal(msg); break;    /* SYS_SIGNAL */
        default: result = -1; break;
    }

    if (result != -1) {
        *r = g_reply;
    }
    return result;
}

static muzix_server_ops_t g_pm_ops = {
    .init = pm_server_init,
    .handle_message = pm_server_handle,
    .cleanup = pm_server_cleanup,
};

int main(void) {
    muzix_server_t server;
    muzix_server_init(&server, MUZIX_SERVER_TYPE_PM, 3, &g_pm_ops);
    return muzix_server_run(&server);
}