/*
 * kernel/ipc.c - IPC core for the Muzix microkernel
 *
 * Muzix implements an independent message-passing IPC layer inspired by
 * selected MINIX 1.0 concepts, but with its own process identity model
 * (the IPC slot number IS the kernel proc-table slot; see AGENTS.md) and
 * its own syscall ABI. It is not a MINIX port.
 * 
 * host-test-not-portable: uses Z80 `di`/`ei` assembly instructions for
 * interrupt locking which are not available in host compilation.
 * This module must be compiled only for the Z80 target.
 */

#include "../include/muzix_ipc.h"
#include <string.h>

#define TASK_Q      0
#define SERVER_Q    1
#define USER_Q      2
#define NR_TASKS    10
#define NR_PROCS    16
#define LOW_USER    0
#define NIL_PROC    ((muzix_ipc_proc_t *)0)
#define IDLE        (-100)
#define HARDWARE    (-1)

#define OK          0
#define E_BAD_SRC   (-3)
#define E_BAD_DEST  (-4)
#define E_NO_PERM   (-5)

#define RECEIVING   MUZIX_IPC_FLAG_RECEIVING
#define SENDING     MUZIX_IPC_FLAG_SENDING
#define P_SLOT_FREE MUZIX_IPC_FLAG_SLOT_FREE

#define RET_REG     0

#define CLICK_SHIFT 4

/* The procs[] array offset, spelled once in each direction.
 *
 * A process number nr does not index procs[] from zero: the low half of the
 * array is the negative system-task numbers, so nr sits at array index
 * NR_TASKS + nr and a task nr sits at -nr - 1.  That offset is a property of
 * the ARRAY, not of the process (AGENTS.md "Process Identity"), so it is
 * computed here and nowhere else - muzix_ipc_proc() is the only code in the
 * tree that indexes procs[], and both directions of the map go through these
 * two helpers.  Everything else takes and returns process numbers.
 *
 * The reverse direction used by muzix_ipc_proc_nr() is a pointer loop, not
 * this arithmetic: `rp - procs` compiles to a 16-bit divide by sizeof(),
 * which calls __divsint, and KERNEL_LINK_ORDER links no .lib - the note on
 * that function has the full argument. */
static int muzix_ipc_index(int16_t nr)
{
    if (nr < 0) {
        return -nr - 1;
    }
    return NR_TASKS + nr;
}

static int muzix_ipc_nr_of_index(int idx)
{
    if (idx < NR_TASKS) {
        return -idx - 1;
    }
    return idx - NR_TASKS;
}

static void lock(void) {
    __asm__("di");
}

static void restore(void) {
    __asm__("ei");
}

static void cp_mess(int16_t src, uint16_t src_phys, muzix_ipc_message_t *src_ptr,
                    int16_t dst, uint16_t dst_phys, muzix_ipc_message_t *dst_ptr) {
    (void)src;
    (void)src_phys;
    (void)dst;
    (void)dst_phys;
    *dst_ptr = *src_ptr;
}

void muzix_ipc_init(muzix_ipc_table_t *ipc) {
    if (!ipc) return;
    memset(ipc, 0, sizeof(*ipc));
    ipc->proc_count = 0;
    /* A zeroed entry and a registered one have to be distinguishable.
     *
     * memset leaves every p_flags == 0, and 0 is the flag set of a
     * REGISTERED entry - so with P_SLOT_FREE == 0x04 the guard in
     * muzix_ipc_register_server() (`rp->p_flags & P_SLOT_FREE`) was never
     * true, nothing was ever registered, and every send to a server was
     * refused at the endpoint check.  Mark all 26 entries free here, and
     * let registration be the thing that clears the bit; unregister puts
     * it back. */
    for (int i = 0; i < 26; i++) {
        ipc->procs[i].p_flags = P_SLOT_FREE;
    }
    for (int i = 0; i < 3; i++) {
        ipc->rdy_head[i] = NIL_PROC;
        ipc->rdy_tail[i] = NIL_PROC;
    }
    ipc->cur_proc = IDLE;
    ipc->prev_proc = IDLE;
    ipc->proc_ptr = &ipc->procs[NR_TASKS - 1];
    ipc->bill_ptr = ipc->proc_ptr;
}

muzix_ipc_proc_t *muzix_ipc_proc(muzix_ipc_table_t *ipc, int16_t nr) {
    if (!ipc) return NIL_PROC;
    if (nr < -NR_TASKS || nr >= NR_PROCS) return NIL_PROC;
    return &ipc->procs[muzix_ipc_index(nr)];
}

int16_t muzix_ipc_proc_nr(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp) {
    if (!ipc || !rp) return -1;
    /* Compare against each slot instead of dividing a pointer difference.
     *
     * `rp - ipc->procs` looks like the obvious spelling and is a trap on this
     * tree: SDCC compiles a pointer difference into a 16-bit division by
     * sizeof(muzix_ipc_proc_t), which calls __divsint - and __divsint is ONLY
     * defined in lib/math.s, a userspace library the kernel does not link.  The
     * kernel then has a reference to address 0x0000, the boot stub: every
     * muzix_ipc_ready() and every mini_send() return would jump there and the
     * machine would restart instead of delivering.  The tree only built because
     * a stale ipc.rel from before this spelling was reused; a clean build
     * exposed it.  A loop over the same array is exact, has no runtime
     * dependency, and the array is NR_PROCS entries. */
    int16_t i;
    for (i = 0; i < NR_PROCS; i++) {
        if (&ipc->procs[i] == rp) {
            return (int16_t)muzix_ipc_nr_of_index(i);
        }
    }
    return -1;
}

void muzix_ipc_ready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp) {
    if (!ipc || !rp || rp == NIL_PROC) return;
    
    lock();
    int16_t r = muzix_ipc_proc_nr(ipc, rp);
    int q = (r < 0 ? TASK_Q : r < LOW_USER ? SERVER_Q : USER_Q);
    
    if (ipc->rdy_head[q] == NIL_PROC) {
        ipc->rdy_head[q] = rp;
    } else {
        ipc->rdy_tail[q]->p_nextready = rp;
    }
    ipc->rdy_tail[q] = rp;
    rp->p_nextready = NIL_PROC;
    restore();
}

void muzix_ipc_unready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp) {
    if (!ipc || !rp || rp == NIL_PROC) return;
    
    lock();
    int16_t r = muzix_ipc_proc_nr(ipc, rp);
    int q = (r < 0 ? TASK_Q : r < LOW_USER ? SERVER_Q : USER_Q);
    
    muzix_ipc_proc_t *xp = ipc->rdy_head[q];
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

static int mini_send(muzix_ipc_table_t *ipc, int16_t caller, int16_t dest, muzix_ipc_message_t *m_ptr) {
    muzix_ipc_proc_t *caller_ptr, *dest_ptr, *next_ptr;

    /* There used to be a destination guard here - MINIX 1.0's rule that only
     * the negative-numbered system tasks may be sent to:
     *
     *     if (caller >= LOW_USER && (dest != MUZIX_FS_TASK && dest != MUZIX_MM_TASK))
     *         return E_BAD_DEST;
     *
     * It refused EVERY user->server send in this tree, and it did so silently:
     * Muzix unified process identity (AGENTS.md "Process Identity": the process
     * number IS the slot, servers are MUZIX_PROC_FS/MM/PM = 2/3/4), so the
     * routed-syscall path passes the positive slot 2 while the guard compared
     * against the obsolete negative task numbers -5/-4.  Every shell write
     * returned E_BAD_DEST before the destination's flags were even read, so
     * nothing was copied, queued, woken or delivered: measured, 244/244 sends
     * refused at the guard in three runs, zero hits past it, zero hits in the
     * delivery branch and the queue branch, and the FS server dispatched
     * m_type == 0 forever because its receive buffer was only ever memset.
     *
     * It is deleted, not widened: the endpoint validation that follows is the
     * same check with current numbers - muzix_ipc_proc() returns NIL_PROC for a
     * number outside the table and P_SLOT_FREE covers a slot that is not
     * registered - and the public muzix_ipc_send() wrapper already range-checks
     * both arguments.  The one sender that may not block on a queue, HARDWARE,
     * is still refused in the queue branch below. */
    caller_ptr = muzix_ipc_proc(ipc, caller);
    dest_ptr = muzix_ipc_proc(ipc, dest);

    if (!caller_ptr || !dest_ptr || (dest_ptr->p_flags & P_SLOT_FREE)) {
        return E_BAD_DEST;
    }

    if ((dest_ptr->p_flags & RECEIVING) &&
        (dest_ptr->p_getfrom == MUZIX_IPC_ANY || dest_ptr->p_getfrom == caller)) {
        /* Deliver into the destination's OWN message buffer and set DELIVERED,
         * per the contract in include/muzix_ipc.h: the IPC core never reaches
         * into userspace memory (tools/check_memory_manager.rb admits only a
         * file that defines an muzix_mm_* function, and this file defines
         * none), so it stores the message in p_msg and lets the destination
         * collect it.  This used to write through dest_ptr->p_messbuf, which
         * for a process parked in mini_rec() is the address of the KERNEL
         * STACK FRAME that called it - the syscall handler's local, abandoned
         * the moment the handler parked - so a delivery landed in memory the
         * kernel had already left.  p_messbuf stays what it is for the queue
         * path below, where it points at a SENDER's message. */
        cp_mess(caller, 0, m_ptr, dest, 0, &dest_ptr->p_msg);
        dest_ptr->p_flags |= MUZIX_IPC_FLAG_DELIVERED;
        dest_ptr->p_flags &= ~RECEIVING;
        if ((dest_ptr->p_flags & (uint8_t)~MUZIX_IPC_FLAG_DELIVERED) == 0) {
            muzix_ipc_ready(ipc, dest_ptr);
        }
        /* Return the destination's array index (process number + NR_TASKS,
         * which the syscall layer decodes as `woken_slot = result - 10`) to
         * signal it was woken.  The index comes from the same accessor pair
         * as the forward map, so the `+ 10` spelling itself lives only in
         * muzix_ipc_index(); the VALUE is unchanged, because the callers in
         * kernel/syscalls_ipc.c and kernel/exec_loader.c still subtract 10. */
        return muzix_ipc_index(muzix_ipc_proc_nr(ipc, dest_ptr));
    } else {
        if (caller == HARDWARE) return E_BAD_DEST;
        /* MINIX 1.0 semantics: the queue entry points at the SENDER's message,
         * and the sender owns that storage.  The kernel-routed senders used to
         * hand in the stack frame of whatever handler parked - see the note on
         * g_route_msg in kernel/syscalls_ipc.c - so the queue held a pointer
         * into a dead frame and the collect read whatever the kernel stack had
         * been reused for.  They now stage into a per-slot buffer first, which
         * keeps the core dumb about ownership.
         *
         * Do NOT "fix" this by copying into caller_ptr->p_msg instead: p_msg is
         * the same slot's INCOMING buffer - mini_send's delivery branch writes
         * it, mini_rec's DELIVERED path copies out of it, and
         * muzix_syscalls_deliver_parked() reads it.  Sharing one buffer between
         * a queued outgoing message and the next incoming one was measured on
         * 2026-10-14 (three variants, reproducible) to leave the machine never
         * idling: ~1500 map installs per 90M cycles against 223 traps, shell and
         * fs_server round-robin at full tilt with no HALT anywhere. */
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

/* Receive a message. Returns 0 if message delivered, 1 if blocked waiting. */
static int mini_rec(muzix_ipc_table_t *ipc, int16_t caller, int16_t src, muzix_ipc_message_t *m_ptr) {
    muzix_ipc_proc_t *caller_ptr, *sender_ptr, *prev_ptr;
    int16_t sender;
    
    caller_ptr = muzix_ipc_proc(ipc, caller);
    if (!caller_ptr) return E_BAD_SRC;

    /* Collect a message that was delivered while this process was not looking.
     *
     * mini_send() delivers into p_msg and sets DELIVERED; the process it woke
     * consumes it through muzix_syscalls_deliver_parked() in the syscall layer,
     * which is what copies it into userspace and clears the bit.  This check is
     * the other half of that contract (include/muzix_ipc.h: mini_rec() "checks
     * this bit before blocking, copies p_msg into the buffer it was handed,
     * clears it, and returns delivered"), and it covers the case the parked
     * wake does not: a receive that arrives AFTER the delivery - the process
     * was running when the message landed, or the wake and the collect raced.
     * Without it the bit stays set, the next receive blocks on a message that
     * is sitting right there, and the process waits for a wake that will never
     * come. */
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
            sender_ptr->p_flags &= ~SENDING;
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
     *
     * p_messbuf is deliberately NOT set here.  This field has exactly one
     * reader in this tree - the queue branch of mini_rec() above, which reads
     * a QUEUED SENDER's message through it - and a process can be a queued
     * sender and a parked receiver at the same time, which is the normal
     * shape of a request-and-reply: the routed-syscall path
     * (muzix_handle_userspace_syscall, kernel/syscalls_ipc.c) stages its
     * request into g_route_msg[slot], mini_send() queues that pointer when the
     * server is busy, and the same syscall then blocks for its reply right
     * here.  Writing the receive buffer over the field destroyed the queued
     * message pointer, and the server's next receive walked its callerq to
     * the sender's entry and read whatever the kernel stack had been reused
     * for since - measured on 2026-10-14 with the kernel trace ring: the FS
     * server collected its own reply bytes (m_source = the FS itself) as a
     * request from the shell, replied to itself, and stamped
     * slots[FS].ipc_client with its own slot, after which every shell write
     * was copied from the wrong client and the prompt after a userspace
     * program never reached the console.
     *
     * A parked receiver has no reader for this field, so dropping the write
     * costs nothing: a delivery lands in p_msg (mini_send's delivery branch
     * and muzix_syscalls_deliver_parked()), and the syscall layer reaches the
     * caller's userspace buffer through p_user_addr.  MINIX sets p_messbuf
     * here because there the queue and the receive share ONE buffer per
     * process; the kernel-routed paths here use two, which is what makes the
     * aliasing possible at all. */
    caller_ptr->p_getfrom = src;
    caller_ptr->p_flags |= RECEIVING;
    muzix_ipc_unready(ipc, caller_ptr);
    
    return 1;  /* Blocked */
}

int muzix_ipc_send(muzix_ipc_table_t *ipc, int16_t sender, int16_t dest, muzix_ipc_message_t *msg) {
    if (!ipc || !msg) return MUZIX_IPC_EINVAL;
    if (sender < -NR_TASKS || sender >= NR_PROCS) return MUZIX_IPC_EBADSRC;
    if (dest < -NR_TASKS || (dest >= NR_PROCS && dest != MUZIX_IPC_ANY)) return MUZIX_IPC_EBADDEST;
    return mini_send(ipc, sender, dest, msg);
}

int muzix_ipc_receive(muzix_ipc_table_t *ipc, int16_t receiver, int16_t src, muzix_ipc_message_t *msg) {
    if (!ipc || !msg) return MUZIX_IPC_EINVAL;
    if (receiver < -NR_TASKS || receiver >= NR_PROCS) return MUZIX_IPC_EBADSRC;
    if (src < -NR_TASKS || (src >= NR_PROCS && src != MUZIX_IPC_ANY)) return MUZIX_IPC_EBADSRC;
    return mini_rec(ipc, receiver, src, msg);
}

int muzix_ipc_sendrec(muzix_ipc_table_t *ipc, int16_t caller, int16_t src_dest, muzix_ipc_message_t *msg) {
    int result;
    if (!ipc || !msg) return MUZIX_IPC_EINVAL;
    if (caller < -NR_TASKS || caller >= NR_PROCS) return MUZIX_IPC_EBADSRC;
    if (src_dest < -NR_TASKS || (src_dest >= NR_PROCS && src_dest != MUZIX_IPC_ANY)) return MUZIX_IPC_EBADDEST;
    
    if (src_dest == MUZIX_IPC_ANY) {
        return mini_rec(ipc, caller, src_dest, msg);
    }
    
    result = mini_send(ipc, caller, src_dest, msg);
    if (result < 0) {
        return result;
    }
    /* If mini_send returned >= 10, a process was woken. Treat as success for the send. */
    return mini_rec(ipc, caller, src_dest, msg);
}

void muzix_ipc_schedule(muzix_ipc_table_t *ipc) {
    if (!ipc) return;
    
    lock();
    int q;
    if (ipc->rdy_head[TASK_Q] != NIL_PROC) q = TASK_Q;
    else if (ipc->rdy_head[SERVER_Q] != NIL_PROC) q = SERVER_Q;
    else q = USER_Q;
    
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

void muzix_ipc_pick_proc(muzix_ipc_table_t *ipc) {
    muzix_ipc_schedule(ipc);
}

/* Get the global IPC table - defined in kernel_main.c */
extern muzix_ipc_table_t *muzix_ipc_get_global_impl(void);
muzix_ipc_table_t *muzix_ipc_get_global(void)
{
    return muzix_ipc_get_global_impl();
}

/* Register a server process in the IPC table.
 * Maps kernel proc table slot to IPC table slot for message passing.
 * Returns IPC slot number on success, -1 on failure.
 */
int muzix_ipc_register_server(muzix_ipc_table_t *ipc, int16_t kernel_slot, int16_t server_id)
{
    if (!ipc) return -1;
    if (kernel_slot < 0 || kernel_slot >= 16) return -1;
    
    /* IPC table slots 10-25 are for user processes (LOW_USER=6 and above)
     * Map kernel slots 1-8 to IPC slots 10-17.  The index is the accessor's
     * to take, not this function's: it is the array offset, and the offset
     * has exactly one owner.  kernel_slot < 16 above keeps it inside
     * procs[26]. */
    int16_t ipc_slot = (int16_t)muzix_ipc_index(kernel_slot);
    
    muzix_ipc_proc_t *rp = &ipc->procs[ipc_slot];
    if (rp->p_flags & P_SLOT_FREE) {
        rp->p_id = (uint8_t)ipc_slot;
        rp->p_pid = kernel_slot;
        rp->p_flags = 0;
        rp->p_getfrom = MUZIX_IPC_ANY;
        rp->p_messbuf = NIL_PROC;
        rp->p_callerq = NIL_PROC;
        rp->p_sendlink = NIL_PROC;
        rp->p_nextready = NIL_PROC;
        for (int i = 0; i < 8; i++) rp->p_reg[i] = 0;
        /* Server is now registered in IPC table */
        return ipc_slot;
    }
    return -1;
}

/* Unregister a server from the IPC table */
void muzix_ipc_unregister_server(muzix_ipc_table_t *ipc, int16_t kernel_slot)
{
    if (!ipc) return;
    /* Mirror register_server()'s validation.  Only slots 0-15 have entries
     * at 10 + kernel_slot; a negative argument must not be re-mapped into
     * the system-task half of the array through this door. */
    if (kernel_slot < 0 || kernel_slot >= 16) return;
    int16_t ipc_slot = (int16_t)muzix_ipc_index(kernel_slot);
    ipc->procs[ipc_slot].p_flags = P_SLOT_FREE;
}

/* Set one entry's message buffer pointer from outside kernel/ipc.c.
 *
 * p_messbuf belongs to an entry, and reaching the entry means the
 * NR_TASKS + arithmetic - which has exactly one owner - so the way in from
 * another file is this function rather than a hand-written
 * `g_ipc.procs[10 + slot]` at the call site.  nr is a PROCESS NUMBER, like
 * every other number in this API.  Returns 0 on success, -1 if nr is out of
 * range. */
int muzix_ipc_set_messbuf(muzix_ipc_table_t *ipc, int16_t nr, muzix_ipc_message_t *buf)
{
    if (!ipc) return -1;
    muzix_ipc_proc_t *rp = muzix_ipc_proc(ipc, nr);
    if (!rp) return -1;
    rp->p_messbuf = buf;
    return 0;
}