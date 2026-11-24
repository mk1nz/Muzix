/* * * IPC core for the Muzix microkernel: message-passing layer
 * with its own process identity model (the IPC slot number IS
 * the kernel proc-table slot) and its own syscall ABI.
 * * Not a MINIX 1.0 port. Inspired by selected MINIX 1.0
 * concepts, but independent.
 * * host-test-not-portable: uses Z80 `di`/`ei` assembly
 * instructions for interrupt locking, not available in host
 * compilation. Compiled only for the Z80 target.
 */

#include "../include/muzix_ipc.h"
#include "libc.h"

/* Ready queue indices (local spelling of the proc_table values). */
#define TASK_Q 0
#define SERVER_Q 1
#define USER_Q 2
/* Number of system-task entries at the front of procs[]. */
#define NR_TASKS 10
/* Total process entries in procs[]. */
#define NR_PROCS 16
/* Lowest user process number. */
#define LOW_USER 0
/* Null process pointer. */
#define NIL_PROC ((muzix_ipc_proc_t *)0)
/* Idle process number (no valid process). */
#define IDLE (-100)
/* Hardware task number. */
#define HARDWARE (-1)

/* IPC call succeeded. */
#define OK 0
/* Bad source process. */
#define E_BAD_SRC (-3)
/* Bad destination process. */
#define E_BAD_DEST (-4)
/* No permission. */
#define E_NO_PERM (-5)

#define RECEIVING MUZIX_IPC_FLAG_RECEIVING
#define SENDING MUZIX_IPC_FLAG_SENDING
#define P_SLOT_FREE MUZIX_IPC_FLAG_SLOT_FREE

#define RET_REG 0

#define CLICK_SHIFT 4

/* * Convert a process number to a procs[] array index.
 * * A process number nr does not index procs[] from zero: the low half of the
 * array holds the negative system-task numbers, so nr sits at array index
 * NR_TASKS + nr and a task nr sits at -nr - 1. That offset is a property of
 * the ARRAY, not of the process (see AGENTS.md "Process Identity"), so it is
 * computed here and nowhere else - muzix_ipc_proc() is the only code in the
 * tree that indexes procs[].
 * * nr: process number
 * Array index into procs[]
 */
static int muzix_ipc_index(int16_t nr)
{
 if (nr < 0) {
 return -nr - 1;
 }
 /* NR_TASKS is the array offset for user processes. Only this helper and
 * muzix_ipc_proc() may apply it; everything else works in process numbers. */
 int offset = NR_TASKS;
 return offset + nr;
}

/* * Convert a procs[] array index back to a process number.
 * idx: array index
 * Process number
 */
static int muzix_ipc_nr_of_index(int idx)
{
 if (idx < NR_TASKS) {
 return -idx - 1;
 }
 return idx - NR_TASKS;
}

/* Disable interrupts. */
static void lock(void)
{
 __asm__("di");
}

/* Re-enable interrupts. */
static void restore(void)
{
 __asm__("ei");
}

/* * Copy a message from source to destination.
 * src: source process number (unused)
 * src_phys: source physical address (unused)
 * src_ptr: source message pointer
 * dst: destination process number (unused)
 * dst_phys: destination physical address (unused)
 * dst_ptr: destination message pointer
 */
static void cp_mess(int16_t src, uint16_t src_phys, muzix_ipc_message_t *src_ptr,
 int16_t dst, uint16_t dst_phys, muzix_ipc_message_t *dst_ptr)
{
 (void)src;
 (void)src_phys;
 (void)dst;
 (void)dst_phys;
 *dst_ptr = *src_ptr;
}

/* * Initialise an IPC table.
 * \param[in,out] ipc: IPC table to initialise
 * * Marks all 26 entries free (P_SLOT_FREE), because a
 * zeroed entry and a registered one must be distinguishable:
 * memset leaves p_flags == 0, which is the flag set of a
 * REGISTERED entry, so the guard
 * `rp->p_flags & P_SLOT_FREE` was never true without this.
 */
void muzix_ipc_init(muzix_ipc_table_t *ipc)
{
 int i;
 int last_task_index;

 if (!ipc) {
 return;
 }
 memset(ipc, 0, sizeof(*ipc));
 ipc->proc_count = 0;
 for (i = 0; i < 26; i++) {
 ipc->procs[i].p_flags = P_SLOT_FREE;
 }
 for (i = 0; i < 3; i++) {
 ipc->rdy_head[i] = NIL_PROC;
 ipc->rdy_tail[i] = NIL_PROC;
 }
 ipc->cur_proc = IDLE;
 ipc->prev_proc = IDLE;
 last_task_index = NR_TASKS - 1;
 ipc->proc_ptr = &ipc->procs[last_task_index];
 ipc->bill_ptr = ipc->proc_ptr;
}

/* * The only sanctioned way to index procs[] by process number.
 * ipc: IPC table
 * nr: process number
 * Pointer to the process entry, or NIL_PROC if out of range
 */
muzix_ipc_proc_t *muzix_ipc_proc(muzix_ipc_table_t *ipc, int16_t nr)
{
 if (!ipc) {
 return NIL_PROC;
 }
 if (nr < -NR_TASKS || nr >= NR_PROCS) {
 return NIL_PROC;
 }
 return &ipc->procs[muzix_ipc_index(nr)];
}

/* * Recover a process number from a process entry pointer.
 * * Compares against each slot instead of dividing a pointer
 * difference: `rp - ipc->procs` compiles to a 16-bit division
 * by sizeof(muzix_ipc_proc_t), which calls __divsint - and
 * __divsint is only defined in lib/math.s, a userspace library
 * the kernel does not link.
 * ipc: IPC table
 * rp: process entry pointer
 * Process number, or -1 if not found
 */
int16_t muzix_ipc_proc_nr(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp)
{
 int16_t i;

 if (!ipc || !rp) {
 return -1;
 }
 for (i = 0; i < 26; i++) {
 if (&ipc->procs[i] == rp) {
 return (int16_t)muzix_ipc_nr_of_index(i);
 }
 }
 return -1;
}

/* * Mark a process as ready and enqueue it.
 * \param[in,out] ipc: IPC table
 * rp: process entry to ready
 */
void muzix_ipc_ready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp)
{
 int16_t r;
 int q;

 if (!ipc || !rp || rp == NIL_PROC) {
 return;
 }

 lock();
 r = muzix_ipc_proc_nr(ipc, rp);
 q = (r < 0 ? TASK_Q : r < LOW_USER ? SERVER_Q : USER_Q);

 if (ipc->rdy_head[q] == NIL_PROC) {
 ipc->rdy_head[q] = rp;
 } else {
 ipc->rdy_tail[q]->p_nextready = rp;
 }
 ipc->rdy_tail[q] = rp;
 rp->p_nextready = NIL_PROC;
 restore();
}

/* * Mark a process as not ready.
 * \param[in,out] ipc: IPC table
 * rp: process entry to unready
 */
void muzix_ipc_unready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp)
{
 int16_t r;
 int q;
 muzix_ipc_proc_t *xp;

 if (!ipc || !rp || rp == NIL_PROC) {
 return;
 }

 lock();
 r = muzix_ipc_proc_nr(ipc, rp);
 q = (r < 0 ? TASK_Q : r < LOW_USER ? SERVER_Q : USER_Q);

 xp = ipc->rdy_head[q];
 if (xp == NIL_PROC) {
 restore();
 return;
 }
 if (xp == rp) {
 ipc->rdy_head[q] = xp->p_nextready;
 } else {
 while (xp->p_nextready != rp) {
 if ((xp = xp->p_nextready) == NIL_PROC) {
 restore();
 return;
 }
 }
 xp->p_nextready = xp->p_nextready->p_nextready;
 while (xp->p_nextready != NIL_PROC) {
 xp = xp->p_nextready;
 }
 ipc->rdy_tail[q] = xp;
 }
 restore();
}

/* * Send a message to a destination (internal).
 * * There used to be a destination guard here (MINIX 1.0's
 * rule that only negative-numbered system tasks may be sent
 * to). It refused EVERY user->server send in this tree
 * silently, because Muzix unified process identity (the
 * process number IS the slot; servers are MUZIX_PROC_FS/MM/PM
 * = 2/3/4), so the routed-syscall path passes the positive
 * slot 2 while the guard compared against the obsolete
 * negative task numbers -5/-4. It is deleted, not widened:
 * the endpoint validation that follows is the same check with
 * current numbers.
 * \param[in,out] ipc: IPC table
 * caller: sending process number
 * dest: destination process number
 * m_ptr: message to send
 * OK on success, negative error code otherwise
 */
static int mini_send(muzix_ipc_table_t *ipc, int16_t caller, int16_t dest,
 muzix_ipc_message_t *m_ptr)
{
 muzix_ipc_proc_t *caller_ptr;
 muzix_ipc_proc_t *dest_ptr;
 muzix_ipc_proc_t *next_ptr;

 caller_ptr = muzix_ipc_proc(ipc, caller);
 dest_ptr = muzix_ipc_proc(ipc, dest);

 if (!caller_ptr || !dest_ptr || (dest_ptr->p_flags & P_SLOT_FREE)) {
 return E_BAD_DEST;
 }

 if ((dest_ptr->p_flags & RECEIVING) &&
 (dest_ptr->p_getfrom == MUZIX_IPC_ANY ||
 dest_ptr->p_getfrom == caller)) {
 /* Deliver into the destination's OWN message buffer and set DELIVERED,
 * per the contract in include/muzix_ipc.h: the IPC core never reaches
 * into userspace memory (tools/check_memory_manager.rb admits only a
 * file that defines an muzix_mm_* function, and this file defines
 * none), so it stores the message in p_msg and lets the destination
 * collect it. */
 cp_mess(caller, 0, m_ptr, dest, 0, &dest_ptr->p_msg);
 dest_ptr->p_flags |= MUZIX_IPC_FLAG_DELIVERED;
 dest_ptr->p_flags &= (uint8_t)~RECEIVING;
 if ((dest_ptr->p_flags & (uint8_t)~MUZIX_IPC_FLAG_DELIVERED) == 0) {
 muzix_ipc_ready(ipc, dest_ptr);
 }
 /* Return the destination's array index to signal it was woken. */
 return muzix_ipc_index(muzix_ipc_proc_nr(ipc, dest_ptr));
 } else {
 if (caller == HARDWARE) {
 return E_BAD_DEST;
 }
 /* MINIX 1.0 semantics: the queue entry points at the SENDER's message,
 * and the sender owns that storage. */
 caller_ptr->p_messbuf = m_ptr;
 caller_ptr->p_flags |= SENDING;
 muzix_ipc_unready(ipc, caller_ptr);

 if ((next_ptr = dest_ptr->p_callerq) == NIL_PROC) {
 dest_ptr->p_callerq = caller_ptr;
 } else {
 while (next_ptr->p_sendlink != NIL_PROC) {
 next_ptr = next_ptr->p_sendlink;
 }
 next_ptr->p_sendlink = caller_ptr;
 }
 caller_ptr->p_sendlink = NIL_PROC;
 }
 return OK;
}

/* * Receive a message (internal). Returns 0 if delivered,
 * 1 if blocked waiting.
 * \param[in,out] ipc: IPC table
 * caller: receiving process number
 * src: source to receive from, or MUZIX_IPC_ANY
 * m_ptr: buffer to receive into
 * 0 on delivery, 1 if blocked, negative error code
 */
static int mini_rec(muzix_ipc_table_t *ipc, int16_t caller, int16_t src,
 muzix_ipc_message_t *m_ptr)
{
 muzix_ipc_proc_t *caller_ptr;
 muzix_ipc_proc_t *sender_ptr;
 muzix_ipc_proc_t *prev_ptr;
 int16_t sender;

 caller_ptr = muzix_ipc_proc(ipc, caller);
 if (!caller_ptr) {
 return MUZIX_IPC_EBADSRC;
 }

 /* Collect a message that was delivered while this process was not looking.
 * mini_send() delivers into p_msg and sets DELIVERED. */
 if (caller_ptr->p_flags & MUZIX_IPC_FLAG_DELIVERED) {
 cp_mess(caller, 0, &caller_ptr->p_msg, caller, 0, m_ptr);
 caller_ptr->p_flags &= (uint8_t)~MUZIX_IPC_FLAG_DELIVERED;
 return 0;
 }

 sender_ptr = caller_ptr->p_callerq;
 while (sender_ptr != NIL_PROC) {
 sender = muzix_ipc_proc_nr(ipc, sender_ptr);
 if (src == MUZIX_IPC_ANY || src == sender) {
 cp_mess(sender, 0, sender_ptr->p_messbuf, caller, 0, m_ptr);
 sender_ptr->p_flags &= (uint8_t)~SENDING;
 if (sender_ptr->p_flags == 0) {
 muzix_ipc_ready(ipc, sender_ptr);
 }
 if (sender_ptr == caller_ptr->p_callerq) {
 caller_ptr->p_callerq = sender_ptr->p_sendlink;
 } else {
 prev_ptr->p_sendlink = sender_ptr->p_sendlink;
 }
 return 0;
 }
 prev_ptr = sender_ptr;
 sender_ptr = sender_ptr->p_sendlink;
 }

 /* No message available - block.
 * * p_messbuf is deliberately NOT set here. It has exactly one reader in
 * this tree - the queue branch of mini_rec() above, which reads a queued
 * sender's message through it. A parked receiver has no reader for this
 * field, so dropping the write costs nothing: a delivery lands in p_msg
 * (mini_send's delivery branch and muzix_syscalls_deliver_parked()), and
 * the syscall layer reaches the caller's userspace buffer through
 * p_user_addr. */
 caller_ptr->p_getfrom = src;
 caller_ptr->p_flags |= RECEIVING;
 muzix_ipc_unready(ipc, caller_ptr);

 return 1; /* Blocked */
}

/* * Send a message to a destination (public).
 * \param[in,out] ipc: IPC table
 * sender: sending process number
 * dest: destination process number
 * msg: message to send
 * 0 on success, negative error code otherwise
 */
int muzix_ipc_send(muzix_ipc_table_t *ipc, int16_t sender, int16_t dest,
 muzix_ipc_message_t *msg)
{
 if (!ipc || !msg) {
 return MUZIX_IPC_EINVAL;
 }
 if (sender < -NR_TASKS || sender >= NR_PROCS) {
 return MUZIX_IPC_EBADSRC;
 }
 if (dest < -NR_TASKS || (dest >= NR_PROCS && dest != MUZIX_IPC_ANY)) {
 return MUZIX_IPC_EBADDEST;
 }
 return mini_send(ipc, sender, dest, msg);
}

/* * Receive a message (public).
 * \param[in,out] ipc: IPC table
 * receiver: receiving process number
 * src: source to receive from, or MUZIX_IPC_ANY
 * msg: buffer to receive into
 * 0 on success, negative error code otherwise
 */
int muzix_ipc_receive(muzix_ipc_table_t *ipc, int16_t receiver, int16_t src,
 muzix_ipc_message_t *msg)
{
 if (!ipc || !msg) {
 return MUZIX_IPC_EINVAL;
 }
 if (receiver < -NR_TASKS || receiver >= NR_PROCS) {
 return MUZIX_IPC_EBADSRC;
 }
 if (src < -NR_TASKS || (src >= NR_PROCS && src != MUZIX_IPC_ANY)) {
 return MUZIX_IPC_EBADSRC;
 }
 return mini_rec(ipc, receiver, src, msg);
}

/* * Send and receive (public).
 * \param[in,out] ipc: IPC table
 * caller: calling process number
 * src_dest: destination/source slot
 * \param[in,out] msg: message to send, and buffer for the reply
 * 0 on success, negative error code otherwise
 */
int muzix_ipc_sendrec(muzix_ipc_table_t *ipc, int16_t caller, int16_t src_dest,
 muzix_ipc_message_t *msg)
{
 int result;

 if (!ipc || !msg) {
 return MUZIX_IPC_EINVAL;
 }
 if (caller < -NR_TASKS || caller >= NR_PROCS) {
 return MUZIX_IPC_EBADSRC;
 }
 if (src_dest < -NR_TASKS || (src_dest >= NR_PROCS && src_dest != MUZIX_IPC_ANY)) {
 return MUZIX_IPC_EBADDEST;
 }

 if (src_dest == MUZIX_IPC_ANY) {
 return mini_rec(ipc, caller, src_dest, msg);
 }

 result = mini_send(ipc, caller, src_dest, msg);
 if (result < 0) {
 return result;
 }
 /* If mini_send returned >= 10, a process was woken. Treat as success. */
 return mini_rec(ipc, caller, src_dest, msg);
}

/* * Schedule: pick the next ready process.
 * \param[in,out] ipc: IPC table
 */
void muzix_ipc_schedule(muzix_ipc_table_t *ipc)
{
 int q;

 if (!ipc) {
 return;
 }

 lock();
 if (ipc->rdy_head[TASK_Q] != NIL_PROC) {
 q = TASK_Q;
 } else if (ipc->rdy_head[SERVER_Q] != NIL_PROC) {
 q = SERVER_Q;
 } else {
 q = USER_Q;
 }

 ipc->prev_proc = ipc->cur_proc;
 if (ipc->rdy_head[q] != NIL_PROC) {
 ipc->cur_proc = muzix_ipc_proc_nr(ipc, ipc->rdy_head[q]);
 ipc->proc_ptr = ipc->rdy_head[q];
 if (ipc->cur_proc >= LOW_USER) {
 ipc->bill_ptr = ipc->proc_ptr;
 }
 } else {
 ipc->cur_proc = IDLE;
 ipc->proc_ptr = muzix_ipc_proc(ipc, HARDWARE);
  ipc->bill_ptr = ipc->proc_ptr;
  }
  restore();
}

/* Get the global IPC table - defined in kernel_main.c */
extern muzix_ipc_table_t *muzix_ipc_get_global_impl(void);

/* * Return the global IPC table pointer.
 * Pointer to the global IPC table
 */
muzix_ipc_table_t *muzix_ipc_get_global(void)
{
 return muzix_ipc_get_global_impl();
}

/* * Register a server process in the IPC table.
 * * Maps kernel proc table slot to IPC table slot for message passing.
 * \param[in,out] ipc: IPC table
 * kernel_slot: kernel proc table slot
 * server_id: server id number
 * IPC slot number on success, -1 on failure
 */
int muzix_ipc_register_server(muzix_ipc_table_t *ipc, int16_t kernel_slot,
 int16_t server_id)
{
 int16_t ipc_slot;
 muzix_ipc_proc_t *rp;

 (void)server_id;

 if (!ipc) {
 return -1;
 }
 if (kernel_slot < 0 || kernel_slot >= 16) {
 return -1;
 }

 /* IPC table slots 10-25 are for user processes. Map kernel slots 1-8 to
 * IPC slots 10-17. The index is the accessor's to take, not this
 * function's: it is the array offset, and the offset has exactly one
 * owner. kernel_slot < 16 above keeps it inside procs[26]. */
 ipc_slot = (int16_t)muzix_ipc_index(kernel_slot);

 rp = &ipc->procs[ipc_slot];
 if (rp->p_flags & P_SLOT_FREE) {
 rp->p_id = (uint8_t)ipc_slot;
 rp->p_pid = kernel_slot;
 rp->p_flags = 0;
 rp->p_getfrom = MUZIX_IPC_ANY;
 rp->p_messbuf = NIL_PROC;
 rp->p_callerq = NIL_PROC;
 rp->p_sendlink = NIL_PROC;
 rp->p_nextready = NIL_PROC;
 for (int i = 0; i < 8; i++) {
 rp->p_reg[i] = 0;
 }
 return ipc_slot;
 }
 return -1;
}

/* * Unregister a server from the IPC table.
 * \param[in,out] ipc: IPC table
 * kernel_slot: kernel proc table slot
 */
void muzix_ipc_unregister_server(muzix_ipc_table_t *ipc, int16_t kernel_slot)
{
 int16_t ipc_slot;

 if (!ipc) {
 return;
 }
 if (kernel_slot < 0 || kernel_slot >= 16) {
 return;
 }
 ipc_slot = (int16_t)muzix_ipc_index(kernel_slot);
 ipc->procs[ipc_slot].p_flags = P_SLOT_FREE;
}

/* * Set one entry's message buffer pointer from outside
 * kernel/ipc.c.
 * * nr is a PROCESS NUMBER, like every other number in this API.
 * \param[in,out] ipc: IPC table
 * nr: process number
 * buf: message buffer pointer
 * 0 on success, -1 if nr is out of range
 */
int muzix_ipc_set_messbuf(muzix_ipc_table_t *ipc, int16_t nr,
 muzix_ipc_message_t *buf)
{
 muzix_ipc_proc_t *rp;

 if (!ipc) {
 return -1;
 }
 rp = muzix_ipc_proc(ipc, nr);
 if (!rp) {
 return -1;
 }
 rp->p_messbuf = buf;
 return 0;
}
