#include "libc.h"
#include "socket_manager.h"
#include "proc_table.h"

/* host-test-not-portable: uses Z80 critical-section macros
   (MUZIX_SOCK_ENTER_CRITICAL / MUZIX_SOCK_EXIT_CRITICAL), W5100 hardware
   access, and kernel socket manager types not available on the host */

muzix_socket_manager_t g_muzix_socket_manager;

static int8_t
free_ref_slot(const muzix_socket_entry_t *e) {
    uint8_t i;
    for (i = 0; i < MUZIX_SOCKET_REF_MAX; i++) {
        if (e->refs[i].slot == 0 && e->refs[i].generation == 0) {
            return (int8_t)i;
        }
    }
    return -1;
}

void
muzix_socket_manager_init(muzix_socket_manager_t *mgr) {
    if (!mgr) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
    g_muzix_socket_manager = *mgr;
}

int
muzix_socket_manager_allocate(muzix_socket_manager_t *mgr,
                              int slot, uint8_t generation,
                              uint8_t protocol) {
    muzix_socket_entry_t *e;
    uint8_t s;
    int8_t ri;

    if (!mgr || slot < 0 || (uint8_t)slot >= MUZIX_PROC_TABLE_MAX) {
        return MUZIX_SOCK_ERR_INVAL;
    }
    if (protocol != W5100_PROTO_TCP && protocol != W5100_PROTO_UDP) {
        return MUZIX_SOCK_ERR_INVAL;
    }

    MUZIX_SOCK_ENTER_CRITICAL();
    s = 0;
    while (s < MUZIX_SOCKET_MAX && mgr->sockets[s].state != MUZIX_SOCK_STATE_FREE) {
        s++;
    }
    if (s >= MUZIX_SOCKET_MAX) {
        MUZIX_SOCK_EXIT_CRITICAL();
        return MUZIX_SOCK_ERR_NO_SOCKET;
    }
    e = &mgr->sockets[s];
    e->state = MUZIX_SOCK_STATE_ALLOCATED;
    e->protocol = protocol;
    e->ref_count = 0;
    MUZIX_SOCK_EXIT_CRITICAL();

    if (w5100_open_socket(s, protocol, W5100_DEFAULT_SOURCE_PORT) != 0) {
        MUZIX_SOCK_ENTER_CRITICAL();
        e->state = MUZIX_SOCK_STATE_FREE;
        e->protocol = 0;
        e->ref_count = 0;
        MUZIX_SOCK_EXIT_CRITICAL();
        return MUZIX_SOCK_ERR_HW;
    }

    MUZIX_SOCK_ENTER_CRITICAL();
    ri = free_ref_slot(e);
    if (ri < 0) {
        w5100_close(s);
        e->state = MUZIX_SOCK_STATE_FREE;
        e->protocol = 0;
        e->ref_count = 0;
        MUZIX_SOCK_EXIT_CRITICAL();
        return MUZIX_SOCK_ERR_INVAL;
    }
    e->ref_count = 1;
    e->refs[ri].slot = (uint8_t)slot;
    e->refs[ri].generation = generation;
    e->refs[ri].permissions = MUZIX_SOCK_PERM_OWNER;
    MUZIX_SOCK_EXIT_CRITICAL();

    return (int)s;
}

/* Force-free a socket entry in the manager.  Used by http_reset_slot() after
 * net_close() has already closed the socket at the hardware level; the manager
 * still thinks the socket is allocated, so without this the socket number is
 * leaked and socket() eventually runs out of physical sockets. */
void
muzix_socket_manager_drop_socket(muzix_socket_manager_t *mgr, int sock) {
    if (!mgr || sock < 0 || sock >= MUZIX_SOCKET_MAX) {
        return;
    }
    muzix_socket_entry_t *e = &mgr->sockets[sock];
    e->state = MUZIX_SOCK_STATE_FREE;
    e->protocol = 0;
    e->ref_count = 0;
    memset(e->refs, 0, sizeof(e->refs));
}
