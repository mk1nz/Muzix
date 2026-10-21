#include "syscalls.h"
#include "../include/muzix_ipc.h"
#include "proc_table.h"
#include "exec_loader.h"
#include <string.h>
#include "../lib/syscall.h"

#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/tick.h"
#include "load.h"
#include "../mm/mm_service.h"
#include "rom_read.h"

/*
 * Which server a userspace syscall is routed to.
 *
 * This used to be a set of small "server id" macros - MUZIX_IPC_SERVER_FS=1 and
 * friends - separate from the process numbers they eventually had to become.
 * The two were off by one in a way that looked fine at every call site, which
 * is what made the `10 +` conversions above necessary in the first place.
 *
 * There is now ONE set of numbers, MUZIX_PROC_* in include/muzix_ipc.h, and
 * this function returns one of them directly.  The ids are gone.
 */
#define MUZIX_IPC_SERVER_FS   MUZIX_PROC_FS
#define MUZIX_IPC_SERVER_MM   MUZIX_PROC_MM
#define MUZIX_IPC_SERVER_PM   MUZIX_PROC_PM

/* Forward declarations for functions in syscalls.c */
extern int muzix_current_slot(muzix_userspace_syscall_context_t *ctx);
extern muzix_proc_slot_t *muzix_current_process(muzix_userspace_syscall_context_t *ctx);
extern int32_t muzix_dispatch_userspace_syscall(muzix_userspace_syscall_context_t *ctx, int32_t syscall_num, int32_t arg1, int32_t arg2, int32_t arg3);
extern int muzix_copy_user_to_current(muzix_userspace_syscall_context_t *ctx, uint16_t uaddr, const uint8_t *src, size_t len);
extern int muzix_copy_user_from_current(muzix_userspace_syscall_context_t *ctx, uint16_t uaddr, uint8_t *dst, size_t len);
/* The two SRV_COPY handlers copy on behalf of the sender that called in, not
 * on behalf of whoever is running now, so they cannot go through the
 * *_user_*_current() pair above - those resolve the current process.  These
 * take the target slot explicitly and live in syscalls.c. */
extern int muzix_copy_to_proc(muzix_userspace_syscall_context_t *ctx, uint8_t target_slot, uint16_t target_addr, const uint8_t *buffer, uint16_t length);
extern int muzix_copy_from_proc(muzix_userspace_syscall_context_t *ctx, uint8_t target_slot, uint16_t target_addr, uint8_t *buffer, uint16_t length);

/* Global IPC table from kernel_main.c */
extern muzix_ipc_table_t g_ipc;

/* Window-0 variables (defined in kernel_entry.s) */
extern uint16_t muzix_switch_pending;
extern uint16_t muzix_resume_pc;
/* The trap's fork-stack measurement and the one staging buffer the copy uses,
 * both defined in kernel/kernel_main.c next to the resume cells. */
extern uint16_t muzix_fork_stack_len;
extern uint8_t muzix_fork_stack_scratch[];
/* The one memory manager the kernel built at boot (kernel/kernel_main.c); the
 * exec loader keeps its own extern for the same object. */
extern muzix_mm_service_t g_mm_service;
extern uint16_t muzix_resume_sp;
extern uint16_t muzix_resume_ret;
extern uint8_t muzix_resume_banks[4];

/*
 * Remember where a process's userspace message buffer is, before it blocks.
 *
 * mini_rec() parks the receive in the process's own p_msg so the message
 * survives the switch, but the reply that eventually arrives has to end up in
 * the buffer the process's own wrapper passed to sendrec() - a local on its
 * stack.  Nothing wrote that address down, so a resumed process came back to a
 * buffer no reply had ever touched and answered a request it had never seen.
 *
 * This is that record, taken while the caller is still running - which is the
 * only time its own buffer address is available, because after the park the
 * frame that held it is abandoned.
 */
static void muzix_syscalls_note_recv_buffer(int slot, uint16_t user_msg)
{
    muzix_ipc_proc_t *rp;

    if (slot <= 0 || slot >= MUZIX_PROC_TABLE_MAX) {
        return;
    }
    rp = muzix_ipc_proc(&g_ipc, (int16_t)slot);
    if (rp) {
        rp->p_user_addr = user_msg;
    }
}

/*
 * =====================================================================
 * THE REPLY COPY-OUT.  This is the piece that was never written.
 * =====================================================================
 *
 * A parked syscall is waiting for this, and until now it never came: the
 * reply landed in caller->p_msg, the slot was readied in the kernel table, and
 * the resumed process got resume_ret == 0 - the park placeholder.  Its write()
 * therefore returned 0 for every character, putchar() mapped that to -1, and
 * the shell treated its own output as failure and exited.  That is the whole of
 * the silent console.
 *
 * Three independent audits of this exact question all landed on this function,
 * and all three chose the same option: the KERNEL performs the copy, at
 * delivery time.  The alternatives and why they lose:
 *
 *   - Do it in kernel/ipc.c, where the message is actually delivered.  No:
 *     tools/check_memory_manager.rb admits only files that DEFINE an
 *     muzix_mm_* function, and ipc.c defines none.  Crossing a bank boundary
 *     is the memory manager's job and the gate says so.
 *
 *   - Have the caller fetch it after resuming.  No, and the tree already
 *     recorded why: resume-and-collect is what made the FS server answer
 *     stale requests for so long (the note at muzix_handle_ipc_sendrec), and
 *     a parked routed write() resumes with 0 and reports failure straight out
 *     of libc.  Worse, a parked syscall's frame is abandoned - the code after
 *     muzix_kernel_switchout() does not run - so there is no frame to do it in.
 *
 * This runs where a frame still exists: in the delivering syscall, with ctx and
 * a live kernel stack, before the caller is readied.  So:
 *
 *   1. copy caller->p_msg into the buffer the caller recorded, through the
 *      sanctioned muzix_copy_to_proc() - which forwards to
 *      muzix_mm_copy_kernel_to_user(), so the MM gate is satisfied without
 *      touching ipc.c;
 *   2. stamp the slot's resume_ret with the reply's m1_i1, so the resumed
 *      syscall returns the real answer rather than the park placeholder;
 *   3. clear DELIVERED, because the message has been handed over and is no
 *      longer waiting for a collect.
 *
 * The buf recorded is the one the caller passed on the way IN.  It is captured
 * here because it cannot be captured later: after the park the frame that held
 * it is gone (see muzix_syscalls_note_recv_buffer).
 *
 * A copy that fails, or a buffer address that was never recorded, is not
 * fatal.  The message stays in p_msg with DELIVERED set, so the caller's next
 * receive collects it by the existing path.  That fallback is why this is an
 * enhancement to the wake rather than a replacement for the collect.
 */
static void muzix_syscalls_deliver_parked(muzix_userspace_syscall_context_t *ctx,
                                          int woken_slot)
{
    muzix_kernel_proc_table_t *table = ctx->kernel->process_table;
    muzix_ipc_proc_t *rp;
    muzix_ipc_message_t msg;
    int client;

    if (woken_slot <= 0 || woken_slot >= MUZIX_PROC_TABLE_MAX) {
        return;
    }
    rp = muzix_ipc_proc(&g_ipc, (int16_t)woken_slot);
    if (!rp) {
        return;
    }
    if (!(rp->p_flags & MUZIX_IPC_FLAG_DELIVERED)) {
        return;
    }

    msg = rp->p_msg;
    client = rp->p_msg.m_source;

    /*
     * RECORD WHO SENT THIS, ON THE WOKEN PROCESS, BEFORE IT RUNS.
     *
     * ipc_client is what SYS_IPC_SRV_COPY_*_CLIENT copy through, and it was
     * never set for a server that parked to receive: the only writer was the
     * tail of muzix_handle_ipc_sendrec(), which sits below the park's
     * `return 0` and so never executes - the frame is abandoned when
     * muzix_kernel_switchout() hands the processor away.  g_loop is
     * zero-initialised, so slots[FS].ipc_client held 0 for the shell's very
     * first write, the SRV_COPY handler refused it as "no client", and the byte
     * never left the shell's stack.  That is the whole of the silent banner.
     *
     * It is worse than a constant zero, which is why this is not a one-line
     * copy fix.  When the FS server later sends its own REPLY, the same tail
     * does run, and stamps slots[FS].ipc_client with msg.m_source - which on
     * that path is the FS server's own slot.  So after the first exchange
     * every server-to-client copy resolved the client's address against the
     * SERVER's map, which is the same silent-garbage class as the one-sided
     * "user to kernel" copy that preceded it.
     *
     * Stamping it here, from the delivered message's own m_source, closes both:
     * the value is the real sender, it is written while the frame that knows
     * it is still live, and it lands before the woken process can issue any
     * copy that depends on it.
     */
    if (client >= 0 && client < MUZIX_PROC_TABLE_MAX && table) {
        table->slots[woken_slot].ipc_client = (uint8_t)client;
    }

    /* The resumed caller's return value, stamped on the slot.  This is a
     * scalar the resumed caller reports from libc: a byte count for write, a
     * descriptor for open, a status for the rest.  Leaving it at the park
     * placeholder is what made every write report zero.
     *
     * WHICH VALUE DEPENDS ON THE DIRECTION THE MESSAGE TRAVELS, and the
     * receiver's own p_getfrom is what says which it is - mini_send() sets
     * DELIVERED and clears RECEIVING on delivery but leaves p_getfrom as the
     * receiver last set it:
     *
     *   p_getfrom == MUZIX_IPC_ANY - a process parked waiting for any message,
     *     which in this tree is a SERVER in server_recv().  What is being
     *     handed to it is a REQUEST, and its receive reports SUCCESS - 0 -
     *     with the request itself sitting in its buffer.  m1_i1 of a request
     *     is an argument (a descriptor, a buffer address), never a status.
     *
     *   p_getfrom == a specific slot - a client parked waiting for that one
     *     peer, which is the routed-syscall shape waiting for its server's
     *     reply.  There m1_i1 IS the status the caller reports.
     *
     * Stamping the request's m1_i1 unconditionally was measured on 2026-10-15
     * and cost the prompt after every command.  SYS_GETCWD carries the
     * caller's buffer address in m1_i1 - 0xA29D for the shell's cwd[] - which
     * as int16_t is negative, so the FS server's server_recv() returned
     * negative, muzix_server_run() read that as "resumed without a message"
     * and retried without ever looking at the request it had just been handed.
     * Traced: the request was delivered (DELIVERED, buffer copied) and the
     * server parked straight back into receive; the reply that should have
     * answered it never existed.  Every other routed syscall survived only
     * because its m1_i1 is a small descriptor - 0, 1 or 2 - which is
     * non-negative, so the same latent bug was invisible there.
     *
     * The receive succeeded; its return value is 0. */
    if (client >= 0 && client < MUZIX_PROC_TABLE_MAX && table) {
        muzix_proc_slot_t *dst = &table->slots[woken_slot];
        uint16_t retval = (rp->p_getfrom == MUZIX_IPC_ANY)
                              ? (uint16_t)0
                              : (uint16_t)msg.m1_i1;
        muzix_proc_table_set_resume(table, woken_slot,
                                    dst->resume_pc, dst->resume_sp,
                                    retval);
    }

    if (rp->p_user_addr != 0) {
        /* Both are userspace addresses, so both halves go through the MM's
         * map-to-map copy - the same one the servers use, and never a
         * single-sided primitive whose "kernel_buf" would be this process's
         * stack while the client's page is installed in that window. */
        if (muzix_copy_to_proc(ctx, (uint8_t)woken_slot, rp->p_user_addr,
                               (const uint8_t *)&msg, sizeof(msg)) != 0) {
            /* Refused: leave DELIVERED set so the collect path still works. */
            return;
        }
    }

    /*
     * HANDED OVER EITHER WAY, so clear the bit in both arms.
     *
     * Returning early here - which is what this did when p_user_addr was 0 -
     * leaves DELIVERED set, and the caller's NEXT receive then collects this
     * reply from mini_rec() without ever sending a request.  A routed syscall
     * parks with p_user_addr == 0 on purpose: its answer is the scalar stamped
     * above, and there is no buffer to fill.  Bailing out of the hand-over for
     * that case turns every second write() into a replay of the first one's
     * answer.
     *
     * The refused-copy case above keeps the bit and returns, deliberately: a
     * copy that did not happen has NOT been handed over.
     */
    rp->p_flags &= (uint8_t)~MUZIX_IPC_FLAG_DELIVERED;
}

/*
 * Hand a delivered message to a process that is parked in sendrec().
 *
 * mini_send() writes a message into a blocked receiver's p_msg and sets
 * DELIVERED.  It cannot write the receiver's userspace memory itself - it is
 * IPC core, and tools/check_memory_manager.rb only admits a caller of the copy
 * primitives that defines an muzix_mm_* function - but it does know the exact
 * moment a message becomes the parked process's business, and so does the
 * caller of this function: it just called mini_send() and the woken process
 * number came back in the return code.
 *
 * So the copy happens here, in the syscall layer, on a kernel stack that is
 * still live and with the kernel map still installed, which is the one place
 * both truths hold.  The alternative - resuming the parked process and letting
 * it collect the message with its next mini_rec() - is why the FS server
 * answered stale requests for so long: it resumes into `server_recv()`'s
 * return with resume_ret == 0 and no message in `in`, handles whatever the last
 * request left there, and only then gets the real one.
 *
 * The DELIVERED bit is cleared here rather than by mini_rec(): the message has
 * been delivered, and a parked process that has been handed its reply must not
 * find it again on its next receive.  If the copy is refused - a user address in
 * window 0 or 3, past the staging cap - the bit is left set so the old collect
 * path still works.
 *
 * A parked receive with p_user_addr == 0 has no message buffer to write: that is
 * a kernel-routed syscall, whose answer is a scalar return value rather than a
 * message.  There is nothing to copy, but resume_ret still has to be the reply's
 * value or every routed write returns 0 and its caller reads that as failure.
 */


/* Send-only IPC: MINIX's send() with no receive attached.
 *
 * SYS_IPC_SENDREC to a specific slot runs mini_send and then mini_rec FROM THAT
 * SAME PEER (kernel/ipc.c:390-397).  For a server replying to its client that
 * second half is a thief, and it cost this tree a measurable bug: the server's
 * mini_rec blocks on the client, so the client's NEXT request - mini_send
 * delivers to a receiver waiting on exactly that caller (kernel/ipc.c:153-158)
 * - is copied into the reply sendrec's dead buffer instead of into the
 * server's own ANY-receive, where nothing ever looks for it again.  Measured
 * on the emulator: the shell read five bytes over five such round trips and
 * the sixth read parked forever, because the FS server's handle_read never saw
 * the request that had landed in the dead reply frame.  MINIX keeps the halves
 * separate for exactly this reason - replies go out with send(), and the next
 * request is collected by the server's own receive() - and so does this
 * handler: send, wake and deliver, no receive, no park. */
/*
 * A per-slot OUTGOING message buffer for the kernel-routed send paths.
 *
 * mini_send()'s queue branch books the message with a pointer to the SENDER's
 * storage (kernel/ipc.c), and the sender is blocked between the queue and the
 * collect - so that storage has to outlive the park.  A stack local does not:
 * muzix_handle_ipc_send(), muzix_handle_ipc_sendrec() and the routed-syscall
 * send all declared `muzix_ipc_message_t msg;` and passed &msg, and every one
 * of them parks the caller when the destination is busy, leaving the queue
 * pointing into a dead kernel frame - the next process's syscall frames land
 * on exactly those bytes.  Measured before this fix: the FS received m_type
 * 0x32/0x00 where SYS_WRITE/SYS_GETCWD had been sent.
 *
 * So the kernel-routed senders stage their message here and pass that.  The
 * index is the caller's own kernel slot, the same value muzix_current_slot()
 * returns and this file's bounds guards already use - no procs[] indexing and
 * no NR_TASKS arithmetic (tools/check_proc_identity.rb).  A slot has at most
 * one message in flight, because its process is blocked while it is queued.
 */
static muzix_ipc_message_t g_route_msg[MUZIX_PROC_TABLE_MAX];

static int32_t muzix_handle_ipc_send(
    muzix_userspace_syscall_context_t *ctx,
    int16_t dest_slot,
    muzix_ipc_message_t *user_msg)
{
    muzix_ipc_message_t msg;
    muzix_kernel_proc_table_t *table;
    int current_slot;
    int ipc_slot;
    int32_t result;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    current_slot = muzix_current_slot(ctx);

    if (current_slot < 0) {
        return -1;
    }

    /* The caller is its own process number - the same spelling the sendrec
     * handler uses, and for the same reason: mini_send() queues the caller on
     * the destination's callerq and mini_rec() walks that queue. */
    ipc_slot = current_slot;

    /* Copy message from userspace to kernel, exactly as the sendrec handler
     * does; the send half below reads this copy. */
    if (muzix_copy_user_from_current(ctx, (uint16_t)user_msg, (uint8_t *)&msg, sizeof(msg)) != 0) {
        return -1;
    }

    /* The sender is whoever is sending, not the requester this is a reply to */
    msg.m_source = ipc_slot;

    if (current_slot >= 0 && current_slot < MUZIX_PROC_TABLE_MAX) {
        g_route_msg[current_slot] = msg;
        result = muzix_ipc_send(&g_ipc, ipc_slot, dest_slot, &g_route_msg[current_slot]);
    } else {
        result = -1;
    }
    if (result < 0) {
        return result;
    }

    if (result >= 10) {
        /* A process was woken by the delivery.  It has to be readied in the
         * KERNEL process table as well as the IPC table, or pick_next() never
         * dispatches it - the same decode the sendrec handler's else branch
         * performs, and muzix_syscalls_deliver_parked() copies the message into
         * the woken process's own buffer and stamps its resume value while
         * this frame is still live. */
        int woken_slot = result - 10;
        if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_syscalls_deliver_parked(ctx, woken_slot);
            muzix_proc_table_ready(table, woken_slot);
        }
    }

    return result;
}

/* ---- SYS_EXEC -------------------------------------------------------------
 *
 * Replace the calling process with a program from the filesystem.
 *
 * Until this handler existed, MUZIX_USER_SYS_EXEC (2) fell off the end of
 * muzix_syscall_to_server()'s table - it matches none of the FS or PM ranges -
 * and the local dispatch in muzix_handle_userspace_syscall() had no branch
 * for it either, so every exec came back -1 and the shell could only print
 * "shell: cannot execute".  The loader itself has been here the whole time:
 * muzix_exec_load_for_slot() is what the kernel calls directly to start the
 * shell at boot, so its happy path - MZ header check, two-page allocation,
 * text copy through the MM, argv block on the new stack, slot map, entry
 * point - is exercised on every power-on.  What was missing was only the
 * syscall-shaped entry.
 *
 * The handler stages the user's path and argv into kernel memory (the loader
 * dereferences argv from kernel context), drives the load on the ROM path
 * (see muzix_exec_set_ipc_mode), and then does NOT return into the calling
 * image: the loader has already installed the new map, readied the slot and
 * set the entry point, so this clears the slot's resume point - the one the
 * trap recorded on the way into exec - and asks for a switch.  Whoever is
 * dispatched next sees resume_valid == 0 and enters the slot at its entry
 * point through muzix_zeta_enter_userspace(), exactly as the boot path does.
 * On failure the handler returns -1 and the caller's exec() sees it, which is
 * the clean failure the shell already reports. */

#define MUZIX_EXEC_PATH_MAX   128
#define MUZIX_EXEC_ARGV_MAX   16
#define MUZIX_EXEC_ARG_MAX    64

static char muzix_exec_path_buf[MUZIX_EXEC_PATH_MAX];
static const char *muzix_exec_argv_kernel[MUZIX_EXEC_ARGV_MAX + 1];
static char muzix_exec_arg_pool[MUZIX_EXEC_ARGV_MAX][MUZIX_EXEC_ARG_MAX];

/* ---- SYS_FORK -------------------------------------------------------------
 *
 * Give the calling process a child that is a copy of it, and let each of them
 * return from the syscall it is sitting in: the child with 0, the parent with
 * the child's pid.
 *
 * This handler exists because SYS_FORK used to be routed to the PM server like
 * any other, and the PM's handle_fork() answers it by calling sys_fork() -
 * which traps back into the kernel, routes to the PM again, and lands on the
 * PM's own callerq: the PM collects its own request as the reply, sys_fork()
 * returns the request's own m1_i1 (0), no child is ever created, and the
 * SHELL believes it forked successfully and takes its child branch with pid 0.
 * The child that never existed then runs `exec` on the parent's map, which is
 * why the machine rebooted with the boot stub on screen.
 *
 * The work is the kernel's, and the shape is specified by
 * kernel/test_syscalls.c's fork case (which the host runner cannot link, so it
 * has never executed - treat its assertions as the contract, not as coverage):
 * a new slot from the next free one, a stack page of the child's own, the
 * parent's active stack copied into it in staging-sized pieces through the
 * memory manager, and both processes resumed at the point the parent was
 * captured at - the child with 0, the parent with the child's pid.  The stack
 * copy is the whole of a real fork: without it the child runs its own call on
 * top of the frames the parent is suspended in, which is what the trap's
 * muzix_fork_stack_len measurement exists to prevent.
 *
 * The pid is not slot+1 as that case has it: on the real table slots 1-4 are
 * the shell and the three servers and pids 1-4 are taken, so muzix_proc_table_
 * fork()'s uniqueness check would refuse every child.  The first pid from 8 up
 * that no active slot holds is the same scheme without the collision. */

/* ---- SYS_GETRTC / SYS_SETRTC ---------------------------------------------
 *
 * Read or write the DS1302 from the syscall layer.
 *
 * These two used to be routed to the PM server like everything else, and the
 * PM's handle_getrtc() answers them by calling sys_getrtc() - a user syscall
 * that routes straight back to the PM, lands on the PM's own callerq, and is
 * collected by the PM as its own reply.  That is the same self-routing defect
 * the fork handler documents, and it is why `uptime` printed "cannot read the
 * clock; chip sent sec=00 min=00 hour=00": the app's buffer was never written
 * by anybody who had actually touched the chip.  The RTC is kernel hardware
 * (platform/zeta-v2/rtc_ds1302.c, muzix_zeta_rtc_get/set), so the syscall
 * layer reads it directly, exactly as it forks directly.
 *
 * GETRTC's second argument is the caller's "since" buffer: uptime is the
 * current reading minus the reading at boot, and that baseline lives in
 * exec_loader's muzix_boot_rtc, taken once on the first exec (the boot exec
 * always runs, so it exists by the time anyone can ask - but a caller that
 * somehow gets here first gets the same one-shot attempt, refused and said so
 * rather than answered with a stale or zero baseline). */

#define MUZIX_RTC_UNTRIED  0u
#define MUZIX_RTC_GOOD     1u
#define MUZIX_RTC_REFUSED  2u

static int32_t muzix_handle_rtc(muzix_userspace_syscall_context_t *ctx,
                                int32_t syscall_num,
                                int32_t arg1, int32_t arg2)
{
    muzix_rtc_time_t now;

    if (!ctx) {
        return -1;
    }

    if (syscall_num == MUZIX_USER_SYS_SETRTC) {
        if (muzix_copy_user_from_current(ctx, (uint16_t)arg1,
                                         (uint8_t *)&now, sizeof(now)) != 0) {
            return -1;
        }
        return (muzix_zeta_rtc_set(&now) == 0) ? 0 : -1;
    }

    if (muzix_zeta_rtc_get(&now) != 0) {
        return -1;
    }
    if (muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                   (const uint8_t *)&now, sizeof(now)) != 0) {
        return -1;
    }

    if (arg2 != 0) {
        if (muzix_boot_rtc_state == MUZIX_RTC_UNTRIED) {
            muzix_boot_rtc_state = MUZIX_RTC_REFUSED;
            if (muzix_zeta_rtc_get(&muzix_boot_rtc) == 0) {
                muzix_boot_rtc_state = MUZIX_RTC_GOOD;
            }
        }
        if (muzix_boot_rtc_state == MUZIX_RTC_GOOD) {
            if (muzix_copy_user_to_current(ctx, (uint16_t)arg2,
                                           (const uint8_t *)&muzix_boot_rtc,
                                           sizeof(muzix_boot_rtc)) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* ---- SYS_TIMES / SYS_PROCTAB ---------------------------------------------
 *
 * One slot of the process table, out to the caller's buffer.
 *
 * Both used to be routed to the PM server, whose handlers answer them by
 * calling sys_proctab() - a user syscall that routes straight back to the PM,
 * lands on the PM's own callerq, and is collected by the PM as its own reply.
 * The self-routing defect the fork and RTC handlers above document, twice
 * more: every one of the nine services the PM still handled this way returned
 * the request's own first argument instead of an answer, so `top` and
 * `loadavg` walked a table that was never read and printed nothing.
 *
 * The table is kernel state and the syscall layer already holds it, so these
 * are answered here, exactly as GETRTC and FORK already are.  The reply is the
 * FRONT OF THE SLOT copied out verbatim: struct proc_info is the first
 * MUZIX_PROC_INFO_SIZE bytes of muzix_proc_slot_t, field for field and in the
 * same order, and kernel/proc_table.h checks every offset at compile time -
 * which is why this costs one transfer and no per-field code, and why the
 * counters, the identity columns and the command name all arrive together.
 * The caller's return value is the copy's own result, the same value the PM's
 * handler put in m1_i1 for the routed path to stamp into resume_ret.
 *
 * SYS_TIMES names the calling process's slot; SYS_PROCTAB takes one so a
 * program can enumerate the table.  A slot outside the table is refused rather
 * than read from outside it; a FREE slot is answered rather than refused - it
 * reads back with active == 0, which is what a program walking
 * 0..MUZIX_PROC_INFO_SLOTS-1 is told to skip (lib/syscall.h). */
static int32_t muzix_handle_proctab(muzix_userspace_syscall_context_t *ctx,
                                    int32_t syscall_num,
                                    int32_t arg1, int32_t arg2)
{
    muzix_kernel_proc_table_t *table;
    muzix_proc_slot_t *slot;
    int target;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop || !arg1) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    target = (syscall_num == MUZIX_USER_SYS_TIMES) ? muzix_current_slot(ctx)
                                                   : (int)arg2;
    if (target < 0 || target >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }
    slot = &table->slots[target];

    return muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                      (const uint8_t *)slot,
                                      MUZIX_PROC_INFO_SIZE);
}

/* ---- SYS_LOAD -------------------------------------------------------------
 *
 * The three Q14 busy-fraction windows - 1, 5 and 15 minutes - out to the
 * caller's uint32_t[3].
 *
 * This used to be routed to the PM server, whose handle_load() answers it with
 * sys_load(), which routes straight back to the PM: the self-routing defect
 * again, so `loadavg` and `top` printed three zeros because no copy-out ever
 * ran and the return value was the request's own first argument.  kernel/load.c
 * is linked into the kernel and keeps the windows, so the syscall layer reads
 * them directly - the arrangement the pre-routing kernel had, and the one the
 * emulator measurements in AGENTS.md were taken under.
 *
 * The fold happens HERE, on the way in, and not on a timer: the figure wanted
 * is the average over the interval that has just ended, and an interval is
 * only known once it has ended.  A caller that never reads pays nothing, and a
 * caller that reads often gets an average over a short recent interval rather
 * than the same figure twice.
 *
 * The reply is twelve bytes at the caller's buffer, which is exactly what
 * apps/loadavg.c, apps/top.c and apps/hwdiag.c consume as uint32_t[3]; the
 * return value is the copy's own result, the m1_i1 the PM's handler would have
 * replied. */
static int32_t muzix_handle_load(muzix_userspace_syscall_context_t *ctx,
                                 int32_t arg1)
{
    muzix_kernel_proc_table_t *table;
    uint32_t windows[3];
    int i;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop || !arg1) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    muzix_load_sample(table, muzix_tick_now());
    for (i = 0; i < 3; i++) {
        windows[i] = muzix_load_window(i);
    }

    return muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                      (const uint8_t *)windows,
                                      (uint16_t)sizeof(windows));
}

/* ---- SYS_GETUID / SYS_GETGID / SYS_SETUID / SYS_SETGID / SYS_UMASK --------
 *
 * The credential pair and the creation mask.
 *
 * All five used to be routed to the PM server, whose handlers answer them with
 * the sys_* wrappers - each of which routes straight back to the PM and is
 * collected by the PM as its own reply.  So sys_getuid() returned the request's
 * own first argument, sys_umask() answered with a pointer rather than the old
 * mask, and a setuid that should have been refused was reported as success.
 * The credentials live in the process slot, and the syscall layer already
 * holds both the slot and the ctx copy, so these are answered here.
 *
 * The three setters write BOTH copies: the ctx fields the next syscall's
 * prologue refreshes the caller from, and the slot it refreshes them FROM.
 * Writing only the ctx copy leaves the slot holding the old value, so the next
 * syscall undoes the change the caller was just told had succeeded.
 *
 * UMASK answers with the OLD mask, which is what POSIX umask(2) does and what
 * the file-creation modes in fs/fs_service.c are masked against. */
static int32_t muzix_handle_ids(muzix_userspace_syscall_context_t *ctx,
                                int32_t syscall_num, int32_t arg1)
{
    muzix_proc_slot_t *process;

    if (!ctx) {
        return -1;
    }

    if (syscall_num == MUZIX_USER_SYS_GETUID) {
        return ctx->uid;
    }
    if (syscall_num == MUZIX_USER_SYS_GETGID) {
        return ctx->gid;
    }

    process = muzix_current_process(ctx);

    if (syscall_num == MUZIX_USER_SYS_UMASK) {
        int32_t old_mask = ctx->umask;
        ctx->umask = (uint16_t)arg1 & 0777u;
        if (process) {
            process->umask = ctx->umask;
        }
        return old_mask;
    }

    if (syscall_num == MUZIX_USER_SYS_SETUID) {
        if (arg1 < 0 || arg1 > 0xffff ||
            (ctx->uid != (uint16_t)arg1 && ctx->euid != 0)) {
            return -1;
        }
        ctx->uid = (uint16_t)arg1;
        ctx->euid = (uint16_t)arg1;
        if (process) {
            process->uid = ctx->uid;
            process->euid = ctx->euid;
        }
        return 0;
    }

    /* MUZIX_USER_SYS_SETGID - the same rule against the group pair. */
    if (arg1 < 0 || arg1 > 0xffff ||
        (ctx->gid != (uint16_t)arg1 && ctx->euid != 0)) {
        return -1;
    }
    ctx->gid = (uint16_t)arg1;
    ctx->egid = (uint16_t)arg1;
    if (process) {
        process->gid = ctx->gid;
        process->egid = ctx->egid;
    }
    return 0;
}

/* ---- SYS_SIGNAL -----------------------------------------------------------
 *
 * Queue a bounded default-action signal (1..16) for a process, BY PID.
 *
 * This used to be routed to the PM server, whose handle_signal() answers it
 * with sys_signal() - the self-routing defect once more, so no signal was ever
 * queued and the reply was the request's own first argument.  The pending set
 * is a field of the kernel's process slot and muzix_proc_table_queue_signal()
 * already validates the signal number and the target, so the syscall layer can
 * do the whole of it.
 *
 * A pid rather than a slot, because a pid is the only thing a caller has: the
 * scan is the one the pre-routing kernel made, and the permission test behind
 * it is the same too - root may signal anything, anyone else only a process
 * whose real or effective uid is their own. */
static int32_t muzix_handle_signal(muzix_userspace_syscall_context_t *ctx,
                                   int32_t arg1, int32_t arg2)
{
    muzix_kernel_proc_table_t *table;
    muzix_proc_slot_t *target;
    int slot;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop || ctx->pid < 0 ||
        arg1 <= 0 || arg1 > 0xff || arg2 <= 0 || arg2 > 16) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
        if (table->slots[slot].active &&
            table->slots[slot].pid == (uint8_t)arg1) {
            break;
        }
    }
    if (slot >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }
    target = &table->slots[slot];

    if (ctx->euid != 0 && ctx->uid != target->uid &&
        ctx->uid != target->euid) {
        return -1;
    }

    return muzix_proc_table_queue_signal(table, slot, (uint8_t)arg2) == 0
               ? 0
               : -1;
}

/* ---- SYS_EXIT and SYS_WAIT -----------------------------------------------
 *
 * The kernel's own end-of-process pair.
 *
 * Both used to be routed to the PM server, whose handlers answer them with
 * sys_exit()/sys_wait() - user syscalls that route straight back to the PM and
 * are collected by the PM as its own reply.  The self-routing defect the fork
 * and RTC handlers document, twice more: the shell's `wait(&status)` after a
 * forked command therefore never learned anything, and `uptime` ran and then
 * the shell never returned to its prompt.
 *
 * EXIT is the one-way door: record the process as a zombie of its parent, give
 * its pages back to the memory manager, wake a parent that is waiting, and ask
 * for a switch - the process must not come back from this syscall, and
 * muzix_proc_table_exit() has already unlinked it and cleared current, so the
 * scheduler dispatches somebody else.
 *
 * WAIT is a scan with a park behind it: look for a zombie of the caller; if
 * there is one, reap it and answer with its pid; if there is none, park with
 * the 0xFFFF sentinel (the "infinite" park that muzix_proc_table_wake_expired()
 * now leaves alone) and re-scan when the child's EXIT readies the caller. */

static int muzix_handle_exit(muzix_userspace_syscall_context_t *ctx,
                             int32_t status)
{
    muzix_kernel_proc_table_t *table;
    int slot;
    int parent_slot;
    uint8_t parent_pid;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    slot = muzix_current_slot(ctx);

    if (slot < 0) {
        return -1;
    }

    parent_pid = table->slots[slot].parent_pid;


    /* The pages first, while the slot still says it owns them: exit_status()
     * records the zombie on a slot that has already been unlinked, and a page
     * released after that is a page nobody ever returns to the memory
     * manager. */
    muzix_proc_table_release_pages(table, slot, &g_mm_service);
    muzix_proc_table_exit_status(table, slot, parent_pid, (uint8_t)status);

    /* The IPC entry goes with the slot.  muzix_handle_fork() registered it,
     * and leaving it registered would make the NEXT fork's registration
     * fail - which would hand that child the same P_SLOT_FREE reply-refusal
     * this line exists to prevent.  The four slots kernel_main() registered
     * itself are unaffected: none of them ever exits. */
    muzix_ipc_unregister_server(&g_ipc, (int16_t)slot);

    /* A parent blocked in WAIT is parked in the proc table, not the IPC table,
     * so it is woke with the scheduler's own ready.  A parent that is merely
     * running will find the zombie on its next wait() by itself. */
    for (parent_slot = 0; parent_slot < MUZIX_PROC_TABLE_MAX; parent_slot++) {
        if (table->slots[parent_slot].active &&
            table->slots[parent_slot].pid == parent_pid) {
            muzix_proc_table_ready(table, parent_slot);
            break;
        }
    }

    muzix_kernel_switchout(ctx->kernel->loop);
    return 0;
}

static int muzix_handle_wait(muzix_userspace_syscall_context_t *ctx,
                             int32_t status_ptr)
{
    muzix_kernel_proc_table_t *table;
    int slot;
    uint8_t caller_pid;
    int i;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    slot = muzix_current_slot(ctx);

    if (slot < 0) {
        return -1;
    }

    caller_pid = table->slots[slot].pid;

    for (;;) {
        for (i = 0; i < MUZIX_PROC_TABLE_MAX; i++) {
            if (table->slots[i].exited &&
                table->slots[i].parent_pid == caller_pid) {
                /* fall through to the reap below */
                int child_pid = table->slots[i].pid;
                int32_t child_status = table->slots[i].exit_status;

                /* Reap: clear the zombie so the slot is ordinary memory again
                 * and another fork can take it. */
                table->slots[i].exited = 0;
                table->slots[i].exit_status = 0;

                if (status_ptr != 0) {
                    int32_t status_word = child_status;
                    if (muzix_copy_user_to_current(ctx, (uint16_t)status_ptr,
                                                   (const uint8_t *)&status_word,
                                                   sizeof(status_word)) != 0) {
                        return -1;
                    }
                }
                return child_pid;
            }
        }

        /* No zombie yet: wait for one.  The park is the sentinel "no
         * deadline", so only a child's EXIT ends it - the scheduler will not
         * hand this slot back on a clock tick.
         *
         * AND THIS HANDLER RETURNS AFTER THE SWITCHOUT.  IT DOES NOT LOOP.
         *
         * muzix_kernel_switchout() does not perform the switch: it publishes
         * the next slot's resume point (context_switch_to fills the window-0
         * cells and muzix_resume_banks) and RETURNS TO HERE, leaving the jump
         * to the trap, which sees muzix_switch_pending and follows
         * muzix_zeta_resume_from.  A handler that keeps going after the
         * switchout - which this one did, re-scanning and re-parking in a
         * loop - overwrites the published resume with the next one and never
         * lets the trap jump: the target it published is dispatched and then
         * un-published.  Measured with the loop in place: muzix_context_
         * switch_to was called with the forked child's slot (DE=0005) and the
         * child never executed a single instruction afterwards - not one
         * syscall, not the fork-return test - because the waiting handler's
         * next iteration published somebody else.
         *
         * So this returns 0, the shell's wait() sees 0, and the zombie is
         * reaped by the next wait: a child that exits later is found by the
         * scan the same way, and a shell that calls wait in a loop converges.
         * The alternative - resuming inside this handler - would need the
         * park to hand back control after the wake, which is the trap's jump
         * the loop was fighting. */
        muzix_proc_table_park(table, muzix_current_slot(ctx), 0xFFFF);
        muzix_kernel_switchout(ctx->kernel->loop);
        return 0;
    }
}

static int muzix_handle_fork(muzix_userspace_syscall_context_t *ctx)
{
    muzix_kernel_proc_table_t *table;
    muzix_proc_slot_t *parent;
    process_map_t child_map;
    int parent_slot;
    int child_slot = -1;
    uint8_t child_pid = 8;
    uint16_t pc;
    uint16_t sp;
    uint16_t stack_len;
    int slot;
    int i;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    parent_slot = muzix_current_slot(ctx);

    if (parent_slot < 0) {
        return -1;
    }

    parent = &table->slots[parent_slot];

    /* The next free slot, and a pid nothing active holds. */
    for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
        if (!table->slots[slot].active) {
            child_slot = slot;
            break;
        }
    }
    if (child_slot < 0) {
        return -1;
    }
    for (;;) {
        int taken = 0;
        for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
            if (table->slots[slot].active &&
                table->slots[slot].pid == child_pid) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            break;
        }
        child_pid++;
    }

    /* What the trap published for THIS syscall: where the parent returns to,
     * and how much of its stack is in use.  Both are window-0 cells the trap
     * fills before the kernel map goes on. */
    pc = muzix_resume_pc;
    sp = muzix_resume_sp;
    stack_len = muzix_fork_stack_len;

    if (muzix_proc_table_fork(table, parent_slot, child_slot, child_pid) != 0) {
        return -1;
    }

    /* The shared image: the child runs the parent's text and data until it
     * execs, exactly as muzix_proc_table_fork()'s own comment describes.  Only
     * the stack is the child's own, because that is the only part the child
     * could damage before the exec replaces everything. */
    child_map = parent->map;

    if (stack_len != 0) {
        uint8_t stack_page = 0;

        if (muzix_mm_alloc_page(&g_mm_service, &stack_page) != 0) {
            stack_page = 0;
        }
        if (stack_page != 0) {
            child_map.pages[1] = stack_page;

            /* Copy the parent's live stack into the child's page, in pieces:
             * read the parent through the MM into the fork scratch buffer,
             * write it on into a map that is not published yet.  muzix_mm_copy_
             * kernel_to_map() takes the map directly for exactly this reason -
             * the child's map is only installed if every chunk lands. */
            {
                uint16_t done = 0;
                int failed = 0;

                while (done < stack_len) {
                    uint16_t chunk = (uint16_t)(stack_len - done);
                    if (chunk > MUZIX_ZETA_COPY_STAGING) {
                        chunk = MUZIX_ZETA_COPY_STAGING;
                    }
                    if (muzix_mm_copy_map_to_kernel(&g_mm_service, &parent->map,
                                                    (uint16_t)(sp + done),
                                                    muzix_fork_stack_scratch,
                                                    chunk) != 0) {
                        failed = 1;
                        break;
                    }
                    if (muzix_mm_copy_kernel_to_map(&g_mm_service, &child_map,
                                                    (uint16_t)(sp + done),
                                                    muzix_fork_stack_scratch,
                                                    chunk) != 0) {
                        failed = 1;
                        break;
                    }
                    done = (uint16_t)(done + chunk);
                }

                if (failed) {
                    /* Give the page back and fall back to sharing the parent's
                     * stack: a table entry with nowhere to run is worse than
                     * the old behaviour, and the old behaviour at least
                     * produced a child. */
                    muzix_mm_free_page(&g_mm_service, stack_page);
                    child_map.pages[1] = parent->map.pages[1];
                } else {
                    /* The page is the child's, recorded as such so that its
                     * exit returns it rather than the parent's. */
                    table->slots[child_slot].owned_count = 1;
                    table->slots[child_slot].owned_pages[0] = stack_page;
                }
            }
        }
    }

    table->slots[child_slot].map = child_map;
    table->slots[child_slot].p_flags &= (uint8_t)~MUZIX_PROC_NO_MAP;

    /* Both processes come back to where the parent was, each with its own
     * answer: the child with 0, the parent with the child's pid.  The child's
     * is what the trap delivers first - current is moved to the child below,
     * so the switch resumes it - and the parent's sits in its slot until the
     * child yields or exits and the parent is dispatched again. */
    muzix_proc_table_set_resume(table, child_slot, pc, sp, 0);
    muzix_proc_table_set_resume(table, parent_slot, pc, sp, child_pid);

    /* The parent goes back in the ready queue, and this is not optional.
     *
     * A running process is not in the ready queue - pick_next() popped it when
     * it was dispatched, and park() is what puts a process back.  The parent
     * never parks here: its syscall does not block, so its slot was left with
     * a recorded resume point and no queue to be found in.  The scheduler did
     * not lose it to a bug in the table; it was never put back.  Measured: the
     * shell's own `pid = fork()` test ran exactly once, with pid 0, and then
     * no second run, no wait, no prompt, and no "Goodbye" - the machine had
     * one process where it started with two.
     *
     * ready() is the right call and not park(): the parent has work to do (its
     * recorded resume point), and the child is about to run. */
    /* The child's IPC identity, established HERE and nowhere else.
     *
     * kernel_main() registers the four slots it starts itself - slot 0, the
     * FS, MM and PM servers, and the shell - and before this change nothing
     * registered anybody else, so every child of SYS_FORK carried an IPC
     * entry with P_SLOT_FREE still set.  The consequences were measured on
     * 2026-10-14: mini_send() reads only the DESTINATION's free flag, so
     * the child's own requests went out fine, while every reply the FS sent
     * back was refused with E_BAD_DEST (traced: `MSEND bad: caller=12
     * dest=15 dflags=05`, slot 5 = RECEIVING | SLOT_FREE, 53 times in one
     * run).  The child parked in the syscall anyway and resumed with the
     * park placeholder 0, so readdir() answered 0 forever and `ls` looped
     * ninety times printing empty names; `date` survived only because its
     * output is written by the FS before the reply that would have carried
     * the return value.
     *
     * register_server() refuses a slot that is already registered, and a
     * slot that exited is unregistered in muzix_handle_exit() below, so the
     * pair is idempotent across reuse of the same slot by a later fork.
     * The servers do not come through here - kernel_fork_server() reaches
     * the same table through the system-service dispatch, and kernel_main
     * still registers those four slots itself. */
    muzix_ipc_unregister_server(&g_ipc, (int16_t)child_slot);
    (void)muzix_ipc_register_server(&g_ipc, (int16_t)child_slot,
                                    (int16_t)child_pid);

    table->current = child_slot;
    muzix_proc_table_ready(table, parent_slot);
    muzix_proc_table_ready(table, child_slot);


    /* The publisher, not the cells: muzix_kernel_switchout() fills the window-0
     * resume cells from the child's slot and raises muzix_switch_pending last,
     * once everything the jump needs is in place. */
    muzix_kernel_switchout(ctx->kernel->loop);

    return child_pid;
}

static int32_t muzix_handle_exec(muzix_userspace_syscall_context_t *ctx,
                                 int32_t arg1, int32_t arg2)
{
    muzix_kernel_proc_table_t *table;
    int current_slot;
    int argc = 0;
    int i;
    int rc;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    current_slot = muzix_current_slot(ctx);

    if (current_slot < 0) {
        return -1;
    }

    /* The path, one byte at a time: a block copy of MUZIX_EXEC_PATH_MAX bytes
     * would read past the end of whatever page the string happens to end in,
     * and the copy helpers validate the whole range against the process map,
     * so a short string at a window edge would fail as if the address were
     * bad.  Stopping at the NUL cannot run off the end. */
    for (i = 0; i < MUZIX_EXEC_PATH_MAX - 1; i++) {
        if (muzix_copy_user_from_current(ctx, (uint16_t)arg1 + (uint16_t)i,
                                         (uint8_t *)&muzix_exec_path_buf[i],
                                         1) != 0) {
            return -1;
        }
        if (muzix_exec_path_buf[i] == '\0') {
            break;
        }
    }
    muzix_exec_path_buf[MUZIX_EXEC_PATH_MAX - 1] = '\0';

    /* argv: walk the caller's pointer array one word at a time - again so the
     * read stops at the NULL instead of past the array - and copy each string
     * into a kernel pool.  muzix_exec_copy_argv() later dereferences these
     * from kernel context while writing the block onto the new stack, so the
     * strings have to be here, not at their user addresses. */
    muzix_exec_argv_kernel[0] = NULL;
    if (arg2 != 0) {
        for (i = 0; i < MUZIX_EXEC_ARGV_MAX; i++) {
            uint16_t str_ptr = 0;
            int j;

            if (muzix_copy_user_from_current(ctx,
                                             (uint16_t)arg2 + (uint16_t)(i * 2),
                                             (uint8_t *)&str_ptr, 2) != 0) {
                break;
            }
            if (str_ptr == 0) {
                break;
            }
            for (j = 0; j < MUZIX_EXEC_ARG_MAX - 1; j++) {
                if (muzix_copy_user_from_current(ctx,
                                                 (uint16_t)(str_ptr + (uint16_t)j),
                                                 (uint8_t *)&muzix_exec_arg_pool[i][j],
                                                 1) != 0) {
                    return -1;
                }
                if (muzix_exec_arg_pool[i][j] == '\0') {
                    break;
                }
            }
            muzix_exec_arg_pool[i][MUZIX_EXEC_ARG_MAX - 1] = '\0';
            muzix_exec_argv_kernel[i] = muzix_exec_arg_pool[i];
            argc++;
        }
    }
    muzix_exec_argv_kernel[argc] = NULL;


    /* The rest of the loader's argument block.  The MM service and the process
     * table are the same two objects the other handlers in this file reach
     * through ctx; the loader's own fallbacks cover the boot-time spelling of
     * these, and the syscall spelling must not rely on that. */
    g_muzix_exec_load_args.mm = &g_mm_service;
    g_muzix_exec_load_args.processes = table;
    g_muzix_exec_load_args.slot = current_slot;
    g_muzix_exec_load_args.path = muzix_exec_path_buf;
    g_muzix_exec_load_args.argv = muzix_exec_argv_kernel;

    muzix_exec_set_ipc_mode(false);

    rc = muzix_exec_load_for_slot(muzix_exec_path_buf, muzix_exec_argv_kernel,
                                  current_slot);
    if (rc != 0) {
        return -1;
    }

    /* The calling image is gone: its pages were released by the loader and its
     * map replaced.  Clear the resume point the trap captured on entry so the
     * scheduler takes the entry-point path for this slot instead of jumping
     * back into a program that is no longer mapped, then hand the processor
     * away.  muzix_kernel_switchout() publishes the next slot and raises
     * muzix_switch_pending; the trap sees the flag and follows
     * muzix_zeta_resume_from, which never comes back here. */
    table->slots[current_slot].resume_valid = 0;
    muzix_kernel_switchout(ctx->kernel->loop);

    return 0;
}

static int32_t muzix_handle_ipc_sendrec(
    muzix_userspace_syscall_context_t *ctx,
    int16_t dest_slot,
    muzix_ipc_message_t *user_msg)
{
    muzix_ipc_message_t msg;
    muzix_kernel_proc_table_t *table;
    int current_slot;
    int ipc_slot;
    int32_t result;
    
    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }

    table = &ctx->kernel->loop->system.startup.proc_table;
    current_slot = muzix_current_slot(ctx);

    if (current_slot < 0) {
        return -1;
    }

    /* The caller is its own process number, not an offset of it.  This used to
     * be `10 + current_slot`, which is the array index register_server() writes
     * into; fed to muzix_ipc_sendrec() as `caller` it selected
     * procs[NR_TASKS + 10 + slot], an unregistered slot, so every send and
     * every receive was made on the wrong process.  mini_send() puts the caller
     * in the destination's callerq and mini_rec() walks that queue, so a
     * message could be queued against a slot that then never ran. */
    ipc_slot = current_slot;

    /* Copy message from userspace to kernel */
    if (muzix_copy_user_from_current(ctx, (uint16_t)user_msg, (uint8_t *)&msg, sizeof(msg)) != 0) {
        return -1;
    }

    /*
     * Write down where the reply has to land before this process can block, in
     * either half of sendrec.  After the park the frame that held this address
     * is gone, so the syscall layer that still holds it is the only one that
     * can record it - see muzix_syscalls_note_recv_buffer().
     */
    muzix_syscalls_note_recv_buffer(ipc_slot, (uint16_t)user_msg);

    /*
     * Remember who the USER said this message came from BEFORE overwriting it.
     *
     * `msg.m_source` is set to the current process below, because the sender of
     * a sendrec is whoever is calling it.  But a SERVER calling sendrec to
     * RECEIVE has no meaningful source of its own to offer - the source it
     * cares about is the client that sent the request it is about to read, and
     * that arrives with the delivered message.
     *
     * The old code read m_source at the bottom of this function to fill
     * ipc_client, but by then line 90 had already stamped the server's own slot
     * over it.  So slots[FS].ipc_client ended up holding 2 - the FS server
     * itself - and every sys_ipc_srv_copy_from_client()/to_client() moved bytes
     * between the server and ITSELF rather than between the server and its
     * client.  The copies succeeded, silently, in the wrong process.
     *
     * Note the ordering constraint this places: the delivered message
     * overwrites msg wholesale (muzix_copy_user_to_current at the other end),
     * so the source that matters is the one carried IN the delivered message,
     * not any value captured here.  What is captured here is the sendrec
     * caller's own identity, which is what a REPLY's source should be.
     */

    /* Set source to current process.  The server reads this back as the
     * destination of its reply, and it is also what ipc_client is derived from,
     * so it has to be the sender's process number. */
    msg.m_source = ipc_slot;

    /* Handle receive-only (dest_slot == MUZIX_IPC_ANY) */
    if (dest_slot == MUZIX_IPC_ANY) {
        result = muzix_ipc_receive(&g_ipc, ipc_slot, MUZIX_IPC_ANY, &msg);
        if (result == 1) {
            /* Blocked waiting for message - park this process */
            if (current_slot >= 0 && current_slot < MUZIX_PROC_TABLE_MAX) {
                muzix_proc_table_charge(&table->slots[current_slot], 1);
            }
            muzix_proc_table_park(table, muzix_current_slot(ctx), 0xFFFF);  /* Infinite timeout - woken by IPC */
            
            /* Request the context switch through the publisher, not by
             * raising the flag here.
             *
             * muzix_kernel_switchout() is what both other park paths call, and
             * the flag is not the whole of a switch: muzix_context_switch_to()
             * fills the window-0 cells the trap resumes from (_muzix_resume_pc,
             * _sp, _ret and _muzix_resume_banks) and raises muzix_switch_pending
             * last, once everything the jump needs is published.  Setting the
             * flag on its own left those cells holding what the trap captured
             * for *this* syscall on the way in - the parking process's own
             * return address - and muzix_resume_banks holding whatever an
             * earlier publish left, which after boot is nothing: the cells are
             * a .ds 4 in kernel_entry.s's _CODE and the image is zero.  The
             * trap then resumed the parked process at its own return address
             * under banks 20 00 00 00 - windows 1-3 on ROM page 0 - and the
             * machine printed garbage and wandered into the boot stub.
             *
             * switchout() either publishes the next slot's resume point and
             * returns here (the trap then resumes it), or, for a slot that has
             * never run, dispatches it at its entry point and never returns.
             * For the FS server parking on its first sendrec that is the MM
             * server at its entry point. */
             muzix_kernel_switchout(ctx->kernel->loop);

             /*
              * Same abandoned frame as the route path below: this one is gone by
              * the time the message arrives, so the copy out is not written here.
              *
              * It is written at the other end.  mini_send() reports the process
              * it woke, and the syscall layer that called it still holds ctx and
              * a live stack, so muzix_syscalls_deliver_parked() does the copy
              * there, into the buffer muzix_syscalls_note_recv_buffer()
              * recorded above, and stamps resume_ret with the reply's m1_i1.
              * The resumed process therefore returns from this syscall with its
              * message buffer written and its return value answered.
              */
              return 0;
        }
    } else {
        /* Send via IPC - dest_slot is already an IPC slot from userspace */
        if (current_slot >= 0 && current_slot < MUZIX_PROC_TABLE_MAX) {
            g_route_msg[current_slot] = msg;
            result = muzix_ipc_sendrec(&g_ipc, ipc_slot, dest_slot, &g_route_msg[current_slot]);
        } else {
            result = -1;
        }
        if (result >= 10) {
            /* A process was woken - ready it in the kernel proc_table */
            int woken_slot = result - 10;
            if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
                /*
                 * The message this call sent is now the woken process's
                 * business.  Put it where that process will actually look for
                 * it, while this stack is still live enough to do the copy -
                 * the parked frame cannot, and resume_ret is set to the
                 * message's own value so its sendrec() answers with it.
                 */
                muzix_syscalls_deliver_parked(ctx, woken_slot);
                muzix_proc_table_ready(table, woken_slot);
            }
            result = 0;
        }
    }
    if (result != 0) {
        return -1;
    }
    
    /* Store the message source as the current IPC client for this process.
     *
     * m_source is the sender's PROCESS NUMBER - the same numbering this file
     * now uses everywhere (see muzix_ipc_server_slot) - so it can be stored
     * as-is.  It used to be held as `10 + kernel_slot` and then read back
     * without conversion, so ipc_client held 11 for the shell while
     * muzix_copy_to_proc()/muzix_copy_from_proc() reject any
     * target_slot >= MUZIX_PROC_TABLE_MAX (8): every SRV_COPY from a server to
     * its client was answered with EFAULT, indistinguishably from a genuinely
     * bad address.
     *
     * Validated on the way in because the sender slot comes off the wire: an
     * out-of-range number would otherwise index the proc table from outside
     * it.
     *
     * `msg.m_source` is read here, AFTER muzix_ipc_receive() has returned, and
     * that ordering is now the whole point.  On the collect path (mini_rec found
     * a message waiting in p_msg and returned 0 instead of blocking), msg holds
     * the DELIVERED message and m_source is the client that sent it - exactly
     * the number SYS_IPC_SRV_COPY_*_CLIENT needs.  The old code read the same
     * field but never got here: it sat below a `return 0` that the parked path
     * took, so ipc_client stayed 0 and every server-to-client copy was refused.
     *
     * Do not "fix" this by capturing the source earlier.  The value captured
     * earlier is the sendrec caller's own identity, which for a server's
     * receive is meaningless - the server did not supply it, it zeroed its
     * buffer (see server_recv()).  The source that matters arrives with the
     * delivered message and this is the first line at which it exists.
     */
    if (current_slot < MUZIX_PROC_TABLE_MAX) {
        int client_kernel_slot = msg.m_source;
        table->slots[current_slot].ipc_client =
            (uint8_t)((client_kernel_slot >= 0 &&
                       client_kernel_slot < MUZIX_PROC_TABLE_MAX)
                      ? client_kernel_slot : 0);
    }

    /* Copy result back to userspace */
    if (muzix_copy_user_to_current(ctx, (uint16_t)user_msg, (const uint8_t *)&msg, sizeof(msg)) != 0) {
        return -1;
    }

    return msg.m1_i1;
}

/*
 * The kernel slot of the process serving `server_id`.
 *
 * This returns a PROCESS NUMBER, which is what muzix_ipc_sendrec() and
 * muzix_ipc_receive() take: muzix_ipc_proc() turns a number into
 * &ipc->procs[NR_TASKS + nr], and NR_TASKS is 10, so slot 2 (FS) lives at
 * procs[12] under process number 2.  The return value used to be `10 + slot`,
 * which is the ARRAY INDEX, not the number - and it is passed straight on as
 * the destination.  So an SYS_WRITE destined for FS was delivered to
 * procs[NR_TASKS + 12] = procs[22], a slot nobody registered, and the FS server
 * never saw it.  The `+10` belongs to register_server(), which writes the array
 * directly and correctly; it does not belong here.
 */
/*
 * Resolve a process number to a live kernel slot, for a routed syscall.
 *
 * IT TAKES A PROCESS NUMBER, NOT A SERVER ID.
 *
 * This is the second half of the identity unification, and the half that was
 * still undone.  The old version walked the proc table looking for a slot whose
 * `server_id` field matched, which required a *third* numbering scheme
 * alongside the process number and the kernel slot: kernel_fork_server()
 * numbered FS=1, MM=2, PM=3, while MUZIX_PROC_* says FS=2, MM=3, PM=4.  Two
 * tables, two answers to "which number is the FS server", and every routed
 * syscall went to whichever one the lookup happened to use.
 *
 * The result was silent and total: muzix_syscall_to_server() returned
 * MUZIX_PROC_FS (2), the lookup found no slot with server_id == 2, this
 * function returned -1, and the caller fell through to
 * muzix_dispatch_userspace_syscall() - the non-IPC path.  So a write() from
 * the shell was handled by whatever local handler existed and never reached
 * the FS server at all.  No error, no trace, nothing in the banner.
 *
 * The process number IS the kernel slot, so there is nothing to search for:
 * check the slot is live and registered, and return it.  One number, one
 * meaning, nothing to reconcile.  server_id stays in muzix_kernel_proc_slot_t
 * only because kernel_fork_server() still passes it; nothing reads it any more.
 */
static int muzix_ipc_server_slot(muzix_kernel_proc_table_t *table, int proc_nr) {
    if (proc_nr < 0 || proc_nr >= MUZIX_PROC_TABLE_MAX) return -1;
    if (!table->slots[proc_nr].active) {
        return -1;   /* not running: nothing to route to */
    }
    return proc_nr;  /* the process number is the slot - nothing to translate */
}

static int muzix_syscall_to_server(int syscall_num) {
    if (syscall_num >= MUZIX_USER_SYS_OPEN && syscall_num <= MUZIX_USER_SYS_SEEK) return MUZIX_PROC_FS;
    if (syscall_num >= MUZIX_USER_SYS_MKDIR && syscall_num <= MUZIX_USER_SYS_MKNOD && syscall_num != MUZIX_USER_SYS_SIGNAL) return MUZIX_PROC_FS;
    if (syscall_num >= MUZIX_USER_SYS_READDIR && syscall_num <= MUZIX_USER_SYS_DUP) return MUZIX_PROC_FS;
    if (syscall_num >= MUZIX_USER_SYS_PIPE && syscall_num <= MUZIX_USER_SYS_IOCTL) return MUZIX_PROC_FS;
    /* The rest of the PM set is gone, and deliberately so.  SYS_TIMES,
     * SYS_PROCTAB, SYS_LOAD, SYS_GETUID, SYS_GETGID, SYS_SETUID, SYS_SETGID,
     * SYS_UMASK and SYS_SIGNAL are answered by the kernel-side interceptors in
     * muzix_handle_userspace_syscall_ipc(), which runs before this table is
     * consulted: routing them to the PM made the PM's own handlers trap back
     * into the PM, and every one of those nine services silently answered with
     * the request's own first argument.  The entries below are the PM calls
     * that are ALSO intercepted upstream (FORK/WAIT/EXIT, TIME/GETRTC/SETRTC);
     * they are taxonomic, and removing them is not required for correctness. */
    if (syscall_num == MUZIX_USER_SYS_FORK || syscall_num == MUZIX_USER_SYS_WAIT || syscall_num == MUZIX_USER_SYS_EXIT) return MUZIX_PROC_PM;
    if (syscall_num == MUZIX_USER_SYS_TIME || syscall_num == MUZIX_USER_SYS_GETRTC || syscall_num == MUZIX_USER_SYS_SETRTC) return MUZIX_PROC_PM;
    return 0;
}

/* ---- the ROM filesystem interceptors -----------------------------------
 *
 * open(), readdir() and close() on the ROM filesystem are answered HERE and
 * not routed to the FS server, because the FS server has no file operations
 * to route them to: servers/fs/fs_server.lk links fs_server + fs_clock +
 * tty_device + the bank primitives, the fs/ service layer is not among them,
 * and handle_open()/handle_readdir()/handle_read()/handle_stat() are stubs
 * that answer -1 with a comment saying exactly that.  The kernel owns the
 * ROM block device and the on-disk walk (rom_read.c), so it can serve them
 * directly - the same interceptor shape the kernel already uses for TIMES,
 * PROCTAB, RTC, LOAD and IDS.  rom_read.c carries the full argument. */

#define MUZIX_ROM_PATH_MAX 64

static int32_t muzix_handle_open(muzix_userspace_syscall_context_t *ctx,
                                 int32_t path_arg,
                                 int32_t flags)
{
    char path[MUZIX_ROM_PATH_MAX];
    /* THE TRAP SIGN-EXTENDS.  A userspace address of 0x888A arrives as the
     * int32_t 0xFFFF888A - the trap's argument words are 16-bit and whatever
     * fills the high half fills it with ones - so `path_arg < 0` is TRUE for
     * every address above 0x7FFF, and an open() of anything the linker put in
     * window 2 was refused by the guard before the copy ran.  Every other
     * handler takes the low half at the point of use; this one now does too. */
    uint16_t path_ptr = (uint16_t)path_arg;
    int i;

    (void)flags;
    if (path_ptr == 0) {
        return -1;
    }
    /* One byte at a time and NUL-terminated, the bound the exec path takes:
     * a fixed-length block copy would accept a path that runs to the end of
     * the buffer with no terminator at all. */
    for (i = 0; i < MUZIX_ROM_PATH_MAX - 1; i++) {
        if (muzix_copy_user_from_current(ctx, (uint16_t)(path_ptr + (uint16_t)i),
                                         (uint8_t *)&path[i], 1) != 0) {
            return -1;
        }
        if (path[i] == '\0') {
            break;
        }
    }
    path[MUZIX_ROM_PATH_MAX - 1] = '\0';

    return muzix_rom_open_path(path);
}

extern struct muzix_block_device g_block_dev;

static int32_t muzix_handle_rom_read(muzix_userspace_syscall_context_t *ctx,
                                     int32_t block_arg,
                                     int32_t buffer_arg)
{
    uint16_t block = (uint16_t)block_arg;
    uint16_t buffer_ptr = (uint16_t)buffer_arg;
    uint8_t block_buf[MUZIX_FS_BLOCK_SIZE];

    if (buffer_ptr == 0) {
        return -1;
    }

    if (rom_read_block(&g_block_dev, block, block_buf) != 0) {
        return -1;
    }

    if (muzix_copy_user_to_current(ctx, buffer_ptr, block_buf,
                                   MUZIX_FS_BLOCK_SIZE) != 0) {
        return -1;
    }

    return 0;
}

static int32_t muzix_handle_readdir(muzix_userspace_syscall_context_t *ctx,
                                    int32_t fd,
                                    int32_t index,
                                    int32_t entry_ptr)
{
    /* The userspace struct dirent (lib/libc.h) is inode + name[15], which is
     * exactly the on-disk entry: two bytes and fourteen, sixteen in all. */
    uint8_t entry[17];  /* inode(2) + name[15], matching userspace struct dirent */
    /* The same sign extension: see muzix_handle_open(). */
    uint16_t entry_addr = (uint16_t)entry_ptr;
    int result;

    if (entry_addr == 0) {
        return -1;
    }
    result = muzix_rom_readdir((int)fd, (uint16_t)index, entry);
    if (result == 0) {
        /* muzix_copy_to_proc() and not muzix_copy_to_current(): the latter is
         * syscalls.c's own and carries no declaration in syscalls.h, while
         * this one is the shared entry with the same destination - the
         * calling process, by its own slot. */
        if (muzix_copy_to_proc(ctx, (uint8_t)muzix_current_slot(ctx),
                               entry_addr, entry,
                               (uint16_t)sizeof(entry)) != 0) {
            return -1;
        }
    }
    return result;
}

static int32_t muzix_handle_read(muzix_userspace_syscall_context_t *ctx,
                                 int32_t fd,
                                 int32_t buf_arg,
                                 int32_t count)
{
    /* A kernel-side staging block, because the read lands here and not in the
     * caller's buffer: a process parked in a read has no frame to copy out of
     * when the data arrives, which is the same reason readdir's entry goes
     * through muzix_copy_to_proc() from kernel memory.  256 is the largest
     * single read cat makes, and asking for more answers "count" bytes of it
     * rather than failing - the loop above this one asks again. */
    uint8_t kbuf[256];
    /* The same sign extension: see muzix_handle_open(). */
    uint16_t buf_addr = (uint16_t)buf_arg;
    uint16_t want = (count > 256) ? 256 : (uint16_t)count;
    int n;

    if (buf_addr == 0 || count <= 0) {
        return -1;
    }
    n = muzix_rom_read((int)fd, kbuf, want);
    if (n > 0) {
        if (muzix_copy_to_proc(ctx, (uint8_t)muzix_current_slot(ctx),
                               buf_addr, kbuf, (uint16_t)n) != 0) {
            return -1;
        }
    }
    return n;
}

static int32_t muzix_handle_stat(muzix_userspace_syscall_context_t *ctx,
                                 int32_t path_arg,
                                 int32_t statbuf_arg)
{
    char path[MUZIX_ROM_PATH_MAX];
    /* The same sign extension: see muzix_handle_open(). */
    uint16_t path_ptr = (uint16_t)path_arg;
    uint16_t statbuf_ptr = (uint16_t)statbuf_arg;
    uint8_t inode_buf[32];
    int i;

    (void)ctx;
    if (path_ptr == 0 || statbuf_ptr == 0) {
        return -1;
    }
    for (i = 0; i < MUZIX_ROM_PATH_MAX - 1; i++) {
        if (muzix_copy_user_from_current(ctx, (uint16_t)(path_ptr + (uint16_t)i),
                                         (uint8_t *)&path[i], 1) != 0) {
            return -1;
        }
        if (path[i] == '\0') {
            break;
        }
    }
    path[MUZIX_ROM_PATH_MAX - 1] = '\0';

    if (muzix_rom_lookup_inode(path, inode_buf) != 0) {
        return -1;
    }

    if (muzix_copy_to_proc(ctx, (uint8_t)muzix_current_slot(ctx),
                           statbuf_ptr, inode_buf,
                           (uint16_t)sizeof(inode_buf)) != 0) {
        return -1;
    }
    return 0;
}

static int32_t muzix_handle_fstat(muzix_userspace_syscall_context_t *ctx,
                                  int32_t fd,
                                  int32_t statbuf_arg)
{
    uint16_t statbuf_ptr = (uint16_t)statbuf_arg;
    uint8_t inode_buf[32];

    if (statbuf_ptr == 0) {
        return -1;
    }
    if (muzix_rom_fd_is_synthetic((int)fd)) {
        if (muzix_rom_fstat((int)fd, inode_buf) != 0) {
            return -1;
        }
        if (muzix_copy_to_proc(ctx, (uint8_t)muzix_current_slot(ctx),
                               statbuf_ptr, inode_buf,
                               (uint16_t)sizeof(inode_buf)) != 0) {
            return -1;
        }
        return 0;
    }
    /* Non-synthetic fd: route to the FS server (same as before). */
    return -1;
}

static int32_t muzix_handle_seek(muzix_userspace_syscall_context_t *ctx,
                                 int32_t fd,
                                 int32_t offset,
                                 int32_t whence)
{
    (void)ctx;
    return muzix_rom_seek((int)fd, (uint32_t)offset, (int)whence);
}

static int32_t muzix_handle_getcwd(muzix_userspace_syscall_context_t *ctx,
                                   int32_t buf_arg,
                                   int32_t size)
{
    /* The ROM filesystem has exactly one directory and it is "/", so the
     * answer is a constant.  It is answered HERE because the routed path is
     * the one place the shell's prompt can hang: the FS server's
     * handle_getcwd() is a stub, and measured on 2026-10-15 the shell parked
     * in the getcwd of its prompt refresh after every command and did not
     * come back until the NEXT keystroke woke it - the prompt after a command
     * was missing, which reads as "ls does not return to the shell".  A
     * constant answer costs one string copy and takes the round trip out of
     * the prompt loop entirely.
     *
     * The return is the caller's own buffer address, which is what
     * lib/syscall.c's sys_getcwd() treats as success (`ret < 0` is the only
     * failure it looks for). */
    static const char root_cwd[] = "/";
    /* The same sign extension: see muzix_handle_open(). */
    uint16_t buf_addr = (uint16_t)buf_arg;

    if (buf_addr == 0 || size < 2) {
        return -1;
    }
    if (muzix_copy_to_proc(ctx, (uint8_t)muzix_current_slot(ctx),
                           buf_addr, (const uint8_t *)root_cwd,
                           (uint16_t)(sizeof(root_cwd))) != 0) {
        return -1;
    }
    return (int32_t)buf_addr;
}

static int32_t muzix_handle_userspace_syscall_ipc(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3)
{
    muzix_ipc_message_t msg;
    muzix_kernel_proc_table_t *table;
    int server_slot;
    int current_slot;
    int32_t result;
    
    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }
    
    table = &ctx->kernel->loop->system.startup.proc_table;
    current_slot = muzix_current_slot(ctx);
    
    if (current_slot < 0) {
        return -1;
    }
    
    memset(&msg, 0, sizeof(msg));
    /* The caller's own process number, as above: `10 + current_slot` was an
     * array index and made both the source and the destination of every
     * kernel-routed message land on unregistered slots. */
    int ipc_slot = current_slot;
    msg.m_source = ipc_slot;
    msg.m_type = syscall_num;
    msg.m1_i1 = arg1;
    msg.m1_i2 = arg2;
    msg.m1_i3 = arg3;
    msg.m1_p1 = (uint16_t)arg1;
    msg.m1_p2 = (uint16_t)arg2;
    msg.m1_p3 = (uint16_t)arg3;

    /*
     * SYS_PARK is NOT handled here.  muzix_handle_userspace_syscall() intercepts
     * it before this function is ever called (the block above its call to this
     * one), so a second park path in here is dead code - and it was a subtly
     * different one: it charged and parked without the `process` validity check
     * the live intercept makes first.  One park, one site.
     */

    if (syscall_num == MUZIX_USER_SYS_GETPID) {
        return ctx->pid;
    }
    if (syscall_num == MUZIX_USER_SYS_GETPPID) {
        return ctx->ppid;
    }
    
    /* NOT here: the internal MUZIX_SYS_YIELD - it used to be answered by an
     * early `if (syscall_num == MUZIX_SYS_YIELD) return 0;` in this block, and
     * MUZIX_SYS_YIELD is 10, which is also the user SYS_OPEN.  The trap cannot
     * tell the two namespaces apart, so that line answered every open() with
     * 0 - a descriptor that looks perfectly valid - and `ls` spent its life
     * calling readdir() on fd 0.  The internal numbers belong to the
     * record-driven dispatcher (muzix_handle_syscall, and the COPY/YIELD/EXIT
     * cases in muzix_dispatch_userspace_syscall()); on this path the number is
     * the user's, so 10 is SYS_OPEN and the interceptors below own it. */

    if (syscall_num == MUZIX_USER_SYS_IPC_SENDREC) {
        /* arg1 = dest_slot, arg2 = pointer to muzix_ipc_message_t in userspace */
        return muzix_handle_ipc_sendrec(ctx, (int16_t)arg1, (muzix_ipc_message_t *)(uint16_t)arg2);
    }

    if (syscall_num == SYS_IPC_SEND) {
        /* arg1 = dest_slot, arg2 = pointer to muzix_ipc_message_t in userspace.
         * Send only, no receive half - the reply path for servers, so that the
         * kernel's receive-from-the-same-peer in the sendrec handler cannot
         * steal the client's next request out from under the server's own
         * ANY-receive. */
        return muzix_handle_ipc_send(ctx, (int16_t)arg1, (muzix_ipc_message_t *)(uint16_t)arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_EXEC) {
        /* arg1 = path, arg2 = argv.  The load itself is the boot-time loader
         * reached through a syscall; see muzix_handle_exec() above for why the
         * caller is not resumed on success. */
        return muzix_handle_exec(ctx, arg1, arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_EXIT) {
        /* One-way: the process does not come back.  See muzix_handle_exit(). */
        return muzix_handle_exit(ctx, arg1);
    }

    if (syscall_num == MUZIX_USER_SYS_WAIT) {
        /* arg1 = the caller's status buffer.  See muzix_handle_wait(). */
        return muzix_handle_wait(ctx, arg1);
    }

    if (syscall_num == MUZIX_USER_SYS_OPEN) {
        /* arg1 = path, arg2 = flags.  The kernel's own ROM filesystem, not
         * the FS server: see the interceptors above. */
        return muzix_handle_open(ctx, arg1, arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_READDIR &&
        muzix_rom_fd_is_synthetic((int)arg1)) {
        /* arg1 = fd, arg2 = index, arg3 = the caller's struct dirent.  A
         * synthetic descriptor is the kernel's; any other fd still routes. */
        return muzix_handle_readdir(ctx, arg1, arg2, arg3);
    }

    if (syscall_num == MUZIX_USER_SYS_READ &&
        muzix_rom_fd_is_synthetic((int)arg1)) {
        /* arg1 = fd, arg2 = the caller's buffer, arg3 = count.  cat and
         * hwdiag both read a file this way; the console's fd 0 still routes
         * to the FS, which is the one handler there that is real. */
        return muzix_handle_read(ctx, arg1, arg2, arg3);
    }

    if (syscall_num == MUZIX_USER_SYS_SEEK &&
        muzix_rom_fd_is_synthetic((int)arg1)) {
        /* arg1 = fd, arg2 = offset, arg3 = whence. */
        return muzix_handle_seek(ctx, arg1, arg2, arg3);
    }

    /* SYS_GETCWD is routed to the FS server, which maintains g_fs_cwd
     * updated by handle_chdir.  No special kernel handling needed - the
     * routed syscall path copies arg1 (user buffer) and arg2 (size),
     * and the server's handle_getcwd copies g_fs_cwd back via SRV_COPY_TO_CLIENT. */
    
    if (syscall_num == MUZIX_USER_SYS_CLOSE &&
        muzix_rom_fd_is_synthetic((int)arg1)) {
        /* A synthetic descriptor holds nothing to close. */
        return 0;
    }

    if (syscall_num == MUZIX_USER_SYS_STAT) {
        return muzix_handle_stat(ctx, arg1, arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_FSTAT &&
        muzix_rom_fd_is_synthetic((int)arg1)) {
        return muzix_handle_fstat(ctx, arg1, arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_GETRTC ||
        syscall_num == MUZIX_USER_SYS_SETRTC) {
        /* arg1 = the caller's muzix_rtc_time_t buffer, arg2 = the optional
         * baseline buffer.  Handled here rather than routed to the PM, whose
         * handler answers it with a syscall that routes back to itself. */
        return muzix_handle_rtc(ctx, syscall_num, arg1, arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_FORK) {

        /* Fork is the kernel's own work and used to be routed to the PM, whose
         * handler answers it with a syscall that routes straight back to the
         * PM itself - so no child was ever created and the shell's "child" ran
         * exec on the parent's map.  See muzix_handle_fork() above. */
        return muzix_handle_fork(ctx);
    }

    if (syscall_num == MUZIX_USER_SYS_TIMES ||
        syscall_num == MUZIX_USER_SYS_PROCTAB) {
        /* arg1 = the caller's struct proc_info buffer, arg2 = the slot (read
         * only by PROCTAB).  The PM's handler answered by calling
         * sys_proctab(), which routed straight back to the PM itself - see
         * muzix_handle_proctab() above. */
        return muzix_handle_proctab(ctx, syscall_num, arg1, arg2);
    }

    if (syscall_num == MUZIX_USER_SYS_LOAD) {
        /* arg1 = the caller's uint32_t[3], for the 1, 5 and 15 minute windows.
         * See muzix_handle_load() above. */
        return muzix_handle_load(ctx, arg1);
    }

    if (syscall_num == MUZIX_USER_SYS_GETUID ||
        syscall_num == MUZIX_USER_SYS_GETGID ||
        syscall_num == MUZIX_USER_SYS_SETUID ||
        syscall_num == MUZIX_USER_SYS_SETGID ||
        syscall_num == MUZIX_USER_SYS_UMASK) {
        /* arg1 = the new id or mask, read only by the three setters.  See
         * muzix_handle_ids() above. */
        return muzix_handle_ids(ctx, syscall_num, arg1);
    }

    if (syscall_num == MUZIX_USER_SYS_SIGNAL) {
        /* arg1 = the target pid, arg2 = the signal 1..16.  See
         * muzix_handle_signal() above. */
        return muzix_handle_signal(ctx, arg1, arg2);
    }

    /* SYS_ROM_READ: block arg1, buffer arg2 */
    if (syscall_num == MUZIX_USER_SYS_ROM_READ) {
        return muzix_handle_rom_read(ctx, arg1, arg2);
    }

    
    /* SYS_IPC_COPY_TO_USER / SYS_IPC_COPY_FROM_USER are refused here, and the
     * reason is the MM boundary: their contract says "arg2 = kernel_buf", and
     * a process cannot name a kernel buffer.  What actually arrives is the
     * caller's own userspace address (servers/mm/mm_copy.c passes its stack
     * buffer), and muzix_copy_user_to_current() would dereference it under
     * the KERNEL map - the calling server's own page read as kernel memory.
     *
     * A server moving bytes between its own space and its client uses
     * SYS_IPC_SRV_COPY_TO_CLIENT/FROM_CLIENT below, which the kernel answers
     * with the map-to-map primitives; a process moving its own data uses
     * SYS_READ/SYS_WRITE.  The two wrappers stay in lib/syscall.h only for
     * the unlinked servers/mm/mm_copy.c, which now uses the SRV pair too.
     */
    if (syscall_num == SYS_IPC_COPY_TO_USER ||
        syscall_num == SYS_IPC_COPY_FROM_USER) {
        return -1;
    }
    
    if (syscall_num == SYS_IPC_SRV_COPY_TO_CLIENT) {
        /* arg1 = client_user_addr, arg2 = server_kernel_buf, arg3 = len */
        /* Use the stored IPC client for this process.  ipc_client was already
         * converted to a KERNEL slot where it is stored (above), so it can go
         * to muzix_copy_to_proc() as-is.  The old code stored it as an IPC slot
         * and every copy was refused here with EFAULT,
         * target_slot >= MUZIX_PROC_TABLE_MAX. */
        if (current_slot < MUZIX_PROC_TABLE_MAX) {
            uint8_t client_slot = table->slots[current_slot].ipc_client;
            if (client_slot != 0) {
                /* arg1 = address in the CLIENT, arg2 = address in THIS
                 * server's own space.  Both are userspace, so both halves go
                 * through the MM's map-to-map copy - never through the
                 * single-sided primitive, whose "kernel_buf" would be this
                 * server's stack while the client's page is installed in that
                 * very window.  See muzix_copy_client_to_server() in
                 * kernel/syscalls.c. */
                return muzix_copy_server_to_client(ctx, client_slot, (uint16_t)arg1,
                                                   (uint8_t)current_slot, (uint16_t)arg2,
                                                   (uint16_t)arg3);
            }
        }
        return -1;
    }

    if (syscall_num == SYS_IPC_SRV_COPY_FROM_CLIENT) {
        /* arg1 = address in the CLIENT, arg2 = address in THIS server's own space. */
        if (current_slot < MUZIX_PROC_TABLE_MAX) {
            uint8_t client_slot = table->slots[current_slot].ipc_client;
            if (client_slot != 0) {
                /*
                 * BOTH ends are userspace, so this MUST go through the MM's
                 * map-to-map copy and never through muzix_copy_from_proc().
                 *
                 * muzix_copy_from_proc() names its destination `kernel_buf`, and
                 * that is the contract: zeta_copy_user_to_kernel() installs the
                 * source's page, then hands the bytes to a destination it assumes
                 * is a KERNEL address.  A server's buffer is not one - it is a
                 * local in the server's own window.  Worse, the store happens
                 * after map_call_impl() has already restored the map it found on
                 * entry, which on the syscall path is the KERNEL map.  So the
                 * server's window address is resolved against kernel page 0x21,
                 * which per build/kernel.map is kernel _CODE.
                 *
                 * Two consequences, and both of them were measured:
                 *   - the client's bytes never reach the server's buffer at all,
                 *     so muzix_tty_write() transmits uninitialised stack;
                 *   - every console write stores into kernel code at an address
                 *     chosen by the FS server's frame layout.  That is why adding
                 *     a few bytes to fs_server.c changed whether ANYTHING appeared
                 *     on the console, echo included: the write target moves across
                 *     the boundary between dead code and the scheduler's hot path.
                 *
                 * See the long note in kernel/syscalls.c on
                 * muzix_copy_client_to_server().
                 */
                return muzix_copy_client_to_server(ctx, client_slot, (uint16_t)arg1,
                                                   (uint8_t)current_slot, (uint16_t)arg2,
                                                   (uint16_t)arg3);
            }
        }
        return -1;
    }
    
    server_slot = muzix_ipc_server_slot(table, muzix_syscall_to_server(syscall_num));

    if (server_slot < 0) {
        return muzix_dispatch_userspace_syscall(ctx, syscall_num, arg1, arg2, arg3);
    }

    /*
     * The kernel-routed half of a syscall: SEND TO THE SERVER, THEN PARK.
     *
     * This is the piece that was missing, and it is why the console was silent.
     * The old code sent the message, decoded the wake code, readied the server in
     * the kernel process table, and returned msg.m1_i1 straight to the caller.
     * It never parked the caller.  The caller had not received a reply - it had
     * only *sent* a request - so it returned a value that did not exist yet, and
     * then carried on and issued the same syscall again.  A shell running
     * printf() therefore wrote its banner one character at a time forever, and
     * every single character was a fresh request that the server answered into a
     * buffer nobody had been told about.
     *
     * The shape this must have is the same three steps
     * muzix_handle_ipc_sendrec() uses for the receive-only case above, because
     * it is the same operation: send a request, block until the server replies.
     * Only the park-and-return is new here.
     *
     * The send and the receive are issued as two calls rather than as
     * muzix_ipc_sendrec(), which would hide the receive half's outcome.  That is
     * what the fire-and-forget above was: sendrec() returns mini_send()'s wake
     * code when the send half woke someone, so a caller that also went on to
     * block in mini_rec() was told only about the wake.  The routed syscall is
     * exactly that case - the server it woke is blocked waiting for the next
     * request - and the caller ran straight on, overwriting the request it had
     * just delivered with the next one.  Split, both halves are visible: the send
     * half yields the wake number to ready the server with, and the receive half
     * yields the block this case has to park on.
     *
     * After the park this function returns 0, which the trap turns into the
     * syscall result for a resuming process (muzix_proc_table_park records
     * resume_ret as 0 and proc_context.c publishes it).  The real answer is
     * copied into the caller's message when it resumes, so 0 is a placeholder
     * and not the answer - which is right, because at this point there is no
     * answer yet.
     */
    if (current_slot >= 0 && current_slot < MUZIX_PROC_TABLE_MAX) {
        g_route_msg[current_slot] = msg;
        /* Set the source to the caller's IPC slot so the server's receive
         * sets ipc_client = m_source = the real client, not 0.  Without this,
         * SYS_IPC_SRV_COPY_FROM_CLIENT in handle_chdir gets ipc_client = 0
         * and refuses the copy. */
        g_route_msg[current_slot].m_source = (int16_t)ipc_slot;
        result = muzix_ipc_send(&g_ipc, ipc_slot, server_slot, &g_route_msg[current_slot]);
    } else {
        result = -1;
    }

    /*
     * result >= 10 is mini_send()'s wake code: the process number of the server
     * it unblocked, plus 10.  The number has to reach the kernel process table
     * as well as the IPC table, because mini_send() only readies the server in
     * IPC and pick_next() reads the kernel's queues and nothing else.  This is
     * the decode that was missing here and present in the SENDREC path above.
     *
     * The delivered request is also handed over here, while this stack is live.
     * The server is parked with p_getfrom == ANY, so the request this call sent
     * is the one it will read, and after this point no frame exists that could
     * put it in the server's userspace buffer.  Note the order: the copy first,
     * the ready second, so a resume never observes a slot whose message has
     * been neither delivered nor collected.
     */
    if (result >= 10) {
        int woken_slot = result - 10;
        if (woken_slot >= 0 && woken_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_syscalls_deliver_parked(ctx, woken_slot);
            muzix_proc_table_ready(table, woken_slot);
        }
    }

    /*
     * Now block for the reply, waiting on this server and not on ANY: the
     * caller's routed syscall is a request-and-reply, and a message from
     * anywhere else would have to be answered by someone that is not going to
     * be waiting.
     *
     * The parked receive records no message buffer - there is none, this
     * syscall's answer is a scalar - and it has to say so, or the delivery
     * would write the reply into whatever address the process left there from
     * an earlier sendrec().  0 is not a valid user address: window 0 is the
     * kernel page, so no user buffer can ever live at 0.
     */
    muzix_syscalls_note_recv_buffer(ipc_slot, 0);
    result = muzix_ipc_receive(&g_ipc, ipc_slot, server_slot, &msg);

    if (result == 1) {
        /*
         * mini_rec() blocked: the caller is now parked in the IPC table with
         * RECEIVING and p_getfrom == server_slot, waiting for the reply.
         *
         * It also has to be parked in the KERNEL process table, or the scheduler
         * will dispatch it again immediately - it has not exited and its resume
         * point is valid, so pick_next() hands it straight back and the
         * park/wake cycle spins.  The trace for this reads as `SYS: num=0D
         * slot=01` repeated many times per second, which is what it looks like
         * when the caller is being re-dispatched instead of waiting.
         *
         * `muzix_current_slot(ctx)` is written out in full rather than as the
         * local `current_slot`, even though the local holds the same value, as
         * set at the top of this function.  tools/check_park_site.rb refuses
         * the local spelling, and it is right to: the whole danger of a park() is
         * that the slot it names must be the one that made THIS system call, and
         * an expression that merely happens to equal that is what the gate
         * catches when the provenance drifts.  The value is re-read so there is
         * no way for the two to diverge.
         */
        if (current_slot >= 0 && current_slot < MUZIX_PROC_TABLE_MAX) {
            muzix_proc_table_charge(&table->slots[current_slot], 1);
        }
        muzix_proc_table_park(table, muzix_current_slot(ctx), 0xFFFF);

        /*
         * Request the switch through the publisher.  muzix_kernel_switchout()
         * fills the window-0 cells the trap resumes from and raises
         * muzix_switch_pending last, once everything the jump needs is
         * published.  Raising the flag here directly would leave those cells
         * holding the capture for THIS syscall, and the parked process would be
         * resumed into its own trap return rather than into its reply.
         */
        muzix_kernel_switchout(ctx->kernel->loop);

        /*
         * Same abandoned frame as the receive-only park above, and the copy-out
         * is written at the same place: the copy-out is where the parked
         * caller's own frame can do it, and it currently has none.
         * the reply to this process's recorded userspace buffer when the server
         * sends it, and stamps resume_ret so this syscall returns the reply's
         * own value rather than the 0 the park recorded.
         */
        return 0;
    }

    if (result != 0) {
        return result;
    }

    /*
     * The message was delivered without blocking: the server was already
     * waiting on exactly this caller.  The reply has not been written yet, but
     * the caller also has nothing to wait for, so returning is correct - the
     * message is queued and the answer will come back as the reply.
     */
    return msg.m1_i1;
}

int32_t muzix_handle_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3)
{
    muzix_proc_slot_t *process;
    int32_t result;
    
    if (!ctx) {
        return -1;
    }
    
    process = muzix_current_process(ctx);
    if (process) {
        ctx->pid = process->pid;
        ctx->uid = process->uid;
        ctx->gid = process->gid;
        ctx->euid = process->euid;
        ctx->egid = process->egid;
        ctx->umask = process->umask;
        ctx->ppid = process->parent_pid;
    }
    
    muzix_proc_set_accounted(process);
    muzix_proc_table_charge(process, 0);
    
    
    if (syscall_num == MUZIX_USER_SYS_PARK) {
        muzix_kernel_proc_table_t *t;
        
        if (!process || !ctx->kernel || !ctx->kernel->loop) {
            return -1;
        }
        t = &ctx->kernel->loop->system.startup.proc_table;
        if (arg1 <= 0) {
            arg1 = 1;
        }
        muzix_proc_table_charge(process, 1);
        muzix_proc_table_park(t, muzix_current_slot(ctx), (uint16_t)arg1);
        
        muzix_kernel_switchout(ctx->kernel->loop);
        return 0;
    }
    
    result = muzix_handle_userspace_syscall_ipc(ctx, syscall_num, arg1, arg2, arg3);
    
    muzix_proc_table_charge(process, 1);
    
    return result;
}