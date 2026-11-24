/* * * Process table implementation: fixed-size slot array, three
 * priority ready queues, park/wake, resume points, and
 * per-process accounting.
 */

#include "proc_table.h"

#include "libc.h"

#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/syscall_entry.h"
#include "../platform/zeta-v2/trace.h"

/* * The resume point the syscall trap captured on the way in,
 * in the pair the context switch actually consumes. Defined
 * in kernel/kernel_main.c and read by kernel/proc_context.c.
 */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;

/* * Enqueue a slot onto ready queue q.
 * \param[in,out] table: process table
 * slot: slot to enqueue
 * q: queue index
 */
static void muzix_proc_enqueue(muzix_kernel_proc_table_t *table, int slot, int q)
{
 int tail;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }

 if (table->ready_head[q] == -1) {
 table->ready_head[q] = slot;
 table->ready_tail[q] = slot;
 return;
 }

 tail = table->ready_tail[q];
 if (tail >= 0 && tail < MUZIX_PROC_TABLE_MAX) {
 table->slots[tail].next_ready = slot;
 }
 table->slots[slot].next_ready = -1;
 table->ready_tail[q] = slot;
}

/* * Test whether a slot is on any ready queue.
 * table: process table
 * slot: slot to test
 * 1 if on a queue, 0 otherwise
 */
static int muzix_proc_is_ready(const muzix_kernel_proc_table_t *table, int slot)
{
 int q;
 int current;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return 0;
 }

 for (q = 0; q < MUZIX_PROC_NQ; q++) {
 current = table->ready_head[q];
 while (current >= 0 && current < MUZIX_PROC_TABLE_MAX) {
 if (current == slot) {
 return 1;
 }
 current = table->slots[current].next_ready;
 }
 }
 return 0;
}

/* * Initialise the process table.
 * \param[in,out] table: process table
 * kernel_map: the kernel's own bank map
 */
void muzix_proc_table_init(muzix_kernel_proc_table_t *table,
 const process_map_t *kernel_map)
{
 int i;
 int q;

 if (!table) {
 return;
 }

 memset(table, 0, sizeof(*table));

 /* Only the two fields memset cannot produce: p_flags (not zero) and
 * next_ready (-1). */
 for (i = 0; i < MUZIX_PROC_TABLE_MAX; i++) {
 table->slots[i].p_flags = MUZIX_PROC_FREE;
 table->slots[i].next_ready = -1;
 }
 for (q = 0; q < MUZIX_PROC_NQ; q++) {
 table->ready_head[q] = -1;
 table->ready_tail[q] = -1;
 }
 table->current = -1;
 if (kernel_map) {
 table->kernel_map = *kernel_map;
 }
}

/* * Register a slot with a pid, memory map, and queue.
 * \param[in,out] table: process table
 * slot: slot number to register
 * pid: process id
 * mp: memory model
 * map: bank map
 * q: ready queue (TASK/SERVER/USER)
 * flags: slot flags
 */
void muzix_proc_table_register(muzix_kernel_proc_table_t *table,
 int slot,
 uint8_t pid,
 const muzix_mproc_t *mp,
 const process_map_t *map,
 uint8_t q,
 uint8_t flags)
{
 int current;
 muzix_proc_slot_t *p;

 if (!table || !mp || !map || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX ||
 pid == 0) {
 return;
 }
 for (current = 0; current < MUZIX_PROC_TABLE_MAX; current++) {
 if (current != slot && table->slots[current].active &&
 table->slots[current].pid == pid) {
 return;
 }
 }

 /* One address for the twenty-two fields below: SDCC recomputes the
 * base-plus-index multiplication and every field offset at each mention of
 * table->slots[slot]. */
 p = &table->slots[slot];

 p->pid = pid;
 p->p_flags = flags;
 p->active = 1;
 p->parent_pid = 0;
 p->exit_status = 0;
 p->exited = 0;
 p->ready_q = q;
 p->owned_count = 0;
 p->owned_pages[0] = 0;
 p->owned_pages[1] = 0;
 p->pending_signals = 0;
 p->stack_ptr = 0;
 p->entry_point = 0;
 p->argc = 0;
 p->argv_ptr = 0;
  p->uid = 0;
  p->gid = 0;
  p->euid = 0;
  p->egid = 0;
  p->umask = 0;
  p->user_time = 0;
  p->sys_time = 0;
  p->child_user_time = 0;
  p->child_sys_time = 0;
  /* A slot handed to a new process must not retain a parked deadline from
   * whatever used the slot before.  The table init memsets the whole table,
   * but register() is also the slot-reuse path, so both tick fields are
   * cleared here explicitly rather than assumed zero. */
  p->tick_stamp = 0;
  p->tick_deadline_valid = 0u;
  /* Start the accounting clock now, not at zero. */
  muzix_tick_stamp_store(&p->charge_stamp);
 p->next_ready = -1;
 p->mproc = *mp;
 p->map = *map;
}

/* * Take a slot out of whichever ready queue holds it.
 * * Leaving the queue is what makes parking invisible to pick_next(). Written
 * once, paid once.
 * \param[in,out] table: process table
 * slot: slot to unlink
 */
static void muzix_proc_table_unlink(muzix_kernel_proc_table_t *table, int slot)
{
 int q;
 int previous;
 int current;

 for (q = 0; q < MUZIX_PROC_NQ; q++) {
 previous = -1;
 current = table->ready_head[q];
 while (current >= 0 && current < MUZIX_PROC_TABLE_MAX) {
 if (current == slot) {
 if (previous < 0) {
 table->ready_head[q] = table->slots[current].next_ready;
 } else {
 table->slots[previous].next_ready =
 table->slots[current].next_ready;
 }
 if (table->ready_tail[q] == slot) {
 table->ready_tail[q] = previous;
 }
 break;
 }
 previous = current;
 current = table->slots[current].next_ready;
 }
 }
 table->slots[slot].next_ready = -1;
}

/* The slot whose UTIME and STIME are currently being accumulated, or 0 when
 * no system call is in flight. muzix_handle_userspace_syscall() sets it on
 * the way in; nothing else does. */
static muzix_proc_slot_t *g_accounted;

/* * Tell the accounting which slot is currently being charged.
 * slot: slot to account, or NULL for none
 */
void muzix_proc_set_accounted(muzix_proc_slot_t *slot)
{
 g_accounted = slot;
}

/* * Stop the processor without charging the time it spends
 * stopped.
 * * The stamp is refreshed on BOTH sides of the halt: before, so that the
 * interval already worked is charged by the exit call as normal; after, so
 * that the halt itself is not charged to whatever the process does next.
 */
void muzix_proc_halt_uncounted(void)
{
 z80_idle_halt();
}

/* * Should the machine stop? Returns 1 when it should.
 * table: process table
 * 1 if the machine should idle, 0 otherwise
 */
int muzix_proc_table_should_idle(muzix_kernel_proc_table_t *table)
{
 int slot;
 int parked = 0;

 if (!table) {
 return 0;
 }
 /* Waking first is what makes this safe to call in a loop. */
 if (muzix_proc_table_wake_expired(table) > 0) {
 return 0;
 }
 if (table->current >= 0) {
 return 0;
 }
 for (slot = 0; slot < MUZIX_PROC_NQ; slot++) {
 if (table->ready_head[slot] != -1) {
 return 0;
 }
 }
 /* Nothing runnable. Only stop if something is actually waiting for a
 * wake, which is the difference between idling and hanging. */
 for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
 if (table->slots[slot].active &&
 (table->slots[slot].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
 parked = 1;
 break;
 }
 }
 return parked;
}

/* * Park a slot for `ticks` on the 120 Hz CTC clock
 * (240 = two seconds) and remove it from the ready queues.
 * \param[in,out] table: process table
 * slot: slot to park
 * ticks: tick count to park
 */
void muzix_proc_table_park(muzix_kernel_proc_table_t *table, int slot,
 uint16_t ticks)
{
 muzix_proc_slot_t *p;
 uint16_t sp;
 uint16_t pc;
 uint16_t rsp;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }
 p = &table->slots[slot];
 if (!p->active || (p->p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
 return;
 }

 /* The two words the syscall trap already published, taken as a pair.
 * * This used to read the program counter back out of the stack, which faulted
 * because a process's stack lives in window 1 and by the time any handler
 * runs the trap has already called zeta_kernel_enter(). The trap captured
 * the return address separately, into muzix_resume_pc, for exactly this
 * reason. */
 sp = z80_syscall_user_sp();
 pc = muzix_resume_pc;
 rsp = muzix_resume_sp;

 if (sp != 0u && pc != 0u) {
 muzix_proc_table_set_resume(table, slot, pc, rsp, 0);
 }

  p->p_flags |= MUZIX_PROC_WAIT_TICK;
  if (ticks == 0xFFFFu) {
    /* Infinite wait: no deadline stamp. */
    p->tick_stamp = 0xFFFFu;
    p->tick_deadline_valid = 0u;
  } else {
    p->tick_stamp = (uint16_t)(muzix_tick_now() + ticks);
    p->tick_deadline_valid = 1u;
  }
  muzix_proc_table_unlink(table, slot);

 if (table->current == slot) {
 table->current = -1;
 }
}

/* * Whether any slot is parked at all, whatever its deadline.
 * table: process table
 * 1 if any slot is parked, 0 otherwise
 */
int muzix_proc_table_any_parked(muzix_kernel_proc_table_t *table)
{
 int slot;

 if (!table) {
 return 0;
 }
 for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
 if (table->slots[slot].active &&
 (table->slots[slot].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
 return 1;
 }
 }
 return 0;
}

/* * Put every parked slot whose deadline has passed back in play.
 * \param[in,out] table: process table
 * Number of slots woken
 */
int muzix_proc_table_wake_expired(muzix_kernel_proc_table_t *table)
{
 int slot;
 int woke = 0;

 if (!table) {
 return 0;
 }
 for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
 muzix_proc_slot_t *p = &table->slots[slot];

 if (!p->active || (p->p_flags & MUZIX_PROC_WAIT_TICK) == 0u) {
 continue;
 }
        if (!p->tick_deadline_valid) {
            continue;
        }
        if (!muzix_tick_expired(p->tick_stamp)) {
            continue;
        }
        p->p_flags &= (uint8_t)~MUZIX_PROC_WAIT_TICK;
        p->tick_deadline_valid = 0u;
        muzix_tick_stamp_store(&p->charge_stamp);
 muzix_proc_table_ready(table, slot);
 woke++;
 }
 return woke;
}

/* * Wake every parked slot, because input has arrived.
 * \param[in,out] table: process table
 * Number of slots woken
 */
int muzix_proc_table_wake_input(muzix_kernel_proc_table_t *table)
{
 int slot;
 int woke = 0;

 if (!table) {
 return 0;
 }
 for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
 muzix_proc_slot_t *p = &table->slots[slot];

 if (!p->active || (p->p_flags & MUZIX_PROC_WAIT_TICK) == 0u) {
 continue;
 }
        p->p_flags &= (uint8_t)~MUZIX_PROC_WAIT_TICK;
        p->tick_deadline_valid = 0u;
        muzix_tick_stamp_store(&p->charge_stamp);
        muzix_proc_table_ready(table, slot);
  woke++;
  }
  return woke;
}

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
    muzix_mm_service_t *mm)
{
  int slot;
  int killed = 0;

  if (!table || !mm) {
  return 0;
  }
  for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
  muzix_proc_slot_t *p = &table->slots[slot];

  if (!p->active) {
      continue;
  }
  if (p->pending_signals == 0u) {
      continue;
  }
  muzix_proc_table_deliver_pending(table, slot, mm);
  if (!p->active) {
      killed++;
  }
  }
  return killed;
}

/* * Mark a slot as ready and enqueue it.
 * \param[in,out] table: process table
 * slot: slot to ready
 */
void muzix_proc_table_ready(muzix_kernel_proc_table_t *table, int slot)
{
 int q;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }

 if (!table->slots[slot].active ||
 (table->slots[slot].p_flags & MUZIX_PROC_NO_MAP) != 0) {
 return;
 }

 if (muzix_proc_is_ready(table, slot)) {
 return;
 }

 /* Clear the wait flag: park() refuses to park a slot that already has it,
 * so a readied slot must clear it or it can never park again. */
 table->slots[slot].p_flags &= (uint8_t)~MUZIX_PROC_WAIT_TICK;

 q = table->slots[slot].ready_q;
 if (q < 0 || q >= MUZIX_PROC_NQ) {
 q = MUZIX_PROC_USER_Q;
 }

 if (table->ready_head[q] == -1) {
 table->ready_head[q] = slot;
 table->ready_tail[q] = slot;
 } else {
 muzix_proc_enqueue(table, slot, q);
 }
}

/* * Fork: create a child slot from a parent.
 * \param[in,out] table: process table
 * parent_slot: parent slot number
 * child_slot: child slot number
 * child_pid: child process id
 * 0 on success, -1 on failure
 */
int muzix_proc_table_fork(muzix_kernel_proc_table_t *table,
 int parent_slot,
 int child_slot,
 uint8_t child_pid)
{
 int slot;

 if (!table) {
 return -1;
 }
 if (parent_slot < 0 || parent_slot >= MUZIX_PROC_TABLE_MAX) {
 return -1;
 }
 if (child_slot < 0 || child_slot >= MUZIX_PROC_TABLE_MAX) {
 return -1;
 }
 if (parent_slot == child_slot) {
 return -1;
 }
 if (child_pid == 0) {
 return -1;
 }
 if (!table->slots[parent_slot].active) {
 return -1;
 }
 if (table->slots[child_slot].active) {
 return -1;
 }
 for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
 if (table->slots[slot].active && table->slots[slot].pid == child_pid) {
 return -1;
 }
 }

 table->slots[child_slot] = table->slots[parent_slot];
 table->slots[child_slot].pid = child_pid;
 table->slots[child_slot].ready_q = table->slots[parent_slot].ready_q;
 table->slots[child_slot].mproc.mp_pid = child_pid;
 table->slots[child_slot].p_flags = MUZIX_PROC_NO_MAP;
 table->slots[child_slot].map.pages[0] = 0;
 table->slots[child_slot].map.pages[1] = 0;
 table->slots[child_slot].map.pages[2] = 0;
 table->slots[child_slot].map.pages[3] = 0;
 table->slots[child_slot].parent_pid = table->slots[parent_slot].pid;
 table->slots[child_slot].exit_status = 0;
 table->slots[child_slot].exited = 0;
 table->slots[child_slot].active = 1;
 table->slots[child_slot].pending_signals = 0;
 table->slots[child_slot].user_time = 0;
 table->slots[child_slot].sys_time = 0;
 table->slots[child_slot].child_user_time = 0;
 table->slots[child_slot].child_sys_time = 0;
  /* Not inherited from the parent: the child has been running for no time
   * at all, and it starts counting from here. */
  muzix_tick_stamp_store(&table->slots[child_slot].charge_stamp);
  table->slots[child_slot].tick_deadline_valid = 0u;
  table->slots[child_slot].next_ready = -1;
 table->slots[child_slot].owned_count = 0;
 table->slots[child_slot].owned_pages[0] = 0;
 table->slots[child_slot].owned_pages[1] = 0;
 table->slots[child_slot].name[0] = '\0';
 table->slots[child_slot].resume_valid = 0;
 return 0;
}

/* * Record where a slot should be resumed.
 * \param[in,out] table: process table
 * slot: slot to record resume point for
 * pc: resume program counter
 * sp: resume stack pointer
 * retval: syscall return value
 */
void muzix_proc_table_set_resume(muzix_kernel_proc_table_t *table,
 int slot,
 uint16_t pc,
 uint16_t sp,
 uint16_t retval)
{
 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }
 table->slots[slot].resume_pc = pc;
 table->slots[slot].resume_sp = sp;
 table->slots[slot].resume_ret = retval;
 table->slots[slot].resume_valid = 1;
}

/* * Fetch a slot's resume point.
 * table: process table
 * slot: slot to query
 * pc: resume program counter
 * sp: resume stack pointer
 * retval: syscall return value
 * 1 if a resume point was recorded, 0 otherwise
 */
int muzix_proc_table_get_resume(const muzix_kernel_proc_table_t *table,
 int slot,
 uint16_t *pc,
 uint16_t *sp,
 uint16_t *retval)
{
 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return 0;
 }
 if (!table->slots[slot].resume_valid) {
 return 0;
 }
 if (pc) {
 *pc = table->slots[slot].resume_pc;
 }
 if (sp) {
 *sp = table->slots[slot].resume_sp;
 }
 if (retval) {
 *retval = table->slots[slot].resume_ret;
 }
 return 1;
}

/* * Record the pages a slot owns, replacing any previous record.
 * \param[in,out] table: process table
 * slot: slot to record pages for
 * pages: array of page IDs
 * count: number of pages
 */
void muzix_proc_table_set_owned(muzix_kernel_proc_table_t *table,
 int slot,
 const uint8_t *pages,
 uint8_t count)
{
 int i;

 if (!table || !pages || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }
 if (count > MUZIX_PROC_OWNED_PAGES) {
 count = MUZIX_PROC_OWNED_PAGES;
 }
 for (i = 0; i < MUZIX_PROC_OWNED_PAGES; i++) {
 table->slots[slot].owned_pages[i] =
 (i < count) ? pages[i] : 0;
 }
 table->slots[slot].owned_count = count;
}

/* * Exec: set up a slot for a new program image.
 * \param[in,out] table: process table
 * slot: slot to exec into
 * stack_ptr: stack pointer for the new image
 * 0 on success, -1 on failure
 */
int muzix_proc_table_exec(muzix_kernel_proc_table_t *table,
 int slot,
 uint16_t stack_ptr)
{
 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return -1;
 }

 (void)stack_ptr;
 table->slots[slot].p_flags &= (uint8_t)~MUZIX_PROC_NO_MAP;
  table->slots[slot].stack_ptr = stack_ptr;
  table->slots[slot].tick_deadline_valid = 0u;
  muzix_proc_table_ready(table, slot);
 return 0;
}

/* * Record the command name of an exec into a slot, from the
 * path the exec was given.
 * \param[in,out] table: process table
 * slot: slot to name
 * path: exec path, truncated to MUZIX_PROC_NAME_SIZE-1
 */
void muzix_proc_set_name(muzix_kernel_proc_table_t *table,
 int slot,
 const char *path)
{
 muzix_proc_slot_t *process = &table->slots[slot];
 uint8_t i = 0;

 if (!table || !path || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }

 /* Bounded copy of the path, NUL-terminated, into the one field of the slot
 * that lies inside the window SYS_PROCTAB hands to userspace. */
 while (i < MUZIX_PROC_NAME_SIZE - 1 && path[i] != '\0') {
 process->name[i] = path[i];
 i++;
 }
 process->name[i] = '\0';
}

/* * Release the pages a slot owns back to the allocator, then
 * clear the record.
 * \param[in,out] table: process table
 * slot: slot whose pages to release
 * mm: MM service handle
 */
void muzix_proc_table_release_pages(muzix_kernel_proc_table_t *table,
 int slot,
 muzix_mm_service_t *mm)
{
 uint8_t i;
 uint8_t count;

 if (!table || !mm || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return;
 }

 /* Clear the record first: if a release path ever re-enters, the second pass
 * sees owned_count == 0 instead of releasing the same page twice. */
 count = table->slots[slot].owned_count;
 table->slots[slot].owned_count = 0;
 for (i = 0; i < count && i < MUZIX_PROC_OWNED_PAGES; i++) {
 uint8_t page = table->slots[slot].owned_pages[i];
 table->slots[slot].owned_pages[i] = 0;
 muzix_mm_free_page(mm, page);
 }
}

/* * Exit a slot.
 * \param[in,out] table: process table
 * slot: slot to exit
 * 0 on success, -1 on failure
 */
int
muzix_proc_table_exit(muzix_kernel_proc_table_t *table, int slot)
{
    if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX ||
        !table->slots[slot].active) {
        return -1;
    }

    muzix_proc_table_unlink(table, slot);

    table->slots[slot].active = 0;
    table->slots[slot].p_flags = MUZIX_PROC_FREE;
    table->slots[slot].tick_deadline_valid = 0u;
    if (table->current == slot) {
        table->current = -1;
    }

    /* Wake any parent waiting in sys_wait() for this exiting child. */
    for (int i = 0; i < MUZIX_PROC_TABLE_MAX; i++) {
        if (table->slots[i].active &&
            table->slots[i].pid == table->slots[slot].parent_pid &&
            (table->slots[i].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
            table->slots[i].p_flags &= (uint8_t)~MUZIX_PROC_WAIT_TICK;
            table->slots[i].tick_deadline_valid = 0u;
            muzix_proc_table_ready(table, i);
            break;
        }
    }
    return 0;
}
/* * Record an exit status for a slot.
 * \param[in,out] table: process table
 * slot: slot that exited
 * parent_pid: parent's process id
 * status: exit status
 * 0 on success, -1 on failure
 */
int muzix_proc_table_exit_status(muzix_kernel_proc_table_t *table,
 int slot,
 uint8_t parent_pid,
 uint8_t status)
{
 int parent_slot;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX ||
 !table->slots[slot].active) {
 return -1;
 }
 parent_slot = -1;
 for (int i = 0; i < MUZIX_PROC_TABLE_MAX; i++) {
 if (table->slots[i].active &&
 table->slots[i].pid == parent_pid) {
 parent_slot = i;
 break;
 }
 }
 if (parent_slot >= 0) {
 table->slots[parent_slot].child_user_time +=
 table->slots[slot].user_time + table->slots[slot].child_user_time;
 table->slots[parent_slot].child_sys_time +=
 table->slots[slot].sys_time + table->slots[slot].child_sys_time;
 }
 table->slots[slot].parent_pid = parent_pid;
 table->slots[slot].exit_status = status;
 table->slots[slot].exited = 1;
 if (muzix_proc_table_exit(table, slot) != 0) {
 return -1;
 }
 return 0;
}

/* * Queue a signal for a slot.
 * \param[in,out] table: process table
 * slot: slot to signal
 * signal: signal number
 * 0 on success, -1 on failure
 */
int muzix_proc_table_queue_signal(muzix_kernel_proc_table_t *table,
 int slot,
 uint8_t signal)
{
 uint16_t mask;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX ||
 !table->slots[slot].active || signal == 0 || signal > 16) {
 return -1;
 }
 mask = (uint16_t)1u << (signal - 1);
 table->slots[slot].pending_signals |= mask;
 return 0;
}

/* * Deliver pending signals for a slot.
 * \param[in,out] table: process table
 * slot: slot to deliver signals for
 * mm: MM service handle
 * 0 on success, -1 on failure
 */
int muzix_proc_table_deliver_pending(muzix_kernel_proc_table_t *table,
 int slot,
 muzix_mm_service_t *mm)
{
 uint16_t pending;
 uint8_t signal;
 uint8_t parent_pid;

 if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX ||
 !table->slots[slot].active) {
 return -1;
 }
 pending = table->slots[slot].pending_signals;
 if (pending == 0) {
 return 0;
 }
 signal = 1;
 while ((pending & 1u) == 0) {
 pending >>= 1;
 signal++;
 }
 parent_pid = table->slots[slot].parent_pid;
 table->slots[slot].pending_signals &=
 (uint16_t)~((uint16_t)1u << (signal - 1));
 muzix_proc_table_release_pages(table, slot, mm);
 return muzix_proc_table_exit_status(table, slot, parent_pid,
 (uint8_t)(128u + signal));
}

/* Whether a slot queued for dispatch can actually be dispatched. */
static int muzix_proc_slot_runnable(const muzix_proc_slot_t *p)
{
 return p &&
 p->active &&
 (p->p_flags & MUZIX_PROC_NO_MAP) == 0 &&
 p->entry_point != 0;
}

/* Take the first RUNNABLE slot out of queue `q`. */
static int muzix_proc_table_take(muzix_kernel_proc_table_t *table, int q)
{
 int head = table->ready_head[q];
 int found = -1;
 int slot;

 if (!table || q < 0 || q >= MUZIX_PROC_NQ) {
 return -1;
 }
 if (head < 0 || head >= MUZIX_PROC_TABLE_MAX) {
 return -1;
 }

 /* Read-only first pass, bounded at MUZIX_PROC_TABLE_MAX steps. */
 int steps;

 for (slot = head, steps = 0;
 slot >= 0 && slot < MUZIX_PROC_TABLE_MAX && steps < MUZIX_PROC_TABLE_MAX;
 slot = table->slots[slot].next_ready, steps++) {
 if (muzix_proc_slot_runnable(&table->slots[slot])) {
 found = slot;
 break;
 }
 }
 if (found < 0) {
 return -1; /* nothing runnable: the queue is untouched */
 }
 for (;;) {
 slot = table->ready_head[q];
 if (slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
 return -1; /* malformed chain; leave it alone */
 }
 table->ready_head[q] = table->slots[slot].next_ready;
 table->slots[slot].next_ready = -1;
 if (table->ready_head[q] == -1) {
 table->ready_tail[q] = -1;
 }
 if (slot == found) {
 return slot;
 }
 muzix_proc_enqueue(table, slot, q);
 }
}

/* * Pick the next process to run from the ready queues.
 * table: process table
 * Slot number of the next process, or -1 if none
 */
int muzix_proc_table_pick_next(muzix_kernel_proc_table_t *table)
{
 int q;
 int slot = -1;

 if (!table) {
 return -1;
 }

 for (q = MUZIX_PROC_TASK_Q; q < MUZIX_PROC_NQ; q++) {
 slot = muzix_proc_table_take(table, q);
 if (slot >= 0) {
 break;
 }
 }
 if (slot == -1) {
 table->current = -1;
 return -1;
 }

 table->current = slot;
 return slot;
}

/* * Yield: re-queue the current process and pick the next.
 * table: process table
 * Slot number of the next process, or -1 if none
 */
int muzix_proc_table_yield(muzix_kernel_proc_table_t *table)
{
 int current;
 int next_slot;

 if (!table) {
 return -1;
 }

 current = table->current;
 if (current >= 0 && current < MUZIX_PROC_TABLE_MAX &&
 table->slots[current].active &&
 (table->slots[current].p_flags & MUZIX_PROC_NO_MAP) == 0) {
 muzix_proc_table_ready(table, current);
 }

 next_slot = muzix_proc_table_pick_next(table);
 if (next_slot >= 0 && current >= 0 && current != next_slot) {
 table->slots[current].saved_context.pages[0] =
 table->slots[current].map.pages[0];
 table->slots[current].saved_context.pages[1] =
 table->slots[current].map.pages[1];
 table->slots[current].saved_context.pages[2] =
 table->slots[current].map.pages[2];
 table->slots[current].saved_context.pages[3] =
 table->slots[current].map.pages[3];
 }
 return next_slot;
}

/* * Switch to the kernel map.
 * \param[in,out] table: process table
 */
void muzix_proc_table_switch_to_kernel(muzix_kernel_proc_table_t *table)
{
 if (!table) {
 return;
 }

 table->current = -1;
}

/* * Return the current process's pid.
 * table: process table
 * Current process id
 */
int muzix_proc_table_current_pid(const muzix_kernel_proc_table_t *table)
{
 if (!table || table->current < 0 || table->current >= MUZIX_PROC_TABLE_MAX) {
 return -1;
 }

 return (int)table->slots[table->current].pid;
}

/* * Charge a slot for the time since it was last charged, and
 * re-stamp it.
 * \param[in,out] process: process slot to charge
 * kernel_time: 0 = user interval, 1 = kernel interval
 */
void muzix_proc_table_charge(muzix_proc_slot_t *process, uint8_t kernel_time)
{
 if (!process) {
 return;
 }

 if (kernel_time) {
 muzix_tick_add_process_time(&process->sys_time, &process->charge_stamp);
 } else {
 muzix_tick_add_process_time(&process->user_time, &process->charge_stamp);
 }
}
