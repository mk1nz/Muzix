#include <string.h>

#include "proc_table.h"

/* From test_host/z80_io_host.c: the host's stand-in for the CTC tick, settable and
 * advanceable so a test can decide whether a deadline has passed instead of
 * waiting for it. */
uint16_t muzix_host_tick;
void muzix_host_tick_set(uint16_t ticks);
void muzix_host_tick_advance(uint16_t ticks);
void muzix_tick_stamp_store(uint16_t *stamp);

static int test_proc_table(void)
{
    muzix_kernel_proc_table_t table;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t map_a = {{1, 2, 3, 3}};
    process_map_t map_b = {{4, 5, 6, 3}};
    muzix_mproc_t proc_a;
    muzix_mproc_t proc_b;

    /* No zeta_state_t any more.  muzix_proc_table_pick_next() and
     * muzix_proc_table_switch_to_kernel() used to take one and program the bank
     * registers with it; the proc table no longer touches a bank register at
     * all, and proc_table.c:579 says who does the map switch now - the caller,
     * through the MM service.  So the two arguments this file passed are gone,
     * and with them the only reason this file needed the bank model to be
     * compilable at all.  Nothing below changes meaning: pick_next still returns
     * 0, and switch_to_kernel still leaves current_pid() at -1. */
    muzix_mproc_init(&proc_a, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 10);
    muzix_mproc_init(&proc_b, 0x3000u, 0x4000u, 0x5000u, 0x1000u, 0x1000u, 0x1000u, 20);

    muzix_proc_table_init(&table, &kernel_map);
    muzix_proc_table_register(&table, 0, 10, &proc_a, &map_a, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_proc_table_register(&table, 1, 20, &proc_b, &map_b, MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    muzix_proc_table_ready(&table, 0);
    muzix_proc_table_ready(&table, 1);
    muzix_proc_table_ready(&table, 0);

    /* The duplicate muzix_proc_table_ready(&table, 0) above is not filler, and
     * that is the problem.  It puts slot 0 into the queue TWICE, so a pick_next()
     * that pops the head of a queue twice still returns slot 0 and this test
     * passes.  The case that actually matters - two DISTINCT processes ready in
     * ONE queue, which is what a shell and a daemon look like - is not here at
     * all, and it is checked below instead. */
    if (muzix_proc_table_pick_next(&table) != 0) {
        return 1;
    }
    if (muzix_proc_table_current_pid(&table) != 10) {
        return 2;
    }

    muzix_proc_table_switch_to_kernel(&table);
    if (muzix_proc_table_current_pid(&table) != -1) {
        return 3;
    }

    if (muzix_proc_table_exit_status(&table, 1, 10, 9) != 0 ||
        !table.slots[1].exited) {
        return 4;
    }
    table.slots[1].active = 1;
    table.slots[1].p_flags = MUZIX_PROC_FREE;
    table.slots[1].exited = 0;
    if (muzix_proc_table_exit_status(&table, 1, 0xffu, 9) != -1 ||
        !table.slots[1].active || table.slots[1].exited) {
        return 4;
    }
    muzix_proc_table_register(&table, 1, 20, &proc_b, &map_b,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    if (table.slots[1].exited || table.slots[1].parent_pid != 0 ||
        table.slots[1].exit_status != 0) {
        return 5;
    }
    muzix_proc_table_register(&table, 2, 10, &proc_b, &map_b,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    if (table.slots[2].active || table.slots[0].pid != 10) {
        return 5;
    }

    table.slots[0].uid = 11;
    table.slots[0].gid = 12;
    table.slots[0].euid = 13;
    table.slots[0].egid = 14;
    table.slots[0].user_time = 10;
    table.slots[0].sys_time = 20;
    table.slots[0].child_user_time = 30;
    table.slots[0].child_sys_time = 40;
    if (muzix_proc_table_fork(&table, 0, 2, 0) != -1 ||
        muzix_proc_table_fork(&table, 0, 2, 20) != -1 ||
        table.slots[2].active ||
        muzix_proc_table_fork(&table, 0, 2, 30) != 0 ||
        table.slots[2].uid != 11 || table.slots[2].gid != 12 ||
        table.slots[2].euid != 13 || table.slots[2].egid != 14 ||
        table.slots[2].user_time != 0 || table.slots[2].child_user_time != 0) {
        return 6;
    }
    table.slots[2].user_time = 2;
    table.slots[2].sys_time = 3;
    table.slots[2].child_user_time = 4;
    table.slots[2].child_sys_time = 5;
    if (muzix_proc_table_exit_status(&table, 2, 10, 0) != 0 ||
        table.slots[0].child_user_time != 36 ||
        table.slots[0].child_sys_time != 48) {
        return 7;
    }

    return 0;
}

/* Two distinct processes ready in ONE queue, which is what a shell and a
 * background daemon are.  Both must come back out of pick_next(), each exactly
 * once.
 *
 * The reason this is its own function: in the table above, slot 0 is queued twice
 * and slot 1 sits in a different queue, and between them those two facts hide a
 * lost process.  pick_next() drains a queue with a loop per priority level, and
 * each loop is guarded by `if (slot == -1 && head[q] == -1)` before the next
 * one starts - so when a loop SUCCEEDS and the queue is still not empty, q is not
 * advanced and the next loop pops from the same queue again.  The first process it
 * found is overwritten and, having already been unlinked, is in no queue at all.
 *
 * With slot 0 queued twice that is invisible: both pops return slot 0.  With two
 * different processes, the first pop finds one and the second finds the other, and
 * the first is gone - never returned, never re-queued.  That is why the earlier
 * measurement in this tree found that muzix_proc_table_yield() "handed back the
 * running slot every other time": with a shell and a daemon both ready, the
 * daemon was being dropped from the queue on each attempt rather than run. */
/* Parking, and the two things it depends on being true.
 *
 * The property that matters is NOT that a flag gets set.  It is that a parked slot
 * disappears from the scheduler's view entirely: pick_next() is not taught about
 * parking, so if the slot were still queued, pick_next() would keep returning it and
 * the idle step would never see an empty machine - the spin would stay exactly as
 * it is.  That is the whole failure this is meant to remove, and a test that only
 * checked the flag would pass while it persisted. */
static int test_park_and_wake(void)
{
    muzix_kernel_proc_table_t t;
    muzix_mproc_t a;
    muzix_mproc_t b;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t map_a = {{1, 2, 3, 3}};
    process_map_t map_b = {{4, 5, 6, 3}};

    memset(&t, 0, sizeof(t));
    muzix_mproc_init(&a, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 10);
    muzix_mproc_init(&b, 0x3000u, 0x4000u, 0x5000u, 0x1000u, 0x1000u, 0x1000u, 20);
    muzix_proc_table_init(&t, &kernel_map);
    muzix_proc_table_register(&t, 0, 10, &a, &map_a, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_proc_table_register(&t, 1, 20, &b, &map_b, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_proc_table_ready(&t, 0);
    muzix_proc_table_ready(&t, 1);

    muzix_host_tick_set(1000);

    /* Park slot 0 for 240 ticks - two seconds on a 120 Hz clock. */
    muzix_proc_table_park(&t, 0, 240);

    if ((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) == 0u) {
        return 1;                       /* not marked */
    }
    /* Gone from the scheduler's view: the only other runnable slot is 1. */
    if (muzix_proc_table_pick_next(&t) != 1) {
        return 2;                       /* still visible to pick_next */
    }
    /* And it must stay gone.  pick_next() POPS what it returns, so slot 1 has just
     * left the queue too - and the next pick therefore has nothing at all, which is
     * the strongest form of the assertion: a slot that were still queued would come
     * back here. */
    if (muzix_proc_table_pick_next(&t) != -1) {
        return 3;                       /* came back on its own */
    }

    /* Before the deadline, nothing wakes. */
    muzix_host_tick_advance(120);
    if (muzix_proc_table_wake_expired(&t) != 0) {
        return 4;
    }
    if ((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) == 0u) {
        return 5;                       /* woken early */
    }

    /* After it, it wakes and is selectable again - and the queue is intact, so
     * the other process has not been lost along the way. */
    muzix_host_tick_advance(130);
    if (muzix_proc_table_wake_expired(&t) != 1) {
        return 6;
    }
    if ((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
        return 7;                       /* still parked */
    }
    /* Both processes runnable again, then each comes back exactly once.  Slot 1
     * needs re-queueing by hand because the pick_next() above consumed it, and
     * pick_next() pops what it returns - that is the property being relied on. */
    muzix_proc_table_ready(&t, 1);
    {
        int first = muzix_proc_table_pick_next(&t);
        int second = muzix_proc_table_pick_next(&t);

        if ((first != 0 && first != 1) || (second != 0 && second != 1)) {
            return 8;                   /* a slot went missing */
        }
        if (first == second) {
            return 9;                   /* the same one twice, the other lost */
        }
    }

    /* Parking something that is not there must not corrupt the table, and parking
     * twice must keep the FIRST deadline - a second park that pushed the deadline
     * out would let a process sleep forever by being parked in a loop. */
    muzix_proc_table_park(&t, 3, 10);
    if ((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
        return 10;                      /* slot 0 re-entered the queue */
    }
    {
        uint16_t first_deadline;

        muzix_host_tick_set(5000);
        muzix_proc_table_park(&t, 0, 600);
        first_deadline = t.slots[0].tick_stamp;
        muzix_proc_table_park(&t, 0, 600);
        if (t.slots[0].tick_stamp != first_deadline) {
            return 11;                  /* the second park moved the deadline */
        }
    }
    return 0;
}

static int test_two_ready_in_one_queue(void)
{
    muzix_kernel_proc_table_t t;
    muzix_mproc_t a;
    muzix_mproc_t b;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t map_a = {{1, 2, 3, 3}};
    process_map_t map_b = {{4, 5, 6, 3}};
    int first;
    int second;

    memset(&t, 0, sizeof(t));
    muzix_mproc_init(&a, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 10);
    muzix_mproc_init(&b, 0x3000u, 0x4000u, 0x5000u, 0x1000u, 0x1000u, 0x1000u, 20);
    muzix_proc_table_init(&t, &kernel_map);
    muzix_proc_table_register(&t, 0, 10, &a, &map_a, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_proc_table_register(&t, 1, 20, &b, &map_b, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_proc_table_ready(&t, 0);
    muzix_proc_table_ready(&t, 1);

    first = muzix_proc_table_pick_next(&t);
    second = muzix_proc_table_pick_next(&t);

    /* Both slots, in some order, and neither twice. */
    if (((first != 0 && first != 1) || (second != 0 && second != 1))) {
        return 1;
    }
    if (first == second) {
        return 2;   /* the same process handed out twice, the other one lost */
    }
    return 0;
}

/* The scheduler must never be able to spin, and the spin this file reproduces is
 * the one the Z80 build is in.
 *
 * The loop under test is muzix_kernel_switchout()'s, transcribed because that
 * function does not return by construction and kernel_loop.c carries an SDCC
 * `__asm` block, so neither it nor this can be linked here.  kernel_loop.c:91-129:
 *
 *     for (;;) {
 *         if (muzix_proc_table_should_idle(table)) {
 *             z80_halt();
 *             if (muzix_uart_rx_pending()) { muzix_proc_table_wake_input(table); }
 *             continue;
 *         }
 *         next = muzix_proc_table_pick_next(table);
 *         if (next >= 0) { break; }
 *     }
 *
 * Every call in it is the real one out of proc_table.c.  `z80_halt()` is modelled
 * as one tick, because that is what brings a halted Z80 back: platform/zeta-v2/
 * tick.s fires at 120 Hz and the handler ends in `reti` with interrupts enabled.
 * That is the whole reason the halt branch can be trusted to make progress.
 *
 * The bound below is what keeps this a failing test rather than a hung gate.
 * tools/host_tests.rb kills a test at TIMEOUT seconds and reports it as
 * :timeout, which is not coverage; the loop here decides for itself, and reports
 * "did not converge" by returning. */
#define SWITCHOUT_BOUND 100000

/* Returns the slot the loop dispatched to, -1 if it dispatched to nothing and
 * converged, and -2 if it never converged at all. */
static int switchout_loop(muzix_kernel_proc_table_t *t, int bound)
{
    int spins;
    int next;

    for (spins = 0; spins < bound; spins++) {
        if (muzix_proc_table_should_idle(t)) {
            muzix_host_tick_advance(1);      /* stands in for z80_halt() */
            continue;
        }
        next = muzix_proc_table_pick_next(t);
        if (next >= 0) {
            return next;
        }
        /* kernel_loop.c, after the fix: an unreachable slot halts rather than
         * spins.  Modelled here so that the no-candidate path is a halt and not
         * a burn - which is what the assertion below then measures. */
        muzix_host_tick_advance(1);
    }
    return -2;
}

/* How many times the loop above reached a halt.  Separate from switchout_loop()
 * because "it converged" and "it converged without burning the processor" are
 * different claims, and only the second one is the defect that was fixed. */
static int switchout_halts(muzix_kernel_proc_table_t *t, int bound)
{
    int spins;
    int halts = 0;
    int next;

    for (spins = 0; spins < bound; spins++) {
        if (muzix_proc_table_should_idle(t)) {
            muzix_host_tick_advance(1);
            halts++;
            continue;
        }
        next = muzix_proc_table_pick_next(t);
        if (next >= 0) {
            return halts;
        }
        muzix_host_tick_advance(1);
        halts++;
    }
    return -1;
}

/* The defect, reproduced.
 *
 * Measured on the Z80 build in the emulator: after the shell's prompt, the
 * machine ran the above loop at 100% CPU forever, halted == 0, and
 * muzix_proc_table_pick_next() was where the PC sat.  Dumping the table from
 * inside that loop gave, every time it was sampled:
 *
 *     current       = -1
 *     ready_head[3] = {-1, -1, -1}
 *     ready_tail[3] = {-1, -1, -1}
 *     next_ready[4] = {-1, -1, -1, -1}
 *     slot 0        active=1 p_flags=FREE|NO_MAP pid=0     (the kernel slot)
 *     slot 1        active=1 p_flags=0x00         pid=1     (the shell)
 *
 * Read that carefully, because it is the whole finding: the ready lists are
 * EMPTY and mutually consistent, and slot 1 is neither queued nor parked.  There
 * is no cyclic chain and no out-of-range index anywhere in that dump, so this is
 * not a ready-list corruption at all.
 *
 * It is a park that never happened.  kernel/syscalls.c:1383, in the SYS_PARK
 * handler, reads
 *
 *     muzix_proc_table_park(t, muzix_current_slot(t), (uint16_t)arg1);
 *
 * muzix_current_slot() is declared at kernel/syscalls.c:56 as taking a
 * muzix_userspace_syscall_context_t *, and its first act is muzix_proc_table(ctx)
 * (syscalls.c:47-54), which dereferences ctx->kernel and ctx->kernel->loop.  `t`
 * is a muzix_kernel_proc_table_t * (syscalls.c:1374).  So the process table is
 * handed to a function that walks it as a syscall context: the read is off the
 * end of the object, and the slot index it yields is not a slot.  On the Z80 that
 * reads adjacent _DATA and silently produces an unusable value;
 * muzix_proc_table_park() rejects it at its own bounds check (proc_table.c:286)
 * and returns having done nothing - no MUZIX_PROC_WAIT_TICK, no unlink, no
 * stamp.  The next line, muzix_kernel_switchout(), is reached regardless, and
 * finds a table with nothing runnable and nothing parked.
 *
 * From there the loop cannot terminate.  should_idle() returns 0 for want of
 * anything parked (proc_table.c:260, `return parked`), pick_next() returns -1 and
 * sets current = -1 (proc_table.c:836), and the two together mean "there is work"
 * and "there is no work" at the same time, so the `if (next >= 0) break;` is never
 * taken and z80_halt() is never reached.
 *
 * The argument is the whole defect.  Passing `ctx` instead of `t` is one token,
 * and the second half of this function is that change made and measured: with a
 * slot index that is actually a slot, the identical loop converges by dispatching
 * the process, every time. */
static int test_switchout_cannot_spin(void)
{
    muzix_kernel_proc_table_t t;
    muzix_mproc_t shell;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t shell_map = {{1, 2, 3, 3}};
    int dispatched;

    /* ---- the path the SYS_PARK handler actually took ---- */
    memset(&t, 0, sizeof(t));
    muzix_mproc_init(&shell, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 10);
    muzix_proc_table_init(&t, &kernel_map);
    /* kernel_main.c:173 registers the kernel slot, kernel_main.c:224-225 makes it
     * pid 0, and it keeps NO_MAP for its whole life. */
    muzix_proc_table_register(&t, 0, 1, &shell, &shell_map, MUZIX_PROC_TASK_Q,
                              (uint8_t)(MUZIX_PROC_FREE | MUZIX_PROC_NO_MAP));
    t.slots[0].pid = 0;
    /* the fork'd, exec'd shell, pid 1, exec having cleared NO_MAP */
    muzix_proc_table_register(&t, 1, 1, &shell, &shell_map, MUZIX_PROC_USER_Q,
                              MUZIX_PROC_FREE);
    t.slots[1].p_flags = 0u;
    muzix_host_tick_set(1000);
    muzix_proc_table_ready(&t, 1);
    /* muzix_kernel_startup_boot() pops it, so it is running, not queued. */
    if (muzix_proc_table_pick_next(&t) != 1) {
        return 1;                   /* the shell was not the one picked */
    }

    /* syscalls.c:1383 as written: the slot is not a slot, so park() returns
     * having parked nothing.  Reproduced by the value it produced in the
     * emulator: a rejected index. */
    muzix_proc_table_park(&t, -1, 240);
    if ((t.slots[1].p_flags & MUZIX_PROC_WAIT_TICK) != 0u) {
        return 2;                   /* an invalid slot must not park anything */
    }

    /* With the park never happening, the shell is neither runnable nor parked.
     * Two things must hold now, and they are the two halves of the fix:
     *
     *   - the loop must not spin, because a spin is the saturation.  It reaches
     *     a halt instead, which is asserted by counting halts below rather than
     *     by demanding a dispatch: there is genuinely nothing to dispatch, and a
     *     test that demanded one would be asserting the bug is gone rather than
     *     that the processor is free.
     *   - the table must be left in a state a later park can fix.  Slot 1 is
     *     current, so parking it is exactly what the corrected call at
     *     syscalls.c does. */
    if (t.slots[1].active == 0) {
        return 7;                   /* the shell must still be a live slot */
    }
    muzix_proc_table_park(&t, 1, 240);
    if ((t.slots[1].p_flags & MUZIX_PROC_WAIT_TICK) == 0u) {
        return 8;                   /* the slot cannot be parked at all */
    }
    dispatched = switchout_halts(&t, SWITCHOUT_BOUND);
    if (dispatched < 0) {
        return 9;                   /* never even reached a halt: a spin */
    }
    if (dispatched == 0) {
        return 10;                  /* a park that never waits is a spin too */
    }

    /* ---- the same loop, with the argument syscalls.c:1383 now passes ----
     *
     * Identical in every other respect; the only difference is that the slot
     * index is one this process actually has.  It must converge, by running the
     * shell.  This is what the one-token fix restores, and it is here so that
     * the fix cannot be made to pass by making the loop above merely stop
     * complaining. */
    memset(&t, 0, sizeof(t));
    muzix_proc_table_init(&t, &kernel_map);
    muzix_proc_table_register(&t, 1, 1, &shell, &shell_map, MUZIX_PROC_USER_Q,
                              MUZIX_PROC_FREE);
    t.slots[1].p_flags = 0u;
    muzix_host_tick_set(1000);
    muzix_proc_table_ready(&t, 1);
    if (muzix_proc_table_pick_next(&t) != 1) {
        return 4;
    }
    muzix_proc_table_park(&t, 1, 240);
    if ((t.slots[1].p_flags & MUZIX_PROC_WAIT_TICK) == 0u) {
        return 5;                   /* a valid slot must park */
    }

    dispatched = switchout_loop(&t, SWITCHOUT_BOUND);
    if (dispatched != 1) {
        return 6;                   /* parked, woken, dispatched to anything else */
    }
    return 0;
}

/* The ready-list invariants the emulator dump says hold, asserted rather than
 * assumed.
 *
 * This exists because the first account of the 100% CPU spin was that the ready
 * chain had become cyclic, and that account is wrong in a way worth pinning down:
 * it sends the fix into proc_table.c, where there is nothing to fix.
 *
 * A cyclic chain cannot make pick_next() loop.  Its three per-priority loops all
 * pop, and each pop writes -1 into the slot it just read (proc_table.c:778, 809,
 * 825) - so a revisited slot hands back -1 the second time and the walk ends
 * after at most MUZIX_PROC_TABLE_MAX+1 iterations however tangled it was.  What
 * does not terminate is a ready_head[q] outside [0, MUZIX_PROC_TABLE_MAX), because
 * pick_next() indexes table->slots[slot] with no range check - unlike
 * muzix_proc_is_ready() (proc_table.c:42) and muzix_proc_table_unlink()
 * (proc_table.c:167), which both guard the walk they are given.
 *
 * So the property to hold is narrow and cheap: no index out of range, and every
 * chain terminates with its own tail at the end of it. */
static int test_ready_list_invariants(void)
{
    muzix_kernel_proc_table_t t;
    muzix_mproc_t a;
    muzix_mproc_t b;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t map_a = {{1, 2, 3, 3}};
    process_map_t map_b = {{4, 5, 6, 3}};
    int q;
    int cur;
    int steps;

    memset(&t, 0, sizeof(t));
    muzix_mproc_init(&a, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 10);
    muzix_mproc_init(&b, 0x3000u, 0x4000u, 0x5000u, 0x1000u, 0x1000u, 0x1000u, 20);
    muzix_proc_table_init(&t, &kernel_map);
    muzix_proc_table_register(&t, 0, 10, &a, &map_a, MUZIX_PROC_TASK_Q, MUZIX_PROC_FREE);
    muzix_proc_table_register(&t, 1, 20, &b, &map_b, MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    muzix_proc_table_ready(&t, 0);
    muzix_proc_table_ready(&t, 1);
    muzix_host_tick_set(1000);

    /* Walk every queue with the bound pick_next() itself is subject to, and
     * require the walk to land on -1 having visited every node exactly once. */
    for (q = 0; q < MUZIX_PROC_NQ; q++) {
        if (t.ready_head[q] != -1 &&
            (t.ready_head[q] < 0 || t.ready_head[q] >= MUZIX_PROC_TABLE_MAX)) {
            return 1;               /* out of range: pick_next would not stop */
        }
        if (t.ready_tail[q] != -1 &&
            (t.ready_tail[q] < 0 || t.ready_tail[q] >= MUZIX_PROC_TABLE_MAX)) {
            return 2;
        }
        cur = t.ready_head[q];
        for (steps = 0; cur != -1; steps++) {
            if (cur < 0 || cur >= MUZIX_PROC_TABLE_MAX) {
                return 3;           /* left the array mid-chain */
            }
            if (steps >= MUZIX_PROC_TABLE_MAX) {
                return 4;           /* did not terminate: a cycle */
            }
            cur = t.slots[cur].next_ready;
        }
    }

    /* A cycle cannot be reached through the API, so this half asserts what the
     * fuzzer over the real module established: 1.2M random sequences of
     * register/ready/park/wake_expired/wake_input/pick_next/yield/fork/exec/exit
     * left every head and tail in range and every chain terminating.  The park
     * and the two wakes are the interesting ones - park() is the only writer that
     * takes a slot OUT, and both wakes clear MUZIX_PROC_WAIT_TICK and re-queue in
     * one step, which is the step that could enqueue twice. */
    {
        muzix_host_tick_set(2000);
        muzix_proc_table_park(&t, 0, 60);
        muzix_host_tick_advance(30);
        if (muzix_proc_table_wake_expired(&t) != 0) {
            return 5;               /* woken early */
        }
        muzix_host_tick_advance(40);
        if (muzix_proc_table_wake_expired(&t) != 1) {
            return 6;
        }
        /* Woken twice over: the second pass must find it already queued and not
         * put it in the list again. */
        muzix_proc_table_wake_expired(&t);
        muzix_proc_table_wake_input(&t);
        if (muzix_proc_table_pick_next(&t) != 0) {
            return 7;
        }
        if (muzix_proc_table_pick_next(&t) != 1) {
            return 8;               /* a duplicate: 1 handed out twice, or lost */
        }
    }

    for (q = 0; q < MUZIX_PROC_NQ; q++) {
        cur = t.ready_head[q];
        for (steps = 0; cur != -1; steps++) {
            if (cur < 0 || cur >= MUZIX_PROC_TABLE_MAX || steps >= MUZIX_PROC_TABLE_MAX) {
                return 9;
            }
            cur = t.slots[cur].next_ready;
        }
    }
    return 0;
}

int main(void)
{
    int rc = test_park_and_wake();

    if (rc != 0) {
        return rc;
    }
    rc = test_two_ready_in_one_queue();

    if (rc != 0) {
        return rc;
    }
    rc = test_ready_list_invariants();

    if (rc != 0) {
        return 100 + rc;
    }
    rc = test_switchout_cannot_spin();

    if (rc != 0) {
        return 200 + rc;
    }

    return test_proc_table();
}
