/* * * Process table: fixed-size slot array (MUZIX_PROC_TABLE_MAX=8),
 * three priority ready queues, park/wake, resume points, and
 * per-process accounting.
 * * Process identity: there is ONE process number in Muzix, and
 * it is the number the scheduler calls a slot. The array
 * offset `NR_TASKS +` lives in exactly one function,
 * muzix_ipc_proc(), and is not a property of the process.
 * See include/muzix_ipc.h for the full account.
 */

#ifndef MUZIX_PROC_TABLE_H
#define MUZIX_PROC_TABLE_H

#include "ztypes.h"

#include "../lib/proc_info.h"
#include "../mm/mm_service.h"
#include "../mm/mproc_model.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/context_switch.h"

/* Maximum number of process slots. */
#define MUZIX_PROC_TABLE_MAX 8
/* Ready queue: task queue (highest priority). */
#define MUZIX_PROC_TASK_Q 0
/* Ready queue: server queue. */
#define MUZIX_PROC_SERVER_Q 1
/* Ready queue: user queue (lowest priority). */
#define MUZIX_PROC_USER_Q 2
/* Number of ready queues. */
#define MUZIX_PROC_NQ 3
/* Slot is free. */
#define MUZIX_PROC_FREE 001u
/* Slot is parked: not runnable until its deadline passes. */
#define MUZIX_PROC_WAIT_TICK 004u
/* Slot has no bank map. */
#define MUZIX_PROC_NO_MAP 002u

/* * Number of pages a slot owns exclusively and must return to
 * the allocator.
 * * The bank map cannot serve as the ownership record: pages[0] and pages[3] are
 * always the kernel's own pages (0x20/0x23) and must never be released, while
 * fork copies map and mproc verbatim, so a child briefly holds the parent's
 * page IDs. Deriving what to free from the map would hand the parent's pages
 * back while the parent is still running.
 * * A forked child may own its private stack page before exec; exec_loader must
 * release that replaced page before publishing the new image's text and stack.
 * A child that could not get a private stack owns nothing until exec succeeds.
 * The kernel slot and every slot registered directly own nothing either.
 */
#define MUZIX_PROC_OWNED_PAGES 2

/* * Process table slot.
 * * The first forty bytes are struct proc_info, field for field and in the same
 * order. They are here for that reason rather than by preference: SYS_PROCTAB
 * fills a userspace struct by handing the front of the slot to the copy-out
 * unchanged, and the kernel does not have the spare bytes for filling thirteen
 * fields one at a time. Keeping them contiguous at offset 0 is what makes that
 * a forty-byte transfer instead of a loop.
 * * Do not insert anything above this block. The layout check below turns that
 * into a build failure rather than into a process table that reads back with one
 * column shifted.
 */
typedef struct muzix_proc_slot {
 uint8_t pid; /*< Process id */
 uint8_t p_flags; /*< Process flags */
 uint8_t active; /*< 0 = free, 1 = live */
 uint8_t parent_pid; /*< Parent process id */
 uint32_t user_time; /*< User CPU time in ticks */
 uint32_t sys_time; /*< System CPU time in ticks */
 uint32_t child_user_time; /*< Cumulative user time of children */
 uint32_t child_sys_time; /*< Cumulative system time of children */
 uint8_t exit_status; /*< Exit status if exited */
 uint8_t exited; /*< 1 if the process has exited (zombie) */
 uint8_t ready_q; /*< Ready queue: TASK/SERVER/USER */
 uint8_t owned_count; /*< Number of owned pages */
 char name[MUZIX_PROC_NAME_SIZE]; /*< Command name, NUL-terminated */
 uint16_t pending_signals; /*< Pending signal mask */
 uint16_t resume_pc; /*< Resume program counter */
 uint16_t resume_sp; /*< Resume stack pointer */
 uint16_t resume_ret; /*< Syscall return value in DE */
 uint8_t resume_valid; /*< 1 if a resume point is recorded */
 uint16_t stack_ptr; /*< Stack pointer */
 uint16_t entry_point; /*< Entry point address */
 uint16_t argc; /*< Argument count */
 uint16_t argv_ptr; /*< Argument vector pointer */
 uint16_t uid; /*< Real user id */
 uint16_t gid; /*< Real group id */
 uint16_t euid; /*< Effective user id */
 uint16_t egid; /*< Effective group id */
 uint16_t umask; /*< File creation mask */
 int next_ready; /*< Next ready queue link */
 uint8_t owned_pages[MUZIX_PROC_OWNED_PAGES]; /*< Owned page IDs */
 muzix_mproc_t mproc; /*< Memory map model */
 process_map_t map; /*< Current bank map */
 process_map_t saved_context;/*< Saved context map */
 /* * The tick this slot was last charged for, so the next charge knows how
 * long the interval was. Per slot rather than one global, because the
 * kernel switches processes inside the intervals: a single stamp would
 * charge the time a process spent running to whichever process happened to
 * be current when the charge was finally made.
 * * It is the END of the struct on purpose. Everything above `name` is
 * struct proc_info handed to userspace verbatim by SYS_PROCTAB, and the
 * offset checks below fail the build if anything is inserted into that
 * span - so a field that has nothing to do with the published record
 * belongs past it.
 * * 16 bits, not 32, for the same reason the tick counter is: the reader
 * muzix_proc_table_charge() is not reentrant against the interrupt, and
 * the low-byte-first / high-byte-last ordering the handler uses makes a
 * torn read at most one tick stale rather than arbitrary. It wraps every
 * 546 seconds, and the charge subtracts rather than compares, so the wrap
 * is a non-event.
 */
uint16_t tick_stamp; /*< 120 Hz deadline (park/wake) */
uint8_t tick_deadline_valid; /*< 1 = tick_stamp holds a real deadline; 0 = no deadline pending */
/* * The accounting stamp - the other meaning of the stamp, given its own
 * field.
 * * tick_stamp used to carry two jobs: the deadline park() writes and
 * wake_expired() tests, and the accounting mark muzix_proc_table_charge()
 * reads and re-stamps at syscall entry and exit. They are the same field
 * and different owners, and the intersection of their lifetimes is the
 * bug: a charge on a PARKED slot re-stamps its deadline to `now`, the
 * deadline expires immediately, and the slot is readied on the next
 * scheduler pass - every pass, for ever.
 * * charge_stamp is the accounting mark and nothing else. tick_stamp stays
 * the deadline, written by park() and tested by wake_expired(), and is now
 * touched by no code path that runs while the slot is parked - which is
 * the whole fix: two fields so the two meanings cannot collide.
 */
 uint16_t charge_stamp; /*< Accounting stamp */
 uint8_t server_id; /*< Server id, if this slot is a server */
 uint8_t ipc_client; /*< Current IPC client (last message sender) */
} muzix_proc_slot_t;

/* * Compile-time layout check: the front of a slot is handed to
 * userspace as struct proc_info, so every shared offset is
 * checked here rather than trusted. A mismatch would not fail
 * at run time - the copy would succeed and the columns would
 * simply be another process's, which reads as real data.
 */
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

/* * Kernel process table: slot array, ready queues, kernel map.
 */
typedef struct {
 muzix_proc_slot_t slots[MUZIX_PROC_TABLE_MAX];
 int current;
 int ready_head[MUZIX_PROC_NQ];
 int ready_tail[MUZIX_PROC_NQ];
 process_map_t kernel_map;
} muzix_kernel_proc_table_t;

/* ------------------------------------------------------------------------- */
/* Process table API */
/* ------------------------------------------------------------------------- */

/* * Initialise the process table.
 * \param[in,out] table: table to initialise
 * kernel_map: the kernel's own bank map
 */
void muzix_proc_table_init(muzix_kernel_proc_table_t *table,
 const process_map_t *kernel_map);

/* * Register a slot with a pid, memory map, and queue.
 * \param[in,out] table: process table
 * slot: slot number to register
 * pid: process id
 * mp: memory model
 * map: bank map
 * q: ready queue (TASK/SERVER/USER)
 * flags: slot flags
 */
void muzix_proc_table_register(muzix_kernel_proc_table_t *table, int slot,
 uint8_t pid, const muzix_mproc_t *mp,
 const process_map_t *map, uint8_t q,
 uint8_t flags);

/* * Mark a slot as ready and enqueue it.
 * \param[in,out] table: process table
 * slot: slot to ready
 */
void muzix_proc_table_ready(muzix_kernel_proc_table_t *table, int slot);

/* * Park a slot for `ticks` on the 120 Hz CTC clock (240 = two
 * seconds) and remove it from the ready queues.
 * * muzix_proc_table_park() overwrites the slot's charge stamp, which keeps the
 * parked interval out of the process's UTIME and STIME;
 * muzix_proc_table_wake_expired() re-stamps at the other end.
 * \param[in,out] table: process table
 * slot: slot to park
 * ticks: tick count to park
 */
void muzix_proc_table_park(muzix_kernel_proc_table_t *table, int slot,
 uint16_t ticks);

/* * Return every parked slot whose deadline has passed to its
 * queue.
 * * Run by the scheduler loop after an interrupt has been serviced - NOT from the
 * interrupt handler, which may address only windows 0 and 3 and where neither
 * this module nor muzix_kernel_loop.c lives.
 * \param[in,out] table: process table
 * Number of slots woken
 */
int muzix_proc_table_wake_expired(muzix_kernel_proc_table_t *table);

/* * Deliver pending signals to parked slots and remove them from the table.
 * * A parked process never makes a syscall, so the syscall-entry signal
 * check never fires for it. Instead of waking it and waiting for dispatch,
 * the signal is delivered now: the slot's pages are released, it is marked
 * inactive, and its parent is woken.  This is safe because a parked process
 * is not running - its stack is not live and the kernel map is installed.
 * \param[in,out] table: process table
 * mm: MM service handle (for releasing pages)
 * Number of slots signalled and killed
 */
int muzix_proc_table_deliver_parked_signals(
    muzix_kernel_proc_table_t *table,
    muzix_mm_service_t *mm);

/* * Return every parked slot to its queue because console input
 * has arrived.
 * * Same caller rule as muzix_proc_table_wake_expired() for the same reason, and
 * it is worth keeping in mind that this is the path a keystroke takes: the UART
 * handler in platform/zeta-v2/tick.s reads the byte and raises a flag, and this
 * is what turns that flag into a running process.
 * * The park deadline is two seconds, which is why this is not redundant with the
 * deadline wake: without it a keystroke waits out the remainder of the park it
 * arrived in. Every parked slot is woken rather than only a console waiter -
 * there is no record of why a slot parked - so a process parked on a pipe wakes,
 * finds nothing, and parks again.
 * \param[in,out] table: process table
 * Number of slots woken
 */
int muzix_proc_table_wake_input(muzix_kernel_proc_table_t *table);

/* * Whether any slot is parked at all, whatever its deadline.
 * * Where muzix_proc_table_should_idle() answers "idle" only when something is
 * parked AND nothing is runnable, this answers the narrower question kernel_main
 * asks. It exists for one state: the last process has exited, so nothing is
 * runnable and nothing is parked.
 * table: process table
 * 1 if any slot is parked, 0 otherwise
 */
int muzix_proc_table_any_parked(muzix_kernel_proc_table_t *table);

/* * Should the machine stop? Returns 1 when it should.
 * * This is the question the scheduler asks when it has nothing to dispatch, and
 * it is asked from muzix_kernel_loop_run() with the kernel map installed -
 * never from an interrupt handler. It wakes expired deadlines as a side
 * effect, so calling it is how parking ever ends.
 * * It returns 0 unless there is parked work AND nothing runnable, on purpose.
 * An empty table with nothing parked is a machine that has not booted yet, or
 * one whose last process has exited, and stopping the processor in either
 * state is a hang.
 * table: process table
 * 1 if the machine should idle, 0 otherwise
 */
int muzix_proc_table_should_idle(muzix_kernel_proc_table_t *table);

/* * Tell the accounting which slot is currently being charged,
 * or 0 for none. Set once per system call by
 * muzix_handle_userspace_syscall().
 * slot: slot to account, or NULL for none
 */
void muzix_proc_set_accounted(muzix_proc_slot_t *slot);

/* * Stop the processor until the next interrupt, without charging
 * the interval to the running process.
 * * The charge is the wall time between the two ends of a system call, so a plain
 * `halt` inside a bounded wait is billed as system time and a machine standing
 * still reports 100% busy. This stamps the charge on both sides of the halt,
 * so only executed time is accumulated. It is a no-op if no system call is in
 * flight, which keeps an early boot halt from touching a slot nothing is charging.
 * * The scheduler's own idle halt is a caller too, and deliberately so. The halt
 * that ends a park is taken with a system call in flight, and without it that
 * interval lands in the parked process's STIME - the processor was stopped, so
 * charging it as work is a machine at rest reporting itself busy.
 */
void muzix_proc_halt_uncounted(void);

/* * Fork: create a child slot from a parent.
 * \param[in,out] table: process table
 * parent_slot: parent slot number
 * child_slot: child slot number
 * child_pid: child process id
 * 0 on success, -1 on failure
 */
int muzix_proc_table_fork(muzix_kernel_proc_table_t *table, int parent_slot,
 int child_slot, uint8_t child_pid);

/* * Exec: set up a slot for a new program image.
 * \param[in,out] table: process table
 * slot: slot to exec into
 * stack_ptr: stack pointer for the new image
 * 0 on success, -1 on failure
 */
int muzix_proc_table_exec(muzix_kernel_proc_table_t *table, int slot,
 uint16_t stack_ptr);

/* * Record the command name of an exec into a slot, from the
 * path the exec was given.
 * * Called by the loader rather than by the exec syscall, so that the shell the
 * kernel starts at boot is named as well as the programs the shell starts: the
 * boot path never reaches the syscall handler, and a name filled only there
 * left the shell - the one process always on screen - showing as unnamed.
 * \param[in,out] table: process table
 * slot: slot to name
 * path: exec path, truncated to MUZIX_PROC_NAME_SIZE-1
 */
void muzix_proc_set_name(muzix_kernel_proc_table_t *table, int slot,
 const char *path);

/* * Exit a slot.
 * \param[in,out] table: process table
 * slot: slot to exit
 * 0 on success, -1 on failure
 */
int muzix_proc_table_exit(muzix_kernel_proc_table_t *table, int slot);

/* * Release the pages a slot owns back to the allocator, then
 * clear the record.
 * * The proc table holds no MM handle, so the caller supplies one.
 * \param[in,out] table: process table
 * slot: slot whose pages to release
 * mm: MM service handle
 */
void muzix_proc_table_release_pages(muzix_kernel_proc_table_t *table, int slot,
 muzix_mm_service_t *mm);

/* * Record the pages a slot owns, replacing any previous record.
 * * Call this only once the slot's new map is live, so an exec that fails later
 * cannot release pages the slot was not actually running on.
 * \param[in,out] table: process table
 * slot: slot to record pages for
 * pages: array of page IDs
 * count: number of pages
 */
void muzix_proc_table_set_owned(muzix_kernel_proc_table_t *table, int slot,
 const uint8_t *pages, uint8_t count);

/* * Record where a slot should be resumed: the instruction after
 * the syscall it was inside, the stack it was using, and the
 * value that syscall returns.
 * \param[in,out] table: process table
 * slot: slot to record resume point for
 * pc: resume program counter
 * sp: resume stack pointer
 * retval: syscall return value
 */
void muzix_proc_table_set_resume(muzix_kernel_proc_table_t *table, int slot,
 uint16_t pc, uint16_t sp, uint16_t retval);

/* * Fetch a slot's resume point.
 * table: process table
 * slot: slot to query
 * pc: resume program counter
 * sp: resume stack pointer
 * retval: syscall return value
 * 0 if a resume point was recorded, non-zero if never set
 */
int muzix_proc_table_get_resume(const muzix_kernel_proc_table_t *table, int slot,
 uint16_t *pc, uint16_t *sp,
 uint16_t *retval);

/* * Record an exit status for a slot.
 * \param[in,out] table: process table
 * slot: slot that exited
 * parent_pid: parent's process id
 * status: exit status
 * 0 on success, -1 on failure
 */
int muzix_proc_table_exit_status(muzix_kernel_proc_table_t *table, int slot,
 uint8_t parent_pid, uint8_t status);

/* * Queue a signal for a slot.
 * \param[in,out] table: process table
 * slot: slot to signal
 * signal: signal number
 * 0 on success, -1 on failure
 */
int muzix_proc_table_queue_signal(muzix_kernel_proc_table_t *table, int slot,
 uint8_t signal);

/* * Deliver pending signals for a slot.
 * \param[in,out] table: process table
 * slot: slot to deliver signals for
 * mm: MM service handle
 * 0 on success, -1 on failure
 */
int muzix_proc_table_deliver_pending(muzix_kernel_proc_table_t *table, int slot,
 muzix_mm_service_t *mm);

/* * Pick the next process to run from the ready queues.
 * table: process table
 * Slot number of the next process, or -1 if none
 */
int muzix_proc_table_pick_next(muzix_kernel_proc_table_t *table);

/* * Yield: re-queue the current process and pick the next.
 * \param[in,out] table: process table
 * Slot number of the next process, or -1 if none
 */
int muzix_proc_table_yield(muzix_kernel_proc_table_t *table);

/* * Switch to the kernel map.
 * \param[in,out] table: process table
 */
void muzix_proc_table_switch_to_kernel(muzix_kernel_proc_table_t *table);

/* * Return the current process's pid.
 * table: process table
 * Current process id
 */
int muzix_proc_table_current_pid(const muzix_kernel_proc_table_t *table);

/* * Charge a slot for the time since it was last charged, and
 * re-stamp it.
 * * kernel_time says which of the two counters the interval belonged to. The two
 * callers bracket the syscall, so between them every tick lands in exactly one:
 * * - at syscall ENTRY, with kernel_time == 0, because the interval that just
 * ended was the process's own: the kernel handed it the CPU and it ran
 * until it trapped.
 * - at syscall EXIT, with kernel_time == 1, because the interval that just
 * ended was the kernel's, spent on this process's behalf.
 * * The stamp is per slot (see muzix_proc_slot_t::tick_stamp) so an interval that
 * spans a context switch is charged to the process that was actually running
 * during it rather than to whoever is current when the charge is finally made.
 * A pointer, not a table and an index, because every caller already has the
 * slot and indexing a table of this size costs a multiply this kernel does not
 * have the bytes for.
 * * Pass the process that made the syscall, taken once on the way in - so a fork
 * or an exec that changes which process is current charges the syscall to the
 * process that asked for it, which is the one that caused the work.
 * \param[in,out] process: process slot to charge
 * kernel_time: 0 = user interval, 1 = kernel interval
 */
void muzix_proc_table_charge(muzix_proc_slot_t *process, uint8_t kernel_time);

#endif /* MUZIX_PROC_TABLE_H */
