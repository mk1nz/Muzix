#ifndef MUZIX_IPC_H
#define MUZIX_IPC_H

#include <stdint.h>
#include <stddef.h>

#define MUZIX_IPC_SEND      1
#define MUZIX_IPC_RECEIVE   2
#define MUZIX_IPC_BOTH      3
#define MUZIX_IPC_ANY       255

#define MUZIX_IPC_OK        0
#define MUZIX_IPC_EINVAL    -1
#define MUZIX_IPC_EFAULT    -2
#define MUZIX_IPC_EBADSRC   -3
#define MUZIX_IPC_EBADDEST  -4
#define MUZIX_IPC_EAGAIN    -5
#define MUZIX_IPC_EINTR     -6
#define MUZIX_IPC_ENOMEM    -7

#define MUZIX_IPC_MAX_PAYLOAD 56

typedef struct {
    int16_t m_source;
    int16_t m_type;
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
    } m_u;
} muzix_ipc_message_t;

#define MESS_SIZE (sizeof(muzix_ipc_message_t))

#define m1_i1  m_u.m_m1.m1_i1
#define m1_i2  m_u.m_m1.m1_i2
#define m1_i3  m_u.m_m1.m1_i3
#define m1_p1  m_u.m_m1.m1_p1
#define m1_p2  m_u.m_m1.m1_p2
#define m1_p3  m_u.m_m1.m1_p3

#define m2_i1  m_u.m_m2.m2_i1
#define m2_i2  m_u.m_m2.m2_i2
#define m2_i3  m_u.m_m2.m2_i3
#define m2_l1  m_u.m_m2.m2_l1
#define m2_l2  m_u.m_m2.m2_l2
#define m2_p1  m_u.m_m2.m2_p1

#define m3_i1  m_u.m_m3.m3_i1
#define m3_i2  m_u.m_m3.m3_i2
#define m3_p1  m_u.m_m3.m3_p1
#define m3_ca1 m_u.m_m3.m3_ca1

#define m4_l1  m_u.m_m4.m4_l1
#define m4_l2  m_u.m_m4.m4_l2
#define m4_l3  m_u.m_m4.m4_l3
#define m4_l4  m_u.m_m4.m4_l4

#define m5_c1  m_u.m_m5.m5_c1
#define m5_c2  m_u.m_m5.m5_c2
#define m5_i1  m_u.m_m5.m5_i1
#define m5_i2  m_u.m_m5.m5_i2
#define m5_l1  m_u.m_m5.m5_l1
#define m5_l2  m_u.m_m5.m5_l2
#define m5_l3  m_u.m_m5.m5_l3

#define m6_i1  m_u.m_m6.m6_i1
#define m6_i2  m_u.m_m6.m6_i2
#define m6_i3  m_u.m_m6.m6_i3
#define m6_l1  m_u.m_m6.m6_l1
#define m6_f1  m_u.m_m6.m6_f1

#define MUZIX_TASK_BASE     (-10)
#define MUZIX_HARDWARE      (-1)
#define MUZIX_SYSTEM_TASK   (-2)
#define MUZIX_CLOCK_TASK    (-3)
#define MUZIX_MM_TASK       (-4)
#define MUZIX_FS_TASK       (-5)
#define MUZIX_TTY_TASK      (-6)
#define MUZIX_PM_TASK       (-7)
#define MUZIX_LOW_USER      6

#define MUZIX_IPC_FLAG_RECEIVING  0x01
#define MUZIX_IPC_FLAG_SENDING    0x02
#define MUZIX_IPC_FLAG_SLOT_FREE  0x04
/*
 * A message has been delivered into p_msg and is waiting to be collected.
 *
 * This flag is what lets the KERNEL give up the copy-out.  mini_send() writes
 * the reply into the destination's own p_msg and sets this bit; it does not
 * try to reach the destination's userspace memory, which the memory-manager
 * gate forbids it from doing (tools/check_memory_manager.rb admits only files
 * that define an muzix_mm_* function, and kernel/ipc.c defines none).
 *
 * The destination then collects it itself: mini_rec() checks this bit before
 * blocking, copies p_msg into the buffer it was handed, clears it, and returns
 * "delivered".  The copy into userspace happens one layer up, in
 * muzix_handle_ipc_sendrec(), which already holds ctx and already calls
 * muzix_copy_user_to_current() - the sanctioned syscall layer.
 *
 * That split is the whole point: the kernel stores and hands over, the caller
 * moves the bytes across the bank boundary into its own space.  The alternative
 * - mini_send() doing the copy itself - needs a memory-manager module inside
 * the IPC core, which is exactly the monolith-in-waiting this was moved away
 * from.
 */
#define MUZIX_IPC_FLAG_DELIVERED  0x08

/*
 * =====================================================================
 * PROCESS IDENTITY - READ THIS BEFORE ADDING OR CHANGING A NUMBER
 * =====================================================================
 *
 * There is ONE process number in Muzix, and it is the number the scheduler
 * calls a slot.  A user process and the IPC entry that describes it share it.
 *
 * This was NOT always so, and the way it was wrong is worth writing down
 * because it cost days and it looked like several unrelated bugs.
 *
 * `muzix_ipc_proc_t` is stored in an array whose indices do NOT start at 0.
 * `muzix_ipc_get_proc()` reaches a process as `&procs[NR_TASKS + nr]`, because
 * the low half of the array is reserved for the negative system-task numbers
 * (-1..-10) that MINIX put there.  So for a process number `nr`:
 *
 *     nr >= 0  ->  array index  10 + nr
 *     nr <  0  ->  array index  -nr - 1
 *
 * That `10 +` is an *array offset*.  It is not a property of the process.
 *
 * Five places treated the offset as if it were the process number, and each
 * one was separately plausible, separately wrong, and separately invisible:
 *
 *   kernel/syscalls_ipc.c  ipc_slot = 10 + current_slot;
 *       Used as the `caller` argument of send/receive.  get_proc() then
 *       computed procs[10 + (10 + slot)] = procs[20 + slot] - a slot nobody
 *       registered - so every send and receive was addressed to a dead entry.
 *
 *   kernel/syscalls_ipc.c  muzix_ipc_server_slot() returned `10 + slot`.
 *       Fed straight in as the destination of a routed syscall.
 *
 *   kernel/syscalls_ipc.c  ipc_client = msg.m_source, unconverted.
 *       The only readers are the SRV_COPY handlers, which hand it to
 *       muzix_copy_*_proc().  Those reject anything >= MUZIX_PROC_TABLE_MAX,
 *       so every server-to-client copy was refused with EFAULT - the same
 *       code a genuinely bad address returns, which is why it read as a
 *       filesystem bug and not an identity bug.
 *
 *   shell/shell.c  #define FS_SERVER_IPC_SLOT 11.
 *       A hand-written guess at `10 + server_id`, correct only under the
 *       broken scheme and wrong the moment the numbering was fixed.
 *
 *   lib/server.c  server->ipc_slot = 10 + id.
 *       Dead code - nothing reads it - which is exactly why nothing caught it.
 *
 * THE RULE, so that this is five places that cannot each invent an answer:
 *
 *   1. The process number IS the kernel proc-table slot.  They are the same
 *      integer.  Do not convert between them; there is nothing to convert.
 *   2. The array offset lives in exactly ONE function, muzix_ipc_proc(),
 *      which is the only code in the tree allowed to index procs[].
 *   3. Server numbers are named constants here, one each, defined ONCE.
 *      Never compute one from an id or a slot.
 *   4. tools/check_proc_identity.rb refuses `10 +`, `NR_TASKS +` and any
 *      direct `procs[` outside muzix_ipc_proc().  A regression here now
 *      fails `make check` instead of a boot trace.
 * =====================================================================
 */

/* The numeric base of the negative system-task half of procs[].  The tasks
 * (-1 hardware, -2 system, -3 clock, -4 MM, -5 FS, -6 TTY, -7 PM) live at
 * procs[9], procs[8], ... procs[3]; get_proc() reaches them as -nr - 1.
 * Do not do that arithmetic anywhere else. */
#define MUZIX_IPC_NR_TASKS     10

/* The public server process numbers.  These are the ONLY spelling of them.
 * shell.c and lib/server.c must use these, not their own arithmetic.
 *
 * They equal the kernel slots the servers are registered under, which is what
 * makes them safe: kernel_main.c registers FS with
 * muzix_ipc_register_server(&g_ipc, MUZIX_PROC_FS, ...) and the scheduler
 * dispatches MUZIX_PROC_FS's slot.  One integer, both meanings. */
#define MUZIX_PROC_FS          2
#define MUZIX_PROC_MM          3
#define MUZIX_PROC_PM          4

/* No valid process.  Used as "no destination" rather than 0 or -1, because 0
 * is a real slot (the idle slot) and -1 is the hardware task. */
#define MUZIX_PROC_NONE        (-100)

typedef struct muzix_ipc_proc muzix_ipc_proc_t;

struct muzix_ipc_proc {
    uint8_t p_id;
    int16_t p_pid;
    uint8_t p_flags;
    int16_t p_getfrom;
    /*
     * This process's own process number - RESERVED, never written or read.
     *
     * It was meant to be stamped at init so that muzix_ipc_proc_nr() would
     * not have to recover the number from a pointer, and an earlier comment
     * here claimed the table filled it in together with the forward map.
     * It does not: nothing in the tree writes this field, and
     * muzix_ipc_proc_nr() recovers the number by comparing the pointer
     * against every entry in turn (kernel/ipc.c).  That is deliberate -
     * `rp - procs` compiles to a 16-bit divide by sizeof(muzix_ipc_proc_t),
     * which calls __divsint, and KERNEL_LINK_ORDER links .rel files only,
     * so the helper does not resolve; the loop is exact and has no runtime
     * dependency.  The field stays because dropping it would move every
     * member after it; it is padding, not state.
     */
    int16_t p_nr;
    muzix_ipc_message_t *p_messbuf;
    muzix_ipc_proc_t *p_callerq;
    muzix_ipc_proc_t *p_sendlink;
    muzix_ipc_proc_t *p_nextready;
    uint16_t p_reg[8];
    /*
     * Where a blocked receive is parked.
     *
     * This is a POINTER, and it used to be handed whatever address the caller
     * had - `&msg`, a local of muzix_handle_ipc_sendrec() on the kernel stack.
     * A parked process has no stack to come back to: the trap resets SP to
     * SYSCALL_SP on every entry, so the next process's syscall frames land on
     * exactly the bytes FS's msg occupied.  When the message finally arrived,
     * cp_mess() wrote it through that dead pointer.
     *
     * The fix is that the buffer belongs to the process, not to a stack frame.
     * mini_rec() sets p_messbuf = &rp->p_msg, so the storage outlives the
     * switch, and mini_send() does not need to know who parked there.
     *
     * The copy out to the real userspace buffer still has to happen after the
     * switch, which is what the rest of park/resume is about - see the note in
     * muzix_handle_ipc_sendrec().  Getting this far removes the wild write, but
     * it does not by itself deliver the message to the server.
     */
    muzix_ipc_message_t p_msg;
    /*
     * Where THIS process's userspace message buffer is.
     *
     * mini_rec() parks a blocked receive in the process's own p_msg, which is
     * why the message survives the switch - but the process that asked for it
     * is holding some *other* buffer: the `&msg` its sendrec wrapper was called
     * with, on its own stack.  That address used to be dropped entirely, so
     * whoever resumed a parked caller had no idea where the reply belonged and
     * the caller's buffer was never written.
     *
     * It is recorded by the syscall layer when it is about to block
     * (muzix_handle_ipc_sendrec and the routed-syscall path in
     * kernel/syscalls_ipc.c), because that is the layer that knows both the
     * process and the user address.  It is read by the same layer at delivery
     * time - the moment mini_send() wakes this process - which is where the
     * copy out can still happen, on a kernel stack that is still live.
     *
     * 0 means "nothing parked here" and is not a valid address: window 0 is
     * the kernel page, so no user message buffer can ever live at 0 and a
     * stale value can be told from a real one.
     */
    uint16_t p_user_addr;
    uint16_t _pad;  /* Pad to 32 bytes for power-of-2 indexing */
};

typedef struct {
    muzix_ipc_proc_t procs[26];
    uint8_t proc_count;
    muzix_ipc_proc_t *rdy_head[3];
    muzix_ipc_proc_t *rdy_tail[3];
    int16_t cur_proc;
    int16_t prev_proc;
    muzix_ipc_proc_t *proc_ptr;
    muzix_ipc_proc_t *bill_ptr;
    uint8_t busy_map;
    muzix_ipc_message_t *task_mess[10];
    int sig_procs;
} muzix_ipc_table_t;

void muzix_ipc_init(muzix_ipc_table_t *ipc);
int muzix_ipc_send(muzix_ipc_table_t *ipc, int16_t sender, int16_t dest, muzix_ipc_message_t *msg);
int muzix_ipc_receive(muzix_ipc_table_t *ipc, int16_t receiver, int16_t src, muzix_ipc_message_t *msg);
int muzix_ipc_sendrec(muzix_ipc_table_t *ipc, int16_t caller, int16_t src_dest, muzix_ipc_message_t *msg);
void muzix_ipc_schedule(muzix_ipc_table_t *ipc);
void muzix_ipc_ready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp);
void muzix_ipc_unready(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp);

/*
 * THE ONLY WAY TO GET AN ENTRY OUT OF procs[].
 *
 * Everything else in the tree takes and returns process numbers and never
 * touches the array.  This function exists so the `NR_TASKS +` arithmetic
 * that the array layout forces has exactly one home - see the long note above
 * about the five places that each wrote it out by hand and each got it wrong.
 *
 * Returns a pointer, or MUZIX_IPC_NONE_PROC (NULL) if the number is out of
 * range.  Callers must check; the old code checked for NULL at some call sites
 * and not others.
 */
muzix_ipc_proc_t *muzix_ipc_proc(muzix_ipc_table_t *ipc, int16_t nr);

/* muzix_ipc_get_proc() and muzix_ipc_proc_nr() are the OLD names.  They are
 * kept as thin wrappers over muzix_ipc_proc() so that existing callers keep
 * working while they are migrated, but they are deprecated: new code should
 * call muzix_ipc_proc() directly.  A gate forbids adding callers, not removing
 * them. */
muzix_ipc_proc_t *muzix_ipc_get_proc(muzix_ipc_table_t *ipc, int16_t nr);
int16_t muzix_ipc_proc_nr(muzix_ipc_table_t *ipc, muzix_ipc_proc_t *rp);

muzix_ipc_table_t *muzix_ipc_get_global(void);
int muzix_ipc_register_server(muzix_ipc_table_t *ipc, int16_t kernel_slot, int16_t server_id);
void muzix_ipc_unregister_server(muzix_ipc_table_t *ipc, int16_t kernel_slot);
int muzix_ipc_set_messbuf(muzix_ipc_table_t *ipc, int16_t nr, muzix_ipc_message_t *buf);

#endif