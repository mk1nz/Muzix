#include "proc_table.h"

#include <string.h>

#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/syscall_entry.h"
#include "../platform/zeta-v2/trace.h"

/* The resume point the syscall trap captured on the way in, in the pair the
 * context switch actually consumes.  Defined in kernel/kernel_main.c next to the
 * entry-point pair, and read by kernel/proc_context.c for the same purpose. */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;

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

void muzix_proc_table_init(muzix_kernel_proc_table_t *table, const process_map_t *kernel_map)
{
    int i;
    int q;
    if (!table) {
        return;
    }

    memset(table, 0, sizeof(*table));
    /* Only the two fields the memset above cannot produce.
     *
     * This loop used to store zero into twenty-nine fields of every slot, one
     * at a time, immediately after a memset() had already zeroed every byte of
     * the table - so twenty-nine of the thirty-one stores were writing the value
     * that was already there. The two that are not are p_flags, which is not
     * zero, and next_ready, which is -1. */
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

void muzix_proc_table_register(muzix_kernel_proc_table_t *table,
                              int slot,
                              uint8_t pid,
                              const muzix_mproc_t *mp,
                              const process_map_t *map,
                              uint8_t q,
                              uint8_t flags)
{
    int current;

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

    /* One address for the twenty-two fields below, for the reason given at
     * muzix_proc_table_fork(): `table->slots[slot]` is a base plus an index
     * times sizeof(muzix_proc_slot_t), and SDCC recomputes that multiplication
     * and every field offset at each mention. */
    {
        muzix_proc_slot_t *p = &table->slots[slot];

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
        /* Start the accounting clock now, not at zero.  A slot created N ticks
         * after boot with a zero stamp would be charged all N of them by the
         * first muzix_proc_table_charge() - time the process did not exist for,
         * appearing as a UTIME it had not earned before running a single
         * instruction. */
        muzix_tick_stamp_store(&p->tick_stamp);
        p->next_ready = -1;
        p->mproc = *mp;
        p->map = *map;
    }
}

/* Take a slot out of whichever ready queue holds it.
 *
 * Lifted out of muzix_proc_table_exit() unchanged, because parking needs exactly
 * this walk and there was no way to have it twice for free: SDCC re-derives the
 * offset of next_ready at every step of it, and inside pick_next() - a function
 * already at a register-allocation cliff - the same logic cost 421 bytes for the
 * three priority queues.  Written once, it is paid once.
 *
 * Leaving the queue is what makes parking invisible to pick_next(), so the parked
 * flag never has to be tested there at all.  The alternative - stay queued and let
 * pick_next() skip and re-append - is the expensive one, and it puts the decision
 * about runnability in two places instead of one. */
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



/* The slot whose UTIME and STIME are currently being accumulated, or 0 when no
 * system call is in flight.  muzix_handle_userspace_syscall() sets it on the way
 * in; nothing else does.
 *
 * It exists for one reason: the accounting charges the wall time between the two
 * ends of a system call, and time spent with the processor stopped is wall time
 * that was not spent computing.  Without a way to reach the slot, that time lands
 * in STIME and a machine that is standing still reports itself as 100% busy -
 * which is exactly what `halt` in the console wait produced before this existed:
 * the processor genuinely idled, and the figure did not move. */
static muzix_proc_slot_t *g_accounted;

void muzix_proc_set_accounted(muzix_proc_slot_t *slot)
{
    g_accounted = slot;
}

/* Stop the processor without charging the time it spends stopped.
 *
 * The stamp is refreshed on BOTH sides of the halt.  Before, so that the interval
 * already worked is charged by the exit call as normal; after, so that the halt
 * itself is not charged to whatever the process does next.  Everything between two
 * halts is executed time, and everything from a halt to the next halt is not, so
 * the accumulator ends up holding executed time and nothing else - which is what
 * makes the busy fraction mean what it says.
 *
 * Safe with no system call in flight: it returns rather than stamping nothing, so
 * an early boot halt cannot corrupt a slot that is not being charged. */
void muzix_proc_halt_uncounted(void)
{
    if (g_accounted != 0) {
        muzix_tick_stamp_store(&g_accounted->tick_stamp);
    }
    /* Enable interrupts in the same assembly sequence as HALT so no tick can be
     * consumed in the gap between separate calls. */
    z80_idle_halt();
    if (g_accounted != 0) {
        muzix_tick_stamp_store(&g_accounted->tick_stamp);
    }
}

int muzix_proc_table_should_idle(muzix_kernel_proc_table_t *table)
{
    int slot;
    int parked = 0;

    if (!table) {
        return 0;
    }
    /* Waking first is what makes this safe to call in a loop: a deadline that has
     * passed produces a runnable slot, and the loop below sees it. */
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
    /* Nothing runnable.  Only stop if something is actually waiting for a wake,
     * which is the difference between idling and hanging. */
    for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
        if (table->slots[slot].active &&
            (table->slots[slot].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
            parked = 1;
            break;
        }
    }
    return parked;
}

/* Park a slot for a number of ticks.
 *
 * It leaves the ready queues entirely, which is the whole design: pick_next()
 * finds nothing to run, the loop's idle step stops the processor, and the tick
 * brings it back.  Nothing has to teach pick_next() about parking, so the one
 * function that decides what runs stays the one that already did.
 *
 * `ticks` counts on the 120 Hz CTC clock, so it is a real interval and not a busy
 * count: park(table, slot, 240) is two seconds.  The arithmetic wraps the same way
 * muzix_tty_wait_for_input()'s deadline does, because it is the same counter.
 *
 * The deadline lives in tick_stamp, which is also what muzix_proc_table_charge()
 * subtracts from.  Overwriting it here is deliberate and is what makes the
 * accounting honest: the interval up to the moment of parking has already been
 * charged at the previous charge point, and the parked interval must not be
 * charged to whatever the process does after it wakes.  The wake re-stamps it
 * again for the same reason at the other end. */
void muzix_proc_table_park(muzix_kernel_proc_table_t *table, int slot,
                           uint16_t ticks)
{
    muzix_proc_slot_t *p;

    if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
        return;
    }
    p = &table->slots[slot];
    if (!p->active || (p->p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
        return;
    }
    /* The resume point, and why it is two words and not one.
     *
     * On a Z80 a suspended process's program counter IS the return address in its
     * own stack frame, so SP alone does not lose it.  What has to be recorded is
     * the SP the caller of this function was running with, and the word sitting at
     * that address - which is where the caller returns to, and is the PC a resume
     * jumps to.  platform/zeta-v2/kernel_entry.s's _zeta_resume_jump restores SP
     * from one cell and jumps to an explicit PC in another, which is why both are
     * needed rather than trusting the stack to carry the address.
     *
     * This switch-out pattern is adapted from FUZIX: it stores SP on the way
     * out and restores it on the way in, under the comment "FIXME: do we
     * actually need to restore the stack!".  It is the whole of a context
     * switch on this processor.
     *
     * The third word is what the pending system call reports once resumed, and for
     * a park it is 0 - a park that answered nonzero would read to the library
     * wrapper that made it as a failed call. */
    {
        /* The two words the syscall trap already published, taken as a pair.
         *
         * This used to read the program counter back out of the stack:
         *
         *     uint16_t sp = z80_syscall_user_sp();
         *     if (sp != 0u) {
         *         muzix_proc_table_set_resume(table, slot,
         *                                          *(volatile uint16_t *)sp, sp, 0);
         *     }
         *
         * and that dereference is the fault that rebooted the machine.  A
         * process's stack lives in window 1, so `*(uint16_t *)sp` with sp in
         * 0x4000-0x7FFF only reads the process's own stack while the PROCESS map
         * is installed.  By the time any handler runs it is not: the trap has
         * already called zeta_kernel_enter(), window 1 is kernel page 0x21, and
         * the address read is an offset into the kernel's own code.  The trap
         * captured the return address separately, into muzix_resume_pc, for
         * exactly this reason - platform/zeta-v2/syscall_entry.s does it before
         * the map changes and says so at the capture - and this was the one
         * place that ignored it and re-derived the value.
         *
         * Measured, with the emulator reporting the store:
         *   muzix_proc_table_park(slot 1) at cycle 20,445,227, banks 20 21 22 23,
         *   sp = 0x7E98, and *(uint16_t *)0x7E98 = 0x66DD - the bytes DD 66 out
         *   of the kernel's own code at 0x7E98 (build/muzix-zeta-v2.rom).  Slot 1
         *   was therefore given resume_pc = 0x66DD, which is inside its own stack
         *   page, not its text page.  15,738,083 cycles later the park expired,
         *   muzix_context_switch_to() published that pair, zeta_resume_jump() set
         *   SP from muzix_resume_sp and jumped to 0x66DD, and the CPU executed
         *   stack bytes: window 0 onwards, then the boot stub's LDIR, then window
         *   1 mapped onto ROM page 1 so the stack went read-only and the tick
         *   ISR's pushes were discarded, and finally a `jp (hl)` through a zeroed
         *   return address at the end of muzix_fs_service_write() to 0x0000.
         *
         * The second word was wrong too, in the way syscall_entry.s:113-127
         * records at length: the ordinary return path is `ld sp,(SYSCALL_USER_SP)`
         * followed by `ret`, which leaves SP two bytes higher, and the switch path
         * jumps instead of returning, so it applies that +2 itself.  The raw
         * pre-call SP is therefore two bytes too low to resume on; muzix_resume_sp
         * is the value that is correct for a jump.
         *
         * A parked process that was not inside a system call has no such pair -
         * it would be the values from some earlier call, or nothing.  Zero SP
         * means there is no resume point to record, and get_resume() already
         * reports that by returning 0, so the slot is simply left unrecorded
         * rather than given a stale one.  muzix_resume_pc is tested as well
         * because zero is not a code address: a slot recorded with it would be
         * jumped to, and 0x0000 is the ROM boot stub. */
        uint16_t sp = z80_syscall_user_sp();
        uint16_t pc = muzix_resume_pc;
        uint16_t rsp = muzix_resume_sp;

        if (sp != 0u && pc != 0u) {
            muzix_proc_table_set_resume(table, slot, pc, rsp, 0);
        }
    }
    p->p_flags |= MUZIX_PROC_WAIT_TICK;
    p->tick_stamp = (uint16_t)(muzix_tick_now() + ticks);
    muzix_proc_table_unlink(table, slot);
    /* Parking is done by the running process, so this is usually the current
     * slot.  Clearing it stops the loop from treating "current" as runnable on
     * its way round to noticing that the queue is empty. */
    if (table->current == slot) {
        table->current = -1;
    }
}

/* Whether any slot is parked at all, whatever its deadline says.
 *
 * Distinct from muzix_proc_table_should_idle(), which refuses to answer yes unless
 * something is parked AND nothing is runnable.  That conjunction is the right test
 * for the scheduler's dispatch loop - a park is a promise that something will be
 * woken - but it is the wrong test for the question kernel_main asks, because the
 * state where the LAST process has exited has nothing parked and nothing runnable,
 * and should_idle() answers "there is work" there.
 *
 * That is a spin, not an idle: muzix_kernel_loop_run() would halt nothing, fall
 * through, return, and be called again immediately, at full speed, for ever.  It is
 * the same defect as the one in muzix_kernel_switchout() and it is reached the
 * moment any program finishes - so the load figure read 1.00 at an idle prompt
 * even with the park path working.  Asking this question lets the loop halt
 * instead, and the tick brings it back 120 times a second to ask again. */
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

/* Put every parked slot whose deadline has passed back in play.
 *
 * Called from the scheduler loop once an interrupt has returned, never from the
 * handler: muzix_kernel_loop_run() sits entirely in window 1 and this module
 * straddles the window 0/1 boundary, so neither is addressable from an interrupt
 * handler confined to windows 0 and 3.  The handler counts; the housekeeping runs
 * with the kernel map installed.
 *
 * Re-queueing is safe here and only here, because park() is what unlinked the
 * slot.  Calling muzix_proc_table_ready() on a slot that was still enqueued would
 * find it already ready and return without doing anything, and the process would
 * go on waiting for a deadline that had already passed. */
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
        if (!muzix_tick_expired(p->tick_stamp)) {
            continue;
        }
        p->p_flags &= (uint8_t)~MUZIX_PROC_WAIT_TICK;
        muzix_tick_stamp_store(&p->tick_stamp);
        muzix_proc_table_ready(table, slot);
        woke++;
    }
    return woke;
}

/* Wake every parked slot, because input has arrived.
 *
 * WHY THIS IS NEEDED AT ALL, GIVEN THAT PARK HAS A DEADLINE
 * --------------------------------------------------------
 * The deadline is what makes a park bounded, and it is long enough to be the
 * latency: lib/libc.c's read() parks for 240 ticks - two seconds - because that is
 * the longest a console read should be allowed to block, and it asks again when
 * the park expires.  So without this, a keystroke arriving half a second into a
 * park waits the remaining one and a half seconds before it is even looked at.
 * The park fixes the processor sitting still and leaves the console sluggish;
 * this is what lets the first of the two be true without the second.
 *
 * WHY IT IS CALLED FROM THE LOOP AND NOT FROM THE HANDLER
 * ------------------------------------------------------
 * Exactly the reason wake_expired() has, and the map argument in
 * platform/zeta-v2/tick.s is the one that settles it: the UART handler runs at an
 * arbitrary point in a map change and may touch windows 0 and 3 only, so it
 * cannot reach the ready queues.  It raises a flag in _DATA and stops.  The halt
 * loop reads the flag after the halt returns - which is the only moment an
 * interrupt can have been taken while the processor was stopped, and so exactly
 * the moment a byte might be waiting - and does the requeue from ordinary kernel
 * context with the map known.  The deferral costs nothing: the handler has just
 * returned control to that loop.
 *
 * THE COST, STATED PLAINLY
 * -----------------------
 * Every parked slot is woken, not just the one waiting for the console.  A
 * process parked on a pipe or a file is woken too, finds nothing, and parks
 * again - a spurious wakeup, correct, and bounded by how often input arrives.  It
 * is the alternative to tracking why each process parked, and there is no
 * "parked on the console" flag in the slot to track it with.
 *
 * Returns the number of slots requeued, so the caller can tell whether there is
 * now something to run - which is how the halt loop decides to stop idling. */
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
        muzix_tick_stamp_store(&p->tick_stamp);
        muzix_proc_table_ready(table, slot);
        woke++;
    }
    return woke;
}

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

int muzix_proc_table_fork(muzix_kernel_proc_table_t *table,
                          int parent_slot,
                          int child_slot,
                          uint8_t child_pid)
{
    int slot;

    ;;
    ;;

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
    table->slots[child_slot].mproc.mp_pid = child_pid;
    table->slots[child_slot].p_flags = MUZIX_PROC_NO_MAP;
    table->slots[child_slot].parent_pid = table->slots[parent_slot].pid;
    table->slots[child_slot].exit_status = 0;
    table->slots[child_slot].exited = 0;
    table->slots[child_slot].active = 1;
    table->slots[child_slot].pending_signals = 0;
    table->slots[child_slot].user_time = 0;
    table->slots[child_slot].sys_time = 0;
    table->slots[child_slot].child_user_time = 0;
    table->slots[child_slot].child_sys_time = 0;
    /* Not inherited from the parent, which is the whole point: the child has
     * been running for no time at all, and it starts counting from here.  The
     * slot was copied verbatim above, so without this the child would inherit
     * the parent's stamp and be charged for the parent's accumulated age. */
    muzix_tick_stamp_store(&table->slots[child_slot].tick_stamp);
    table->slots[child_slot].next_ready = -1;
    /* The whole slot was copied verbatim above, which includes the parent's
     * owned_pages. The child runs on the parent's map until it execs, so it
     * must not claim those pages: releasing them on the child's exit would
     * return pages the parent is still executing on. exec_loader replaces the
     * record with the pages it allocated. */
    table->slots[child_slot].owned_count = 0;
    table->slots[child_slot].owned_pages[0] = 0;
    table->slots[child_slot].owned_pages[1] = 0;
    /* The command name does not come across either, for the same reason in the
     * same direction: the child is a copy of the parent right up until it
     * execs, and until then it is not running its parent's program.  One byte is
     * enough - everything that reads the name stops at the terminator, and the
     * exec that follows overwrites the field anyway. */
    table->slots[child_slot].name[0] = '\0';
    /* The whole slot was copied verbatim, including the parent's resume
     * point. The child does not get to resume at the instruction its parent
     * was executing; whoever starts it has to give it a frame of its own. */
    table->slots[child_slot].resume_valid = 0;
    return 0;
}

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
    muzix_proc_table_ready(table, slot);
    return 0;
}

void muzix_proc_set_name(muzix_kernel_proc_table_t *table,
                         int slot,
                         const char *path)
{
    muzix_proc_slot_t *process;
    uint8_t i = 0;

    if (!table || !path || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
        return;
    }
    process = &table->slots[slot];

    /* Bounded copy of the path, NUL-terminated, into the one field of the slot
     * that lies inside the window SYS_PROCTAB hands to userspace - so a program
     * listing the table reads the command name in the transfer it was already
     * making, and the kernel pays no second copy-out for it.
     *
     * Truncation is not an error and the destination is not required to be
     * cleared first: a name too long for the field arrives as its leading
     * MUZIX_PROC_NAME_SIZE-1 characters, which is visible as a short name
     * rather than as a wrong one.
     *
     * This is the leading characters of the path, not its basename.  Turning
     * "/bin/ls" into "ls" means walking back to the last '/', and this kernel
     * has about forty bytes of _CODE to spend in total; the program that shows
     * the column does that part, and it is named in lib/proc_info.h. */
    while (i < MUZIX_PROC_NAME_SIZE - 1 && path[i] != '\0') {
        process->name[i] = path[i];
        i++;
    }
    process->name[i] = '\0';
}

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
     * sees owned_count == 0 instead of releasing the same page twice, which
     * would corrupt the allocator's free bitmap. */
    count = table->slots[slot].owned_count;
    table->slots[slot].owned_count = 0;
    for (i = 0; i < count && i < MUZIX_PROC_OWNED_PAGES; i++) {
        uint8_t page = table->slots[slot].owned_pages[i];
        table->slots[slot].owned_pages[i] = 0;
        muzix_mm_free_page(mm, page);
    }
}

int muzix_proc_table_exit(muzix_kernel_proc_table_t *table, int slot)
{
    if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX ||
        !table->slots[slot].active) {
        return -1;
    }

    muzix_proc_table_unlink(table, slot);

    table->slots[slot].active = 0;
    table->slots[slot].p_flags = MUZIX_PROC_FREE;
    if (table->current == slot) {
        table->current = -1;
    }
    return 0;
}

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
    for (parent_slot = 0; parent_slot < MUZIX_PROC_TABLE_MAX; parent_slot++) {
        if (table->slots[parent_slot].active &&
            table->slots[parent_slot].pid == parent_pid) {
            break;
        }
    }
    if (parent_slot == MUZIX_PROC_TABLE_MAX ||
        muzix_proc_table_exit(table, slot) != 0) {
        return -1;
    }
    table->slots[parent_slot].child_user_time +=
        table->slots[slot].user_time + table->slots[slot].child_user_time;
    table->slots[parent_slot].child_sys_time +=
        table->slots[slot].sys_time + table->slots[slot].child_sys_time;
    table->slots[slot].parent_pid = parent_pid;
    table->slots[slot].exit_status = status;
    table->slots[slot].exited = 1;
    return 0;
}

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

int muzix_proc_table_deliver_pending(muzix_kernel_proc_table_t *table,
                                     int slot)
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
    return muzix_proc_table_exit_status(table, slot, parent_pid,
                                        (uint8_t)(128u + signal));
}

int muzix_proc_table_pick_next(muzix_kernel_proc_table_t *table)
{
    int q;
    int slot = -1;

    if (!table) {
        return -1;
    }

    q = MUZIX_PROC_TASK_Q;
    while (table->ready_head[q] != -1) {
        slot = table->ready_head[q];
        table->ready_head[q] = table->slots[slot].next_ready;
        table->slots[slot].next_ready = -1;
        if (table->ready_head[q] == -1) {
            table->ready_tail[q] = -1;
        }
        if (table->slots[slot].active &&
            (table->slots[slot].p_flags & MUZIX_PROC_NO_MAP) == 0) {
            break;
        }
        slot = -1;
    }
    if (slot == -1 && table->ready_head[q] == -1) {
        q = MUZIX_PROC_SERVER_Q;
    }
    /* `slot == -1 &&` on the two loops below, and it is the whole fix.
     *
     * `q` is advanced only when the queue ran dry, so when a loop SUCCEEDS with
     * entries still behind it, q still names the queue it just took from - and the
     * next loop, which was written to be "the next priority down", popped from
     * that same queue again.  It overwrote `slot` with a second process and left
     * the first one unlinked and in no queue at all: lost, neither returned nor
     * re-queued.
     *
     * With one process ready this is invisible.  With two distinct processes in
     * one queue the first pick returns the second and the next pick returns -1,
     * having found nothing while a ready process existed - which is exactly what
     * kernel/test_proc_table.c now asserts, and what made a shell and a daemon
     * unable to coexist: the daemon was dropped from the queue on every attempt
     * instead of being run. */
    while (slot == -1 && table->ready_head[q] != -1) {
        slot = table->ready_head[q];
        table->ready_head[q] = table->slots[slot].next_ready;
        table->slots[slot].next_ready = -1;
        if (table->ready_head[q] == -1) {
            table->ready_tail[q] = -1;
        }
        if (table->slots[slot].active &&
            (table->slots[slot].p_flags & MUZIX_PROC_NO_MAP) == 0) {
            break;
        }
        slot = -1;
    }
    if (slot == -1 && table->ready_head[q] == -1) {
        q = MUZIX_PROC_USER_Q;
    }
    while (slot == -1 && table->ready_head[q] != -1) {
        slot = table->ready_head[q];
        table->ready_head[q] = table->slots[slot].next_ready;
        table->slots[slot].next_ready = -1;
        if (table->ready_head[q] == -1) {
            table->ready_tail[q] = -1;
        }
        if (table->slots[slot].active &&
            (table->slots[slot].p_flags & MUZIX_PROC_NO_MAP) == 0) {
            break;
        }
        slot = -1;
    }
    if (slot == -1) {
        table->current = -1;
        return -1;
    }

    table->current = slot;
    return slot;
}

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
        /* Context switch is handled by caller via MM service */
        table->slots[current].saved_context.pages[0] = table->slots[current].map.pages[0];
        table->slots[current].saved_context.pages[1] = table->slots[current].map.pages[1];
        table->slots[current].saved_context.pages[2] = table->slots[current].map.pages[2];
        table->slots[current].saved_context.pages[3] = table->slots[current].map.pages[3];
    }
    return next_slot;
}

void muzix_proc_table_switch_to_kernel(muzix_kernel_proc_table_t *table)
{
    if (!table) {
        return;
    }

    table->current = -1;
    /* Kernel map switch handled by caller via MM service */
}

int muzix_proc_table_current_pid(const muzix_kernel_proc_table_t *table)
{
    if (!table || table->current < 0 || table->current >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }

    return (int)table->slots[table->current].pid;
}

/* Real UTIME and STIME, in ticks of the 120 Hz CTC timer.
 *
 * This replaces a `user_time++` that stood in two places in the scheduler loop
 * and counted something that is not time at all: the number of times the kernel
 * had entered userspace.  A process that was never preempted was never charged,
 * so on a machine where nothing is time-sliced - which is every machine this
 * kernel has ever run on - the columns read zero, which reads as a broken
 * counter rather than as a truthful one.  That is the defect apps/top.c spent
 * four lines of screen explaining.  It was also, measured, 191 bytes of _CODE
 * for two increments of a field through a table index.
 *
 * The tick makes the real quantity measurable, and the syscall is the only
 * boundary a process crosses on its own.  Between the kernel handing it the CPU
 * and it trapping, it was running: user time.  Between it trapping and the
 * kernel answering, the kernel was working for it: system time.  Charging at
 * both ends of the syscall therefore accounts for every tick, without needing a
 * timer-driven resample of a process that is not running - which is what
 * preemption would need, and is exactly what this task is not doing.
 *
 * The arithmetic is in muzix_tick_add_process_time(), in assembly, and the
 * reason is measured rather than preferred: written in C this function is 247
 * bytes of _CODE, because struct proc_info makes the counters uint32_t and
 * SDCC 4.5's 32-bit add through a stack frame is four times the cost of the
 * four `add`/`adc` and four stores the operation actually is.  The kernel is
 * 160 bytes above the stack headroom floor its own deepest measured chain sets,
 * so those 194 bytes were the difference between fitting and failing the build.
 *
 * Ticks, not seconds.  The columns are documented as seconds in struct
 * proc_info, and 120 ticks make a second exactly, so the division is available
 * to a caller that wants seconds - but it is not done here, because a 16-bit
 * divide is not free either and nothing in the tree needs the conversion. */
void muzix_proc_table_charge(muzix_proc_slot_t *process, uint8_t kernel_time)
{
    if (!process) {
        return;
    }

    /* Which of the two, and the two are adjacent uint32_t fields at fixed
     * offsets, so this is two address arithmetic sequences and no branch in the
     * callee.  Passing the counters by address rather than the slot keeps the
     * assembly free of struct offsets, which is the thing that would silently
     * break if the struct ever gained a field ahead of them. */
    if (kernel_time) {
        muzix_tick_add_process_time(&process->sys_time, &process->tick_stamp);
    } else {
        muzix_tick_add_process_time(&process->user_time, &process->tick_stamp);
    }
}
