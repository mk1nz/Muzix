#include "server.h"
#include "syscall.h"
#include <string.h>

#define MUZIX_IPC_ANY 255

/*
 * "No source" for a received message.
 *
 * This file does not include include/muzix_ipc.h - it defines its own message
 * struct and its own MUZIX_IPC_ANY above - so it cannot reach MUZIX_PROC_NONE
 * from there.  Defining a local spelling in the same style as MUZIX_IPC_ANY is
 * consistent with how this module is written.
 *
 * The value matters: m_source is the kernel slot of the process that sent a
 * request, and slot 0 is the idle slot, which never sends.  So a message whose
 * source is still 0 after a receive did not carry one, and replying to 0 would
 * address a slot nobody registered - the same class of mistake as a server
 * number invented by hand.  The reply path refuses to run in that case.
 */
#define MUZIX_IPC_NO_SOURCE 0

static muzix_server_t *g_servers[4] = {0};
static int g_server_count = 0;

int muzix_server_init(muzix_server_t *server, uint8_t type, uint8_t id, muzix_server_ops_t *ops) {
    if (!server || !ops || g_server_count >= 4) return -1;

    memset(server, 0, sizeof(*server));
    server->server_type = type;
    server->server_id = id;
    /* Deliberately NOT 10 + id, which is what this read as and what made two
     * unrelated numbering schemes.
     *
     * `10 + slot` is the ARRAY INDEX that muzix_ipc_register_server() writes,
     * because muzix_ipc_proc() reaches a process as procs[NR_TASKS + nr] and
     * NR_TASKS is 10.  A process NUMBER is what send/receive take, and it is
     * the plain kernel slot.
     *
     * A server writes its own number only if the kernel told it, which it does
     * not - it registers it by slot itself.  So a userspace server genuinely
     * cannot know its number at init time, and encoding a wrong one here is
     * worse than encoding none.  This field stays 0 until something authoritative
     * sets it; nothing reads it today, and if something ever does, it should
     * get the number from agree_msg/m_source rather than from here.
     *
     * See include/muzix_ipc.h for the full list of the five places that each
     * invented this arithmetic. */
    server->ipc_slot = 0;
    server->active = 1;
    server->ops = ops;

    g_servers[g_server_count++] = server;
    return 0;
}

/*
 * Receive one message into `out`.
 *
 * A pure receive: 255 is MUZIX_IPC_ANY, so the kernel's receive-only path runs
 * and copies the delivered message into the buffer.  There is no separate
 * SYS_IPC_RECEIVE in the ABI - the receive-only case of SYS_IPC_SENDREC is it.
 *
 * This exists as its own helper, rather than calling server_sendrec() with a
 * zeroed buffer, because the two directions need DIFFERENT buffers and the
 * shared helper cannot express that.  See the note in muzix_server_run().
 */
static int server_recv(int dest_id, muzix_server_message_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    return sys_ipc_sendrec(dest_id, out);
}

static int server_sendrec(int dest_id, int unused, const muzix_server_message_t *msg, muzix_server_message_t *reply) {
    (void)unused;
    muzix_server_message_t kmsg;
    memcpy(&kmsg, msg, sizeof(kmsg));

    /* Reply path.  dest_id is msg.m_source - the process that sent us the
     * request - and the buffer carries the reply, not the request.  This is the
     * only direction that ever worked: the message is built here in userspace,
     * sent, and the kernel's copy-in takes it from the caller's own buffer.
     *
     * The send is sys_ipc_send(), not sys_ipc_sendrec().  A sendrec to the
     * client runs a mini_rec from that same peer underneath (the else branch
     * of muzix_handle_ipc_sendrec), which leaves this server waiting on its
     * client - and the client's next request is then delivered into THIS dead
     * buffer by mini_send, instead of into the server's own ANY-receive, where
     * nothing finds it.  Measured: five byte round trips and then the shell's
     * sixth read parked forever, because handle_read never saw the request
     * that had landed in this frame.  MINIX replies with send() and leaves the
     * next request to the server's receive(); so does this. */
    int result = sys_ipc_send(dest_id, &kmsg);
    if (result >= 0 && reply) {
        memcpy(reply, &kmsg, sizeof(kmsg));
    }
    return result;
}

int muzix_server_send(muzix_server_t *server, int dest_type, int dest_id, const void *msg, void *reply) {
    (void)server;
    (void)dest_type;
    (void)dest_id;
    (void)msg;
    (void)reply;
    return -1;
}
int muzix_server_run(muzix_server_t *server)
{
    if (!server || !server->active || !server->ops) return -1;

    if (server->ops->init) {
        server->ops->init();
    }

    while (server->active) {
        /*
         * TWO SEPARATE BUFFERS.  This is the fix for the bug that made every
         * handler read uninitialised stack, and it is worth writing down
         * because the wrong version looks perfectly reasonable.
         *
         * The old loop declared one `msg` and one `reply` at function scope,
         * called server_sendrec(255, 0, &msg, &reply), and handed the handler
         * `&msg`.  But `msg` was never written by anything - it was only ever
         * READ, memcpy'd into kmsg as the outgoing half of the syscall.  The
         * kernel copies the DELIVERED message into kmsg and then out into
         * `reply`, so the request was sitting in `reply` the whole time and
         * `msg` stayed stack garbage.
         *
         * Worse, the buffers were at function scope and reused across
         * iterations, so even a correct request was overwritten by the next
         * receive before the handler had finished with it.
         *
         * `in` holds the request, `out` holds the reply, and neither is the
         * other.  A handler may read every field of `in` and write every field
         * of `out` without stepping on itself - which matters here because
         * fs_server_handle() starts with memset(out), which would destroy an
         * aliased request.
         */
        muzix_server_message_t in;
        muzix_server_message_t out;

        int result = server_recv(MUZIX_IPC_ANY, &in);

        if (result < 0) {
            /* Blocked or error.  The kernel resumed us without a delivered
             * message; retry rather than treat it as a request. */
            continue;
        }

        memset(&out, 0, sizeof(out));

        if (server->ops->handle_message) {
            server->ops->handle_message(&in, &out);
            if (in.m_source != MUZIX_IPC_NO_SOURCE) {
                server_sendrec(in.m_source, 0, &out, NULL);
            }
        }
    }

    if (server->ops->cleanup) {
        server->ops->cleanup();
    }
    return 0;
}

void muzix_server_exit(muzix_server_t *server) {
    if (server) {
        server->active = 0;
    }
}

/* Userspace copy functions using syscalls */
int muzix_copy_to_user(uint16_t addr, const void *buf, size_t len) {
    return sys_ipc_srv_copy_to_client(addr, buf, len);
}

int muzix_copy_from_user(uint16_t addr, void *buf, size_t len) {
    return sys_ipc_srv_copy_from_client(addr, buf, len);
}