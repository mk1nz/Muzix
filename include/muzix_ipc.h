/* * * Muzix IPC public interface: message structure, result codes,
 * process identity constants, and IPC core API declarations.
 * * Process Identity (critical): there is ONE process number in
 * Muzix, and it is the number the scheduler calls a slot. The
 * array offset `NR_TASKS +` lives in exactly ONE function,
 * muzix_ipc_proc(), and is not a property of the process. See
 * the long note below for the five places that each invented
 * this arithmetic and each got it wrong.
 */

#ifndef MUZIX_IPC_H
#define MUZIX_IPC_H

#include "ztypes.h"

/* IPC operation codes. */
#define MUZIX_IPC_SEND 1
#define MUZIX_IPC_RECEIVE 2
#define MUZIX_IPC_BOTH 3
#define MUZIX_IPC_ANY 255

/* IPC result codes. */
#define MUZIX_IPC_OK 0
#define MUZIX_IPC_EINVAL (-1)
#define MUZIX_IPC_EFAULT (-2)
#define MUZIX_IPC_EBADSRC (-3)
#define MUZIX_IPC_EBADDEST (-4)
#define MUZIX_IPC_EAGAIN (-5)
#define MUZIX_IPC_EINTR (-6)
#define MUZIX_IPC_ENOMEM (-7)

/* Maximum payload bytes carried by an IPC message. */
#define MUZIX_IPC_MAX_PAYLOAD 56

/* Flag bits stored on the IPC proc entry. */
#define MUZIX_IPC_FLAG_RECEIVING 0x01u
#define MUZIX_IPC_FLAG_SENDING 0x02u
#define MUZIX_IPC_FLAG_SLOT_FREE 0x04u
#define MUZIX_IPC_FLAG_DELIVERED 0x08u

/* Numeric base of the negative system-task half of procs[]. Tasks -1..-7
 * (hardware, system, clock, MM, FS, TTY, PM) live at procs[9]..procs[3];
 * get_proc() reaches them as -nr - 1. Do not do that arithmetic anywhere else.
 */
#define MUZIX_IPC_NR_TASKS 10

/* Public server process numbers. These are the ONLY spelling of them.
 * They equal the kernel slots the servers are registered under. */
#define MUZIX_PROC_FS 2
#define MUZIX_PROC_MM 3
#define MUZIX_PROC_PM 4
#define MUZIX_PROC_CRON 5

/* No valid process. Used as "no destination" rather than 0 or -1, because 0
 * is a real slot (the idle slot) and -1 is the hardware task. */
#define MUZIX_PROC_NONE (-100)

/* System-task process numbers (negative, live in procs[0..9] via NR_TASKS
 * offset). MUZIX_TASK_BASE is the first; hardware is -1, system task -2, etc. */
#define MUZIX_TASK_BASE (-10)
#define MUZIX_HARDWARE (-1)
#define MUZIX_SYSTEM_TASK (-2)
#define MUZIX_CLOCK_TASK (-3)
#define MUZIX_MM_TASK (-4)
#define MUZIX_FS_TASK (-5)
#define MUZIX_TTY_TASK (-6)
#define MUZIX_PM_TASK (-7)

/* =====================================================================
 * PROCESS IDENTITY - READ THIS BEFORE ADDING OR CHANGING A NUMBER
 * =====================================================================
 * * There is ONE process number in Muzix, and it is the number the scheduler
 * calls a slot. A user process and the IPC entry that describes it share it.
 * * The array offset NR_TASKS + (10) lives in exactly ONE function,
 * muzix_ipc_proc(), and is NOT a property of the process. Five places once
 * treated the offset as the process number itself, each got it wrong, and
 * each was invisible until the machine ran:
 * * - ipc_slot = 10 + current_slot, used as `caller` -> get_proc() computed
 * procs[10 + (10 + slot)] = procs[20 + slot], a dead entry - every send
 * and receive failed.
 * - muzix_ipc_server_slot() returned 10 + slot, fed as a routed syscall
 * destination.
 * - ipc_client = msg.m_source, unconverted -> SRV_COPY handlers rejected it
 * with EFAULT.
 * - shell/shell.c #define FS_SERVER_IPC_SLOT 11, a hand-written guess.
 * - lib/server.c server->ipc_slot = 10 + id, dead code nothing read.
 * * THE RULE:
 * 1. The process number IS the kernel proc-table slot - same integer.
 * 2. The array offset lives only in muzix_ipc_proc().
 * 3. Server numbers are named constants here, defined once.
 * =====================================================================
 */

/* The IPC message - the payload that crosses process boundaries. Source and
 * dest are process numbers; type is an IPC operation code; param is a tagged
 * union so the same 8-byte payload can carry integers, longs, bytes, or
 * pointers depending on the call. */
struct muzix_ipc_message {
 int16_t m_source; /*< Sending process number */
 int16_t m_type; /*< IPC operation code */
 union {
 struct {
 int16_t m1_i1, m1_i2, m1_i3;
 uint16_t m1_p1, m1_p2, m1_p3;
 } m_m1;
 struct {
 int16_t m2_i1, m2_i2, m2_i3;
 int32_t m2_l1, m2_l2;
 uint16_t m2_p1;
 } m_m2;
 struct {
 int16_t m3_i1, m3_i2;
 uint16_t m3_p1;
 char m3_ca1[14];
 } m_m3;
 struct {
 int32_t m4_l1, m4_l2, m4_l3, m4_l4;
 } m_m4;
 struct {
 int8_t m5_c1, m5_c2;
 int16_t m5_i1, m5_i2;
 int32_t m5_l1, m5_l2, m5_l3;
 } m_m5;
 struct {
 int16_t m6_i1, m6_i2, m6_i3;
 int32_t m6_l1;
 void (*m6_f1)(void);
 } m_m6;
 } m_u; /*< Call-specific payload */
};
typedef struct muzix_ipc_message muzix_ipc_message_t;

/* Maximum payload bytes carried by an IPC message. */
#define MESS_SIZE (sizeof(muzix_ipc_message_t))

/* Message field access macros - named access to the union members.
 * Defined after the struct so member names m1_i1 etc. are not expanded
 * by the preprocessor while the struct is being parsed. */
#define m1_i1 m_u.m_m1.m1_i1
#define m1_i2 m_u.m_m1.m1_i2
#define m1_i3 m_u.m_m1.m1_i3
#define m1_p1 m_u.m_m1.m1_p1
#define m1_p2 m_u.m_m1.m1_p2
#define m1_p3 m_u.m_m1.m1_p3

#define m2_i1 m_u.m_m2.m2_i1
#define m2_i2 m_u.m_m2.m2_i2
#define m2_i3 m_u.m_m2.m2_i3
#define m2_l1 m_u.m_m2.m2_l1
#define m2_l2 m_u.m_m2.m2_l2
#define m2_p1 m_u.m_m2.m2_p1

#define m3_i1 m_u.m_m3.m3_i1
#define m3_i2 m_u.m_m3.m3_i2
#define m3_p1 m_u.m_m3.m3_p1
#define m3_ca1 m_u.m_m3.m3_ca1

#define m4_l1 m_u.m_m4.m4_l1
#define m4_l2 m_u.m_m4.m4_l2
#define m4_l3 m_u.m_m4.m4_l3
#define m4_l4 m_u.m_m4.m4_l4

#define m5_c1 m_u.m_m5.m5_c1
#define m5_c2 m_u.m_m5.m5_c2
#define m5_i1 m_u.m_m5.m5_i1
#define m5_i2 m_u.m_m5.m5_i2
#define m5_l1 m_u.m_m5.m5_l1
#define m5_l2 m_u.m_m5.m5_l2
#define m5_l3 m_u.m_m5.m5_l3

#define m6_i1 m_u.m_m6.m6_i1
#define m6_i2 m_u.m_m6.m6_i2
#define m6_i3 m_u.m_m6.m6_i3
#define m6_l1 m_u.m_m6.m6_l1
#define m6_f1 m_u.m_m6.m6_f1

/* Forward declaration of the per-process IPC entry. */
typedef struct muzix_ipc_proc muzix_ipc_proc_t;

/* * Per-process IPC entry. One per proc-table slot.
 * * p_msg survives a process switch because it lives in the
 * table, not on any stack. p_user_addr records the caller's
 * userspace buffer address so that a delivered message can be
 * copied out at resume time, on a kernel stack that is still
 * live.
 */
struct muzix_ipc_proc {
 uint8_t p_id; /*< Process id */
 int16_t p_pid; /*< Process id number */
 uint8_t p_flags; /*< Flag bits (MUZIX_IPC_FLAG_*) */
 int16_t p_getfrom; /*< Source a blocked receive waits for */
 int16_t p_nr; /*< Reserved padding, never state */
 muzix_ipc_message_t *p_messbuf; /*< Incoming message buffer */
 muzix_ipc_proc_t *p_callerq; /*< Caller queue link */
 muzix_ipc_proc_t *p_sendlink; /*< Send queue link */
 muzix_ipc_proc_t *p_nextready; /*< Ready queue link */
 uint16_t p_reg[8]; /*< Saved register state */
 muzix_ipc_message_t p_msg; /*< Message that survives a switch */
 uint16_t p_user_addr; /*< Caller's userspace buffer address */
 uint16_t _pad; /*< Pad entry to 32 bytes for power-of-2 indexing */
};

/* * IPC table: process array, ready queues, global state.
 */
struct muzix_ipc_table {
 muzix_ipc_proc_t procs[26]; /*< Process entries (10 system-task + 16 user) */
 uint8_t proc_count;
 muzix_ipc_proc_t *rdy_head[3]; /*< Ready queue heads (TASK, SERVER, USER) */
 muzix_ipc_proc_t *rdy_tail[3]; /*< Ready queue tails */
 int16_t cur_proc; /*< Current process number */
 int16_t prev_proc; /*< Previous process number */
 muzix_ipc_proc_t *proc_ptr; /*< Scan pointer */
 muzix_ipc_proc_t *bill_ptr; /*< Billing pointer */
 uint8_t busy_map; /*< Bitmap of busy slots */
 muzix_ipc_message_t *task_mess[10]; /*< System-task message buffers */
 int sig_procs; /*< Count of processes with pending signals */
};
typedef struct muzix_ipc_table muzix_ipc_table_t;

/* ------------------------------------------------------------------------- */
/* IPC core API */
/* ------------------------------------------------------------------------- */

/* * Initialise an IPC table.
 * \param[in,out] ipc: IPC table to initialise
 */
void muzix_ipc_init(muzix_ipc_table_t *ipc);

/* * Send a message to a destination (send-only).
 * ipc: IPC table
 * sender: sending process number
 * dest: destination process number
 * msg: message to send
 * MUZIX_IPC_OK on success, negative error code otherwise
 */
int muzix_ipc_send(muzix_ipc_table_t *ipc, int16_t sender, int16_t dest,
 muzix_ipc_message_t *msg);

/* * Receive a message (receive-only).
 * ipc: IPC table
 * receiver: receiving process number
 * src: source to receive from, or MUZIX_IPC_ANY
 * msg: buffer to receive into
 * MUZIX_IPC_OK on success, negative error code otherwise
 */
int muzix_ipc_receive(muzix_ipc_table_t *ipc, int16_t receiver, int16_t src,
 muzix_ipc_message_t *msg);

/* * Send and receive (sendrec).
 * ipc: IPC table
 * caller: calling process number
 * src_dest: destination/source slot
 * \param[in,out] msg: message to send, and buffer for the reply
 * MUZIX_IPC_OK on success, negative error code otherwise
 */
int muzix_ipc_sendrec(muzix_ipc_table_t *ipc, int16_t caller, int16_t src_dest,
 muzix_ipc_message_t *msg);

/* * Schedule: pick the next ready process.
 * ipc: IPC table
 */
void muzix_ipc_schedule(muzix_ipc_table_t *ipc);

/* * Mark a process as ready.
 * ipc: IPC table
 * rp: process entry to ready
 */
void muzix_ipc_ready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp);

/* * Mark a process as not ready.
 * ipc: IPC table
 * rp: process entry to unready
 */
void muzix_ipc_unready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp);

/* * The only sanctioned way to index procs[] by process number.
 * This keeps the `NR_TASKS +` array offset in one place.
 * ipc: IPC table
 * nr: process number
 * Pointer to the process entry, or NULL if out of range
 */
muzix_ipc_proc_t *muzix_ipc_proc(muzix_ipc_table_t *ipc, int16_t nr);

/* * Deprecated wrapper over muzix_ipc_proc().
 * ipc: IPC table
 * nr: process number
 * Pointer to the process entry, or NULL if out of range
 * \deprecated Use muzix_ipc_proc() instead.
 */
muzix_ipc_proc_t *muzix_ipc_get_proc(muzix_ipc_table_t *ipc, int16_t nr);

/* * Recover a process number from a process entry pointer.
 * ipc: IPC table
 * rp: process entry pointer
 * Process number
 */
int16_t muzix_ipc_proc_nr(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp);

/* * Return the global IPC table pointer.
 * Pointer to the global IPC table
 */
muzix_ipc_table_t *muzix_ipc_get_global(void);

/* * Register a server process under its kernel slot.
 * ipc: IPC table
 * kernel_slot: kernel slot number
 * server_id: server id number
 * 0 on success, -1 on failure
 */
int muzix_ipc_register_server(muzix_ipc_table_t *ipc, int16_t kernel_slot,
 int16_t server_id);

/* * Unregister a server process.
 * ipc: IPC table
 * kernel_slot: kernel slot number
 */
void muzix_ipc_unregister_server(muzix_ipc_table_t *ipc, int16_t kernel_slot);

/* * Set a process's message buffer.
 * ipc: IPC table
 * nr: process number
 * buf: message buffer pointer
 * 0 on success, -1 on failure
 */
int muzix_ipc_set_messbuf(muzix_ipc_table_t *ipc, int16_t nr,
 muzix_ipc_message_t *buf);

#endif /* MUZIX_IPC_H */
