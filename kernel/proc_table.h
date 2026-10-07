#ifndef MUZIX_PROC_TABLE_H
#define MUZIX_PROC_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "../lib/proc_info.h"
#include "../mm/mm_service.h"
#include "../mm/mproc_model.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/context_switch.h"

#define MUZIX_PROC_TABLE_MAX 4
#define MUZIX_PROC_TASK_Q 0
#define MUZIX_PROC_SERVER_Q 1
#define MUZIX_PROC_USER_Q 2
#define MUZIX_PROC_NQ 3
#define MUZIX_PROC_FREE 001u

/* Parked: the slot exists, owns its map, and is not free, but it is not runnable
 * until muzix_proc_table_wake_expired() finds its deadline passed.  A parked slot
 * is not in any ready queue, so pick_next() needs to know nothing about it. */
#define MUZIX_PROC_WAIT_TICK 004u
#define MUZIX_PROC_NO_MAP 002u

/* Pages a slot owns exclusively and must return to the allocator.
 *
 * The bank map cannot serve as the ownership record: pages[0] and pages[3] are
 * always the kernel's own pages (0x20/0x23) and must never be released, while
 * fork copies map and mproc verbatim, so a child briefly holds the parent's
 * page IDs. Deriving what to free from the map would hand the parent's pages
 * back while the parent is still running.
 *
 * A forked child may own its private stack page before exec; exec_loader must
 * release that replaced page before publishing the new image's text and stack.
 * A child that could not get a private stack owns nothing until exec succeeds.
 * The kernel slot and every slot registered directly own nothing either. */
#define MUZIX_PROC_OWNED_PAGES 2

typedef struct {
    /* The first forty bytes are struct proc_info, field for field and in
     * the same order, and they are here for that reason rather than by
     * preference: SYS_PROCTAB fills a userspace struct by handing the front of
     * the slot to the copy-out unchanged, and the kernel does not have the
     * spare bytes for filling thirteen fields one at a time.  Keeping them
     * contiguous at offset 0 is what makes that a forty-byte transfer instead
     * of a loop.
     *
     * The block covers four bytes past the original twenty because a syscall
     * that reported the runnable count directly did not fit: measured at 315
     * bytes of _CODE for the plain version and 191 for one stripped of every
     * bounds check, against 46 bytes of room below _DATA.  Widening the copied
     * window instead costs nothing at all - the size is a compile-time constant
     * in an argument that was already there - and ready_q comes out of it.  The
     * command name comes out of the same mechanism sixteen bytes further on, for
     * the same reason and at the same price of zero.
     *
     * Do not insert anything above this block.  The check below turns that into
     * a build failure rather than into a process table that reads back with one
     * column shifted. */
    uint8_t pid;
    uint8_t p_flags;
    uint8_t active;
    uint8_t parent_pid;
    uint32_t user_time;
    uint32_t sys_time;
    uint32_t child_user_time;
    uint32_t child_sys_time;
    uint8_t exit_status;
    uint8_t exited;
    uint8_t ready_q;
    uint8_t owned_count;
    /* The command name, and the reason the copied window is forty bytes rather
     * than twenty-four.  It is here rather than anywhere else in the slot
     * because here is inside struct proc_info's span: SYS_PROCTAB hands the
     * front of the slot to the copy-out as a literal, so a field that sits in
     * that span reaches userspace for the price of a larger compile-time size
     * and nothing else.  Putting it at the end of the slot would have needed a
     * second transfer, and a second transfer is code.
     *
     * It is not at the *front* of the slot either, for the reason the block
     * above states: the offsets below are checked at compile time, and this
     * field is checked with them.
     *
     * Filled by the exec handler from the path it was handed, truncated to
     * MUZIX_PROC_NAME_SIZE-1 characters, so it is the leading characters of a
     * path rather than its basename: "ls" arrives as "ls", "/bin/ls" arrives
     * as "/bin/ls" and the program showing the column decides what to make of
     * a leading slash.  That split is deliberate - see apps/top.c - because
     * stripping the directory here costs the kernel a backward scan it does not
     * have the bytes for. */
    char name[MUZIX_PROC_NAME_SIZE];
    uint16_t pending_signals;
    /* Where to resume this process, if it is not running.
     *
     * The bank map cannot answer this. A slot knows which pages it runs on, but
     * not *where in them* to continue, and after a switch the process's text
     * page is not the one mapped into window 2, so the program counter is only
     * meaningful as a value carried across the switch - never as a pointer that
     * is dereferenced later. `resume_ret` is the value the syscall the process
     * was inside is going to return in DE: zero for the child of a fork, which
     * is the whole point of it, and the child pid for the parent when it comes
     * back. */
    uint16_t resume_pc;
    uint16_t resume_sp;
    uint16_t resume_ret;
    uint8_t resume_valid;
    uint16_t stack_ptr;
    uint16_t entry_point;
    uint16_t argc;
    uint16_t argv_ptr;
    uint16_t uid;
    uint16_t gid;
    uint16_t euid;
    uint16_t egid;
    uint16_t umask;
    int next_ready;
    uint8_t owned_pages[MUZIX_PROC_OWNED_PAGES];
    muzix_mproc_t mproc;
    process_map_t map;
    process_map_t saved_context;
    /* The tick this slot was last charged for, so the next charge knows how
     * long the interval was.  Per slot rather than one global, because the
     * kernel switches processes inside the intervals: a single stamp would
     * charge the time a process spent running to whichever process happened to
     * be current when the charge was finally made.
     *
     * It is the END of the struct on purpose.  Everything above `name` is
     * struct proc_info handed to userspace verbatim by SYS_PROCTAB, and the
     * offset checks above fail the build if anything is inserted into that
     * span - so a field that has nothing to do with the published record
     * belongs past it.
     *
     * 16 bits, not 32, for the same reason the tick counter is: the reader
     * muzix_proc_table_charge() is not reentrant against the interrupt, and
     * the low-byte-first / high-byte-last ordering the handler uses makes a
     * torn read at most one tick stale rather than arbitrary.  It wraps every
     * 546 seconds, and the charge subtracts rather than compares, so the wrap
     * is a non-event. */
    uint16_t tick_stamp;
} muzix_proc_slot_t;

/* The front of a slot is handed to userspace as a struct proc_info, so every
 * offset the two share is checked here rather than trusted.  A mismatch would
 * not fail anything at run time: the copy would succeed and the columns would
 * simply be another process's, which is the kind of wrong that reads as real
 * data.  `parent_pid` is the public `ppid` - the kernel's name says what it
 * points at, not that it differs. */
typedef char muzix_proc_info_layout_check[
    (sizeof(struct proc_info) == MUZIX_PROC_INFO_SIZE &&
     offsetof(muzix_proc_slot_t, pid) == offsetof(struct proc_info, pid) &&
     offsetof(muzix_proc_slot_t, p_flags) == offsetof(struct proc_info, flags) &&
     offsetof(muzix_proc_slot_t, active) == offsetof(struct proc_info, active) &&
     offsetof(muzix_proc_slot_t, parent_pid) == offsetof(struct proc_info, ppid) &&
     offsetof(muzix_proc_slot_t, user_time) == offsetof(struct proc_info, user_time) &&
     offsetof(muzix_proc_slot_t, sys_time) == offsetof(struct proc_info, sys_time) &&
     offsetof(muzix_proc_slot_t, child_user_time) ==
         offsetof(struct proc_info, child_user_time) &&
     offsetof(muzix_proc_slot_t, child_sys_time) ==
          offsetof(struct proc_info, child_sys_time) &&
     offsetof(muzix_proc_slot_t, exit_status) ==
         offsetof(struct proc_info, exit_status) &&
     offsetof(muzix_proc_slot_t, exited) == offsetof(struct proc_info, exited) &&
     offsetof(muzix_proc_slot_t, ready_q) == offsetof(struct proc_info, ready_q) &&
     offsetof(muzix_proc_slot_t, owned_count) ==
          offsetof(struct proc_info, owned_count) &&
     offsetof(muzix_proc_slot_t, name) == offsetof(struct proc_info, name) &&
     MUZIX_PROC_TASK_Q == MUZIX_PROC_Q_TASK &&
     MUZIX_PROC_SERVER_Q == MUZIX_PROC_Q_SERVER &&
     MUZIX_PROC_USER_Q == MUZIX_PROC_Q_USER &&
     MUZIX_PROC_INFO_SIZE <= MUZIX_ZETA_COPY_STAGING &&
     MUZIX_PROC_INFO_SLOTS == MUZIX_PROC_TABLE_MAX)
        ? 1
        : -1];

typedef struct {
    muzix_proc_slot_t slots[MUZIX_PROC_TABLE_MAX];
    int current;
    int ready_head[MUZIX_PROC_NQ];
    int ready_tail[MUZIX_PROC_NQ];
    process_map_t kernel_map;
} muzix_kernel_proc_table_t;

void muzix_proc_table_init(muzix_kernel_proc_table_t *table, const process_map_t *kernel_map);
void muzix_proc_table_register(muzix_kernel_proc_table_t *table,
                              int slot,
                              uint8_t pid,
                              const muzix_mproc_t *mp,
                              const process_map_t *map,
                              uint8_t q,
                              uint8_t flags);
void muzix_proc_table_ready(muzix_kernel_proc_table_t *table, int slot);

/* Park a slot for `ticks` on the 120 Hz CTC clock - 240 is two seconds - and take
 * it out of the ready queues.  muzix_proc_table_park() overwrites the slot's
 * charge stamp, which is what keeps the parked interval out of the process's
 * UTIME and STIME; muzix_proc_table_wake_expired() re-stamps at the other end. */
void muzix_proc_table_park(muzix_kernel_proc_table_t *table, int slot,
                           uint16_t ticks);

/* Return every parked slot whose deadline has passed to its queue.  Run by the
 * scheduler loop after an interrupt has been serviced - NOT from the interrupt
 * handler, which may address only windows 0 and 3 and where neither this module
 * nor muzix_kernel_loop.c lives.  Returns how many were woken. */
int muzix_proc_table_wake_expired(muzix_kernel_proc_table_t *table);

/* Return every parked slot to its queue, because console input has arrived.
 *
 * The same rule as muzix_proc_table_wake_expired() about who may call it - the
 * scheduler loop after an interrupt, never the handler - for the same reason, and
 * it is worth keeping in mind that this is the path a keystroke takes: the UART
 * handler in platform/zeta-v2/tick.s reads the byte and raises a flag, and this is
 * what turns that flag into a running process.
 *
 * The park deadline is two seconds, which is why this is not redundant with the
 * deadline wake: without it a keystroke waits out the remainder of the park it
 * arrived in.  Every parked slot is woken rather than only a console waiter -
 * there is no record of why a slot parked - so a process parked on a pipe wakes,
 * finds nothing, and parks again.  Returns how many were woken. */
int muzix_proc_table_wake_input(muzix_kernel_proc_table_t *table);

/* Whether any slot is parked at all, whatever its deadline.
 *
 * Where muzix_proc_table_should_idle() answers "idle" only when something is
 * parked AND nothing is runnable, this answers the narrower question kernel_main
 * asks.  It exists for one state: the last process has exited, so nothing is
 * runnable and nothing is parked.  should_idle() calls that "there is work", and
 * a loop that trusts it spins at full speed for ever - which is what kept the load
 * figure at 1.00 at an idle prompt even once parking worked. */
int muzix_proc_table_any_parked(muzix_kernel_proc_table_t *table);

/* Should the machine stop?  Returns 1 when it should.
 *
 * This is the question the scheduler asks when it has nothing to dispatch, and it
 * is asked from muzix_kernel_loop_run() with the kernel map installed - never from
 * an interrupt handler.  It wakes expired deadlines as a side effect, so calling it
 * is how parking ever ends.
 *
 * It returns 0 unless there is parked work AND nothing runnable, on purpose.  An
 * empty table with nothing parked is a machine that has not booted yet, or one
 * whose last process has exited, and stopping the processor in either state is a
 * hang: there would be no deadline coming to wake it and nothing to run when one
 * did. */
int muzix_proc_table_should_idle(muzix_kernel_proc_table_t *table);

/* Tell the accounting which slot is currently being charged, or 0 for none.  Set
 * once per system call by muzix_handle_userspace_syscall(). */
void muzix_proc_set_accounted(muzix_proc_slot_t *slot);

/* Stop the processor until the next interrupt, without charging the interval to
 * the running process.
 *
 * The charge is the wall time between the two ends of a system call, so a plain
 * `halt` inside a bounded wait is billed as system time and a machine standing
 * still reports 100% busy.  This stamps the charge on both sides of the halt, so
 * only executed time is accumulated.  It is a no-op if no system call is in
 * flight, which keeps an early-boot halt from touching a slot nothing is charging.
 *
 * For a caller that is NOT inside a system call - the scheduler's own idle step -
 * there is nothing to charge and z80_halt() is the right call directly. */
void muzix_proc_halt_uncounted(void);
int muzix_proc_table_fork(muzix_kernel_proc_table_t *table,
                          int parent_slot,
                          int child_slot,
                          uint8_t child_pid);
int muzix_proc_table_exec(muzix_kernel_proc_table_t *table,
                          int slot,
                          uint16_t stack_ptr);
/* Record the command name of an exec into a slot, from the path the exec was
 * given.  Called by the loader rather than by the exec syscall, so that the
 * shell the kernel starts at boot is named as well as the programs the shell
 * starts: the boot path never reaches the syscall handler, and a name filled
 * only there left the shell - the one process always on screen - showing as
 * unnamed. */
void muzix_proc_set_name(muzix_kernel_proc_table_t *table,
                         int slot,
                         const char *path);
int muzix_proc_table_exit(muzix_kernel_proc_table_t *table, int slot);
/* Release the pages a slot owns back to the allocator, then clear the record.
 * The proc table holds no MM handle, so the caller supplies one. */
void muzix_proc_table_release_pages(muzix_kernel_proc_table_t *table,
                                    int slot,
                                    muzix_mm_service_t *mm);
/* Record the pages a slot owns, replacing any previous record. Call this only
 * once the slot's new map is live, so an exec that fails later cannot release
 * pages the slot was not actually running on. */
void muzix_proc_table_set_owned(muzix_kernel_proc_table_t *table,
                                int slot,
                                const uint8_t *pages,
                                uint8_t count);

/* Record where a slot should be resumed: the instruction after the syscall it
 * was inside, the stack it was using, and the value that syscall returns. */
void muzix_proc_table_set_resume(muzix_kernel_proc_table_t *table,
                                 int slot,
                                 uint16_t pc,
                                 uint16_t sp,
                                 uint16_t retval);
/* Fetch a slot's resume point. Returns 0 if the slot has never been recorded,
 * which is not an error to ignore: resuming it would jump to a stale zero. */
int muzix_proc_table_get_resume(const muzix_kernel_proc_table_t *table,
                                int slot,
                                uint16_t *pc,
                                uint16_t *sp,
                                uint16_t *retval);
int muzix_proc_table_exit_status(muzix_kernel_proc_table_t *table,
                                 int slot,
                                 uint8_t parent_pid,
                                 uint8_t status);
int muzix_proc_table_queue_signal(muzix_kernel_proc_table_t *table,
                                  int slot,
                                  uint8_t signal);
int muzix_proc_table_deliver_pending(muzix_kernel_proc_table_t *table,
                                     int slot,
                                     muzix_mm_service_t *mm);
int muzix_proc_table_pick_next(muzix_kernel_proc_table_t *table);
int muzix_proc_table_yield(muzix_kernel_proc_table_t *table);
void muzix_proc_table_switch_to_kernel(muzix_kernel_proc_table_t *table);
int muzix_proc_table_current_pid(const muzix_kernel_proc_table_t *table);

/* Charge a slot for the time since it was last charged, and re-stamp it.
 *
 * `kernel_time` says which of the two counters the interval belonged to.  The
 * two callers bracket the syscall, so between them every tick lands in exactly
 * one:
 *
 *   - at syscall ENTRY, with kernel_time == 0, because the interval that just
 *     ended was the process's own: the kernel handed it the CPU and it ran
 *     until it trapped.
 *   - at syscall EXIT, with kernel_time == 1, because the interval that just
 *     ended was the kernel's, spent on this process's behalf.
 *
 * The stamp is per slot (see muzix_proc_slot_t::tick_stamp) so an interval
 * that spans a context switch is charged to the process that was actually
 * running during it rather than to whoever is current when the charge is
 * finally made.  A pointer, not a table and an index, because every caller
 * already has the slot and indexing a 4-slot table of this size costs a
 * multiply this kernel does not have the bytes for.
 *
 * Pass the process that made the syscall, taken once on the way in - so a
 * fork or an exec that changes which process is current charges the syscall to
 * the process that asked for it, which is the one that caused the work. */
void muzix_proc_table_charge(muzix_proc_slot_t *process, uint8_t kernel_time);

#endif
