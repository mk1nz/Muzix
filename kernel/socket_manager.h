#ifndef MUZIX_SOCKET_MANAGER_H
#define MUZIX_SOCKET_MANAGER_H

#include "ztypes.h"

#include "../mm/mproc_model.h"
#include "../platform/zeta-v2/w5100.h"

/*
 * Kernel-level W5100 socket manager.
 *
 * Three-layer design:
 *   Layer A: W5100 hardware driver (platform/zeta-v2/w5100.c) - register
 *            I/O, open/close/listen/connect/send/recv on a numbered socket.
 *   Layer B: This module - sole authority over allocation, lifetime,
 *            reference counting and permissions of the four physical sockets.
 *   Layer C: User-facing syscall interface (kernel/socket_service.c) -
 *            validates descriptors and permissions, calls Layer B.
 *
 * A socket descriptor is the physical socket number (0-3).  Access is gated
 * by per-process references held in the central table, not by the descriptor
 * value alone, so an unrelated process that guesses a descriptor is refused.
 *
 * Process identity is (slot, generation), so slot reuse can never make a
 * stale reference valid again.
 */

/* Critical-section entry/exit: disable/enable maskable interrupts around
 * short socket-manager metadata operations.  On SDCC/Z80 these are real
 * __asm di/__asm ei blocks; on the host they are no-ops (no interrupts). */
#if defined(__SDCC) && defined(__SDCC_z80)
#define MUZIX_SOCK_ENTER_CRITICAL()  __asm di __endasm
#define MUZIX_SOCK_EXIT_CRITICAL()   __asm ei __endasm
#else
#define MUZIX_SOCK_ENTER_CRITICAL()  do { } while (0)
#define MUZIX_SOCK_EXIT_CRITICAL()   do { } while (0)
#endif

#define MUZIX_SOCKET_MAX     4   /* four physical W5100 sockets */
#define MUZIX_SOCKET_REF_MAX 8   /* one reference per process slot max */

/* Socket lifecycle states. */
#define MUZIX_SOCK_STATE_FREE        0   /* not allocated */
#define MUZIX_SOCK_STATE_ALLOCATED   1   /* allocated, not yet bound/listening */
#define MUZIX_SOCK_STATE_LISTEN      2   /* listening for incoming connections */
#define MUZIX_SOCK_STATE_ESTABLISHED 3   /* TCP connection established */
#define MUZIX_SOCK_STATE_CLOSING     4   /* close in progress, awaiting hardware */
#define MUZIX_SOCK_STATE_QUARANTINED 5   /* hardware state uncertain, not free */

/* Per-reference permission bits. */
#define MUZIX_SOCK_PERM_SEND     0x01   /* may SEND data */
#define MUZIX_SOCK_PERM_RECV     0x02   /* may RECV data */
#define MUZIX_SOCK_PERM_STAT     0x04   /* may query NETSTAT */
#define MUZIX_SOCK_PERM_LISTEN   0x08   /* may issue LISTEN */
#define MUZIX_SOCK_PERM_CONNECT  0x10   /* may issue CONNECT */
#define MUZIX_SOCK_PERM_CLOSE    0x20   /* may release own reference */

/* Standard permission sets. */
#define MUZIX_SOCK_PERM_OWNER    (MUZIX_SOCK_PERM_SEND | MUZIX_SOCK_PERM_RECV | \
                                  MUZIX_SOCK_PERM_STAT | MUZIX_SOCK_PERM_LISTEN | \
                                  MUZIX_SOCK_PERM_CONNECT | MUZIX_SOCK_PERM_CLOSE)
#define MUZIX_SOCK_PERM_CGI_CHILD (MUZIX_SOCK_PERM_SEND | MUZIX_SOCK_PERM_RECV | \
                                   MUZIX_SOCK_PERM_STAT)

/* Error values (match existing ABI conventions). */
#define MUZIX_SOCK_ERR_NONE      0
#define MUZIX_SOCK_ERR_NO_SOCKET -1   /* no physical socket available */
#define MUZIX_SOCK_ERR_INVAL     -2   /* invalid argument */
#define MUZIX_SOCK_ERR_ACCESS    -3   /* permission denied */
#define MUZIX_SOCK_ERR_NOTFOUND  -4   /* socket not allocated */
#define MUZIX_SOCK_ERR_BUSY      -5   /* socket in transitional state */
#define MUZIX_SOCK_ERR_HW        -6   /* hardware failure */

/* One per-process reference to a socket. */
typedef struct {
    uint8_t slot;        /* process table slot */
    uint8_t generation;  /* process generation at reference creation */
    uint8_t permissions; /* MUZIX_SOCK_PERM_* bitmask */
} muzix_sock_ref_t;

/* Central table entry, one per physical W5100 socket. */
typedef struct {
    uint8_t state;           /* MUZIX_SOCK_STATE_* */
    uint8_t protocol;        /* W5100_PROTO_TCP / W5100_PROTO_UDP */
    uint8_t ref_count;       /* number of live process references */
    uint8_t busy;            /* nonzero = hardware operation in flight */
    uint8_t op_slot;         /* process slot that started the in-flight op */
    uint8_t op_gen;          /* generation of that slot */
    muzix_sock_ref_t refs[MUZIX_SOCKET_REF_MAX];
} muzix_socket_entry_t;

/* The central socket table. */
typedef struct {
    muzix_socket_entry_t sockets[MUZIX_SOCKET_MAX];
} muzix_socket_manager_t;

/* Initialise the socket table.  All entries start FREE. */
void muzix_socket_manager_init(muzix_socket_manager_t *mgr);

/* Allocate a physical socket and create a reference for the calling process.
 * Returns the physical socket number (0-3) on success, or a negative error.
 * Caller must be a live process slot. */
int muzix_socket_manager_allocate(muzix_socket_manager_t *mgr,
                                  int slot, uint8_t generation,
                                  uint8_t protocol);

/* Force-free a socket entry in the manager.  Used after net_close() has
 * already closed the socket at the hardware level; without this the socket
 * number stays allocated in the manager and socket() eventually runs out of
 * physical sockets. */
void muzix_socket_manager_drop_socket(muzix_socket_manager_t *mgr, int sock);

/* Check that the process holds a reference with the given permission bit.
 * Returns 0 on success, negative error otherwise. */
int muzix_socket_manager_check_perm(muzix_socket_manager_t *mgr,
                                    int sock, int slot, uint8_t generation,
                                    uint8_t perm);

/* Check permission and acquire an operation reference atomically.
 * Returns 0 on success, negative error otherwise.  On success the socket
 * is busy until muzix_socket_manager_op_end() is called. */
int muzix_socket_manager_check_perm_op(muzix_socket_manager_t *mgr,
                                       int sock, int slot, uint8_t generation,
                                       uint8_t perm);

/* Release an operation reference.  Clears the busy flag; if the socket is
 * pending close (last process reference already released), performs the
 * hardware CMD_CLOSE now. */
void muzix_socket_manager_op_end(muzix_socket_manager_t *mgr,
                                 int sock, int slot, uint8_t generation);

/* Release the calling process's reference to a socket.  When the last
 * reference is released, hardware CMD_CLOSE is issued and the socket returns
 * to FREE (or QUARANTINED if hardware cleanup fails).  If an operation is
 * in flight, the close is deferred until the operation ends.  Returns 0 on
 * success. */
int muzix_socket_manager_release(muzix_socket_manager_t *mgr,
                                 int sock, int slot, uint8_t generation);

/* Fork inheritance: duplicate every reference held by parent_slot into
 * child_slot, with child permissions.  Called from the fork syscall handler
 * after the child slot exists.  Returns 0 on success. */
int muzix_socket_manager_inherit(muzix_socket_manager_t *mgr,
                                 int parent_slot, uint8_t parent_gen,
                                 int child_slot, uint8_t child_gen);

/* Process-exit cleanup: release every reference held by the given slot.
 * Called from all process termination paths. */
void muzix_socket_manager_exit_cleanup(muzix_socket_manager_t *mgr,
                                       int slot, uint8_t generation);

/* Global socket manager instance (defined in socket_manager.c). */
extern muzix_socket_manager_t g_muzix_socket_manager;

#endif /* MUZIX_SOCKET_MANAGER_H */
