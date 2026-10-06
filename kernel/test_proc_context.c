#include "proc_context.h"

#include <string.h>

#include "../platform/zeta-v2/bank_io.h"

/*
 * kernel/proc_context.c - the context switch.
 * ==========================================
 *
 * There has never been a test for this file.  It is two functions and they are
 * where the tree was wrong twice, both times in a way that presented as "the
 * shell is broken":
 *
 *   - A process was resumed with its stack pointer two bytes below where the
 *     syscall stub's `ret` needed it, so the return address came off the wrong
 *     word and the process resumed inside the wrong frame.
 *   - A slot index was stored in a pid field, so a process came back believing
 *     it was a different process from the one that had forked it.
 *
 * Neither is visible from a return value: the switch returns 0, the trap does
 * not come back, and whatever happened happens after the kernel is out of the
 * picture.  So this file checks the two things that ARE visible before that:
 * the decision, and the bookkeeping the window-0 assembly is about to read.
 *
 * WHAT THE HOST CANNOT REACH, AND WHY
 * ----------------------------------
 * Nothing below runs a Z80.  The map itself is installed by
 * zeta_resume_process() in platform/zeta-v2/context_switch.c, which programs
 * the bank registers itself - inline assembly SDCC cannot emit, in a file the
 * host compiler rejects.  The transfer to the
 * process is then muzix_zeta_resume_from() -> platform/zeta-v2/kernel_entry.s,
 * which is window-0 code by necessity: window 1 is a process page by the time it
 * runs, so the instructions doing it cannot live there.
 *
 * That leaves four things a host test cannot do, and they are the four things
 * that matter most about a context switch:
 *
 *   - that the four bank registers actually receive muzix_resume_banks[0..3].
 *     Checked here only to the extent that the four bytes the assembly reads are
 *     the target's, in the right order.
 *   - that `ld sp,(_muzix_resume_sp)` / `jp (hl)` use them.  SP is loaded from
 *     one window-0 cell and the program counter out of another, and an off-by-
 *     two in the pair is exactly the first defect listed above.  Nothing here can
 *     see it; only hardware can.
 *   - that the stack the process resumes on is the stack its own frames were
 *     written to.  The fork handler's chunked copy is what establishes that and
 *     it is checked where it happens, in kernel/test_syscalls.c.
 *   - that the bank registers are write-only and so cannot be read back to
 *     confirm the install.  AGENTS.md says so; there is no read.
 *
 * This is the same reason platform/zeta-v2/test_kernel/bank_model.c is kept out
 * of the host gate by tools/host_tests.rb, and the same division of labour:
 * test the decision and the bookkeeping here, and let the emulator and the
 * board own the machine.
 */

/* Published to window-0 assembly and defined in kernel/kernel_main.c.  The
 * runner links the real kernel_main.c to get them rather than defining them
 * here, so that a definition it stopped providing would break this test's link
 * instead of quietly passing against a second set of cells. */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;
extern uint16_t muzix_resume_ret;
extern uint8_t muzix_resume_banks[4];
extern uint16_t muzix_switch_pending;

#define CHECK(cond, why) do { if (!(cond)) { return (why); } } while (0)

/* The parent is in slot 0 and its child in slot 1, and the child's pid is 7 -
 * not 2.  Every assertion below that cares about the difference between a pid
 * and a slot therefore has a value that a mix-up cannot produce by accident,
 * which is the whole reason the two defects this file exists for were
 * survivable for as long as they were. */
#define PARENT_SLOT 0
#define CHILD_SLOT  1
#define PARENT_PID  1
#define CHILD_PID   7

/* Where each of them was when it made its call.  Distinct in both halves, so a
 * frame recorded on the wrong row is visible. */
#define PARENT_PC   0x83BEu
#define PARENT_SP   0x7FAEu
#define CHILD_PC    0x8123u
#define CHILD_SP    0x7F00u

static muzix_kernel_proc_table_t g_table;
static muzix_mproc_t g_mp[2];
static process_map_t g_kmap = {{MUZIX_ZETA_KERNEL_BANK_0,
                                      MUZIX_ZETA_KERNEL_BANK_1,
                                      MUZIX_ZETA_KERNEL_BANK_2,
                                      MUZIX_ZETA_KERNEL_BANK_3}};
static process_map_t g_maps[2];

/* Four different values, one per window.  Window 0 and window 3 are the
 * kernel's own and every process's map carries them; the other two are the
 * process's.  A switch that published two of the four, or published the
 * outgoing process's, is caught by having no pair of bytes left that matches. */
static void set_map(process_map_t *map, uint8_t w1, uint8_t w2)
{
    map->pages[0] = MUZIX_ZETA_KERNEL_BANK_0;
    map->pages[1] = w1;
    map->pages[2] = w2;
    map->pages[3] = MUZIX_ZETA_KERNEL_BANK_3;
}

static void setup(void)
{
    muzix_proc_table_init(&g_table, &g_kmap);
    set_map(&g_maps[PARENT_SLOT], 0x30u, 0x32u);
    set_map(&g_maps[CHILD_SLOT], 0x31u, 0x33u);
    muzix_mproc_init(&g_mp[0], 0x0000u, 0x1000u, 0x2000u, 0x4000u, 0x4000u,
                     0x4000u, PARENT_PID);
    muzix_mproc_init(&g_mp[1], 0x0000u, 0x1000u, 0x2000u, 0x4000u, 0x4000u,
                     0x4000u, CHILD_PID);
    muzix_proc_table_register(&g_table, PARENT_SLOT, PARENT_PID, &g_mp[0],
                              &g_maps[PARENT_SLOT], MUZIX_PROC_USER_Q,
                              MUZIX_PROC_FREE);
    muzix_proc_table_register(&g_table, CHILD_SLOT, CHILD_PID, &g_mp[1],
                              &g_maps[CHILD_SLOT], MUZIX_PROC_USER_Q,
                              MUZIX_PROC_FREE);
    g_table.current = PARENT_SLOT;

    /* The cells as the trap leaves them: nothing published, nothing armed. */
    muzix_resume_pc = 0;
    muzix_resume_sp = 0;
    muzix_resume_ret = 0;
    muzix_switch_pending = 0;
    memset(muzix_resume_banks, 0xEE, sizeof(muzix_resume_banks));
}

/* ============================== case 1: a switch arms its target ============ */

/*
 * muzix_context_switch_to() is a request, not a switch.  The caller keeps
 * running until the trap it is inside returns; what the function owes the
 * assembly is that the four cells it is about to read hold the target's, and
 * that the flag says so.
 */
static int test_a_switch_installs_the_target_and_publishes_its_frame(void)
{
    uint8_t i;

    muzix_proc_table_set_resume(&g_table, PARENT_SLOT, PARENT_PC, PARENT_SP, 0);
    CHECK(muzix_context_switch_to(&g_table, PARENT_SLOT) == 0, 1);

    CHECK(muzix_resume_pc == PARENT_PC, 2);
    CHECK(muzix_resume_sp == PARENT_SP, 3);
    CHECK(muzix_switch_pending == 1, 4);
    for (i = 0; i < 4; i++) {
        CHECK(muzix_resume_banks[i] == g_maps[PARENT_SLOT].pages[i], 5);
    }

    /* All four, not the process's two.  Window 0 holds the boot stub and the
     * syscall vector and window 3 holds _DATA and the C stack; a switch that
     * published only what the process owned would carry the boot stub into
     * whichever page the process's data happened to be. */
    CHECK(muzix_resume_banks[0] == MUZIX_ZETA_KERNEL_BANK_0, 6);
    CHECK(muzix_resume_banks[3] == MUZIX_ZETA_KERNEL_BANK_3, 7);
    CHECK(muzix_resume_banks[1] == 0x30u && muzix_resume_banks[2] == 0x32u, 8);

    /* A second switch publishes the second process's map, not a blend of the
     * two and not the outgoing one's.  Every byte differs between the two maps
     * except the two kernel windows, so this cannot pass by accident. */
    muzix_proc_table_set_resume(&g_table, CHILD_SLOT, CHILD_PC, CHILD_SP, 0);
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 9);
    for (i = 0; i < 4; i++) {
        CHECK(muzix_resume_banks[i] == g_maps[CHILD_SLOT].pages[i], 1);
    }
    CHECK(muzix_resume_pc == CHILD_PC && muzix_resume_sp == CHILD_SP, 2);

    /* Switching is not saving: the target's recorded frame is not consumed, so
     * a process that is switched away from and back to resumes where it was. */
    CHECK(g_table.slots[CHILD_SLOT].resume_pc == CHILD_PC &&
          g_table.slots[CHILD_SLOT].resume_sp == CHILD_SP &&
          g_table.slots[CHILD_SLOT].resume_valid == 1, 3);
    CHECK(muzix_context_switch_to(&g_table, PARENT_SLOT) == 0, 4);
    CHECK(muzix_resume_pc == PARENT_PC && muzix_resume_sp == PARENT_SP, 5);

    /* And the switch does not reach into the slot for anything else: a resume
     * point recorded by one call is still the same resume point after a switch
     * that published it. */
    CHECK(g_table.slots[PARENT_SLOT].resume_pc == PARENT_PC &&
          g_table.slots[PARENT_SLOT].resume_sp == PARENT_SP, 6);
    return 0;
}

/* ============================== case 2: current follows ==================== */

/*
 * The table's idea of who is running has to move with the CPU.  The failure this
 * pins is not hypothetical: the child of a fork runs exec() immediately, and
 * with the parent's value still in place the exec loaded the new program into
 * the *parent's* slot, so `ls` reloaded the shell and the parent was
 * overwritten.
 */
static int test_current_follows_the_switch(void)
{
    uint8_t i;

    muzix_proc_table_set_resume(&g_table, PARENT_SLOT, PARENT_PC, PARENT_SP, 0);
    muzix_proc_table_set_resume(&g_table, CHILD_SLOT, CHILD_PC, CHILD_SP, 0);

    CHECK(g_table.current == PARENT_SLOT, 1);
    CHECK(muzix_proc_table_current_pid(&g_table) == PARENT_PID, 2);

    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 3);
    CHECK(g_table.current == CHILD_SLOT, 4);
    CHECK(muzix_proc_table_current_pid(&g_table) == CHILD_PID, 5);
    /* The slot the switch named is the slot it published, not the other way
     * round: the two are separately recorded and a change to one that did not
     * carry the other is the shape of the bug above. */
    CHECK(muzix_resume_pc == g_table.slots[g_table.current].resume_pc, 6);
    CHECK(muzix_resume_sp == g_table.slots[g_table.current].resume_sp, 7);
    for (i = 0; i < 4; i++) {
        CHECK(muzix_resume_banks[i] ==
              g_table.slots[g_table.current].map.pages[i], 8);
    }

    /* And back, more than once, because a switch that only worked in one
     * direction would still leave a scheduler with one stale answer. */
    CHECK(muzix_context_switch_to(&g_table, PARENT_SLOT) == 0, 9);
    CHECK(g_table.current == PARENT_SLOT &&
          muzix_proc_table_current_pid(&g_table) == PARENT_PID, 1);
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 2);
    CHECK(g_table.current == CHILD_SLOT &&
          muzix_proc_table_current_pid(&g_table) == CHILD_PID, 3);
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 4);
    CHECK(g_table.current == CHILD_SLOT &&
          muzix_proc_table_current_pid(&g_table) == CHILD_PID, 5);

    /* The switch does not touch the table's own kernel map.  That is not the
     * process's: the kernel runs out of its own pages, and a switch that
     * replaced it would move the kernel with the process. */
    CHECK(g_table.kernel_map.pages[0] == MUZIX_ZETA_KERNEL_BANK_0 &&
          g_table.kernel_map.pages[1] == MUZIX_ZETA_KERNEL_BANK_1 &&
          g_table.kernel_map.pages[2] == MUZIX_ZETA_KERNEL_BANK_2 &&
          g_table.kernel_map.pages[3] == MUZIX_ZETA_KERNEL_BANK_3, 6);
    return 0;
}

/* ============================== case 3: the outgoing frame ================ */

/*
 * The value each process gets back from the syscall it is sitting in.  This is
 * fork: the parent returns the child's pid and the child returns zero, one bit
 * apart, and a process that came back with the wrong one believes it is a
 * different process from the one that forked it - which is the "stored a slot
 * index in a pid field" defect, and which no return value of
 * muzix_context_switch_to() could report because the switch returns 0 either way.
 *
 * The two halves are called with two different values on purpose, and CHILD_PID
 * is 7 while both slots are 0 and 1, so a value that came from a slot index
 * rather than from the caller is not equal to either.
 *
 * Where the *choice* of zero is made is kernel/syscalls.c, and
 * kernel/test_syscalls.c pins the recorded side of it there.  What is pinned
 * here is that whatever the caller tags survives the record and is what the
 * switch publishes, for both processes, which is the half nothing checked.
 */
static int test_the_outgoing_frame_carries_the_value_its_syscall_will_return(void)
{
    uint16_t pc = 0;
    uint16_t sp = 0;
    uint16_t ret = 0xFFFFu;

    /* The trap captured where the parent was on the way in, while its own page
     * was still mapped.  That is the only moment those two cells can be right. */
    muzix_resume_pc = PARENT_PC;
    muzix_resume_sp = PARENT_SP;
    CHECK(muzix_context_save_current(&g_table, PARENT_SLOT, CHILD_PID) == 0, 1);
    CHECK(g_table.slots[PARENT_SLOT].resume_pc == PARENT_PC, 2);
    CHECK(g_table.slots[PARENT_SLOT].resume_sp == PARENT_SP, 3);
    CHECK(g_table.slots[PARENT_SLOT].resume_ret == CHILD_PID, 4);
    CHECK(g_table.slots[PARENT_SLOT].resume_valid == 1, 5);

    /* The child is saved with its own captured frame and with zero. */
    muzix_resume_pc = CHILD_PC;
    muzix_resume_sp = CHILD_SP;
    CHECK(muzix_context_save_current(&g_table, CHILD_SLOT, 0) == 0, 6);
    CHECK(g_table.slots[CHILD_SLOT].resume_ret == 0, 7);
    CHECK(g_table.slots[CHILD_SLOT].resume_pc == CHILD_PC &&
          g_table.slots[CHILD_SLOT].resume_sp == CHILD_SP, 8);

    /* Saving one process does not touch the other's row.  The cells above are
     * the trap's, and they are overwritten by every call, so a second save that
     * landed on the wrong slot would leave both processes resuming at
     * whichever frame was captured last. */
    CHECK(g_table.slots[PARENT_SLOT].resume_pc == PARENT_PC &&
          g_table.slots[PARENT_SLOT].resume_sp == PARENT_SP, 9);

    /* The child is what the CPU goes on to after the fork, so the first switch
     * publishes zero. */
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 1);
    CHECK(muzix_resume_ret == 0, 2);
    CHECK(muzix_resume_ret != (uint16_t)CHILD_SLOT, 3);
    CHECK(muzix_resume_pc == CHILD_PC && muzix_resume_sp == CHILD_SP, 4);

    /* And the parent, when it is next resumed, gets the child pid - which is
     * also the return value of the fork() it is sitting in. */
    CHECK(muzix_context_switch_to(&g_table, PARENT_SLOT) == 0, 5);
    CHECK(muzix_resume_ret == CHILD_PID, 6);
    CHECK(muzix_resume_ret != (uint16_t)PARENT_SLOT, 7);
    CHECK(muzix_resume_pc == PARENT_PC && muzix_resume_sp == PARENT_SP, 8);

    /* What the switch publishes came out of the table, not out of the cells
     * the trap happened to leave behind: clearing them changes nothing. */
    muzix_resume_pc = 0xFFFFu;
    muzix_resume_sp = 0xFFFFu;
    muzix_resume_ret = 0xFFFFu;
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 9);
    CHECK(muzix_resume_pc == CHILD_PC && muzix_resume_sp == CHILD_SP &&
          muzix_resume_ret == 0, 1);

    /* A zero-length frame is still a frame.  pc and sp are read as given and
     * nothing is validated against them, which is right here: the trap is the
     * only thing that can measure them and it has already said they are right.
     * Asserted so that a future "refuse a zero stack pointer" is a deliberate
     * change to this test rather than a silent one. */
    muzix_resume_pc = 0;
    muzix_resume_sp = 0;
    CHECK(muzix_context_save_current(&g_table, CHILD_SLOT, 0) == 0, 2);
    CHECK(muzix_proc_table_get_resume(&g_table, CHILD_SLOT, &pc, &sp, &ret) == 1, 3);
    CHECK(pc == 0 && sp == 0 && ret == 0, 4);
    return 0;
}

/* ============================== case 4: no resume point, no switch ======== */

/*
 * A slot that has never been recorded cannot be resumed: its program counter is
 * whatever the table was zeroed to, and jumping there lands the machine in the
 * boot stub at 0x0000.  The case this is about is not a hand-built one - it is
 * the child of a fork, which proc_table_fork() deliberately leaves with no
 * resume point, because the whole slot was copied verbatim from the parent and
 * the child must not resume at the instruction its parent was executing.
 */
static int test_a_slot_with_no_recorded_resume_point_is_refused(void)
{
    uint8_t banks[4];
    uint16_t pc;
    uint16_t sp;
    int child_slot;

    CHECK(muzix_proc_table_fork(&g_table, PARENT_SLOT, 2, 9) == 0, 1);
    child_slot = 2;
    CHECK(g_table.slots[child_slot].active && g_table.slots[child_slot].pid == 9,
          2);
    CHECK(g_table.slots[child_slot].resume_valid == 0, 3);

    /* Establish something for a refusal to spoil. */
    muzix_proc_table_set_resume(&g_table, PARENT_SLOT, PARENT_PC, PARENT_SP, 5);
    CHECK(muzix_context_switch_to(&g_table, PARENT_SLOT) == 0, 4);
    memcpy(banks, muzix_resume_banks, sizeof(banks));
    pc = muzix_resume_pc;
    sp = muzix_resume_sp;
    muzix_switch_pending = 0;             /* what the trap does before acting */

    /* The child is active and has a map, and is still not resumable. */
    CHECK(muzix_context_switch_to(&g_table, child_slot) == -1, 5);

    CHECK(muzix_switch_pending == 0, 6);
    CHECK(g_table.current == PARENT_SLOT, 7);
    CHECK(muzix_resume_pc == pc && muzix_resume_sp == sp, 8);
    CHECK(muzix_resume_ret == 5, 9);
    CHECK(memcmp(muzix_resume_banks, banks, sizeof(banks)) == 0, 1);

    /* Give it a frame and it becomes resumable, which is what the fork handler
     * does before the trap picks the switch up.  So the refusal above was about
     * the missing frame and not about the slot. */
    CHECK(muzix_context_switch_to(&g_table, child_slot) == -1, 2);
    muzix_resume_pc = CHILD_PC;
    muzix_resume_sp = CHILD_SP;
    CHECK(muzix_context_save_current(&g_table, child_slot, 0) == 0, 3);
    CHECK(muzix_context_switch_to(&g_table, child_slot) == 0, 4);
    CHECK(muzix_resume_pc == CHILD_PC && muzix_resume_sp == CHILD_SP, 5);
    CHECK(g_table.current == child_slot, 6);
    return 0;
}

/* ============================== case 5: a refused switch ================== */

/*
 * Every refusal has to leave the published cells exactly as they were, and the
 * flag exactly as it was.  Two reasons, and the second is the one that bites:
 *
 *   1. The cells are what the trap reads.  Publishing a target's frame and then
 *      refusing would leave the trap resuming the wrong process.
 *   2. The flag is cleared by the trap *before* it acts on it, and a switch that
 *      was refused must not raise it again - otherwise every syscall that
 *      follows re-attempts the same impossible switch, and the one process in
 *      the table that could still run never gets to.
 *
 * So this case establishes a real switch first, so that there is a previous map
 * installed for a refusal to spoil, and then refuses the switch every way it can
 * be refused.
 */
static int refused_leaves_everything_alone(const uint8_t *banks, int slot)
{
    int i;

    if (muzix_switch_pending != 0) {
        return 0;
    }
    if (g_table.current != slot) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (muzix_resume_banks[i] != banks[i]) {
            return 0;
        }
    }
    if (muzix_resume_pc != g_table.slots[slot].resume_pc ||
        muzix_resume_sp != g_table.slots[slot].resume_sp ||
        muzix_resume_ret != g_table.slots[slot].resume_ret) {
        return 0;
    }
    return 1;
}

static int test_a_refused_switch_leaves_the_previous_map_installed(void)
{
    uint8_t banks[4];
    int inactive_slot = 3;

    muzix_proc_table_set_resume(&g_table, PARENT_SLOT, PARENT_PC, PARENT_SP, 0);
    muzix_proc_table_set_resume(&g_table, CHILD_SLOT, CHILD_PC, CHILD_SP, 0);
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 1);
    memcpy(banks, muzix_resume_banks, sizeof(banks));

    /* Every refusal below happens with the flag clear, which is the state the
     * trap leaves it in, and each has to leave it clear. */
    muzix_switch_pending = 0;

    /* No table at all. */
    CHECK(muzix_context_switch_to(0, PARENT_SLOT) == -1, 2);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 3);

    /* Below the table, and above it. */
    CHECK(muzix_context_switch_to(&g_table, -1) == -1, 4);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 5);
    CHECK(muzix_context_switch_to(&g_table, MUZIX_PROC_TABLE_MAX) == -1, 6);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 7);
    CHECK(muzix_context_switch_to(&g_table, 1000) == -1, 8);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 9);

    /* A slot that is not in use.  The table is only zeroed here, so this one has
     * a map and a resume point that simply do not belong to anybody. */
    g_table.slots[inactive_slot].active = 0;
    g_table.slots[inactive_slot].resume_valid = 1;
    g_table.slots[inactive_slot].resume_pc = 0xDEADu;
    g_table.slots[inactive_slot].resume_sp = 0xBEEFu;
    set_map(&g_table.slots[inactive_slot].map, 0x3Eu, 0x3Fu);
    CHECK(muzix_context_switch_to(&g_table, inactive_slot) == -1, 1);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 2);
    /* The zeroed rows are not switchable either, which is what stops a stale
     * index from resuming a process that has left. */
    CHECK(muzix_context_switch_to(&g_table, 2) == -1, 3);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 4);

    /* And a refusal after a refusal is still a refusal, not a switch that has
     * been armed by accumulation. */
    CHECK(muzix_context_switch_to(&g_table, 2) == -1, 5);
    CHECK(muzix_context_switch_to(&g_table, -1) == -1, 6);
    CHECK(refused_leaves_everything_alone(banks, CHILD_SLOT), 7);

    /* A switch that is armed still works afterwards, so none of the above left
     * the table in a state that only refusals can reach. */
    CHECK(muzix_context_switch_to(&g_table, PARENT_SLOT) == 0, 8);
    CHECK(muzix_switch_pending == 1 && g_table.current == PARENT_SLOT, 9);

    /* A successful switch does not clear the flag itself: the trap does that,
     * immediately before it acts, and a switch that cleared it on the way out
     * would leave the trap returning normally and the switch never performed. */
    muzix_switch_pending = 0;
    CHECK(muzix_context_switch_to(&g_table, CHILD_SLOT) == 0, 1);
    CHECK(muzix_switch_pending == 1, 2);
    return 0;
}

/* ============================== case 6: the save's own refusals ============ */

static int test_save_current_refuses_what_it_cannot_record(void)
{
    uint16_t pc = 0;
    uint16_t sp = 0;
    uint16_t ret = 0;

    CHECK(muzix_context_save_current(0, PARENT_SLOT, 0) == -1, 1);
    CHECK(muzix_context_save_current(&g_table, -1, 0) == -1, 2);
    CHECK(muzix_context_save_current(&g_table, MUZIX_PROC_TABLE_MAX, 0) == -1, 3);
    CHECK(muzix_context_save_current(&g_table, 1000, 0) == -1, 4);

    /* None of them recorded anything, on any row. */
    CHECK(muzix_proc_table_get_resume(&g_table, PARENT_SLOT, &pc, &sp, &ret) == 0,
          5);
    CHECK(muzix_proc_table_get_resume(&g_table, CHILD_SLOT, &pc, &sp, &ret) == 0,
          6);
    CHECK(muzix_resume_pc == 0 && muzix_resume_sp == 0, 7);

    /* And it does not arm the trap.  The save is bookkeeping; only the switch
     * is a request. */
    CHECK(muzix_switch_pending == 0, 8);

    /* Saving does not need the slot to be active - the trap saves the running
     * process, which is active by definition, but the refusal list above is
     * about the index and the table and not about the slot, and a case that
     * quietly refused here would look identical to one that saved. */
    g_table.slots[3].active = 0;
    muzix_resume_pc = 0x1234u;
    muzix_resume_sp = 0x5678u;
    CHECK(muzix_context_save_current(&g_table, 3, 0) == 0, 9);
    CHECK(muzix_proc_table_get_resume(&g_table, 3, &pc, &sp, &ret) == 1, 1);
    CHECK(pc == 0x1234u && sp == 0x5678u && ret == 0, 2);
    /* Still not armed: a saved frame is not a switch. */
    CHECK(muzix_switch_pending == 0, 3);
    return 0;
}

int main(void)
{
    static int (*const cases[])(void) = {
        test_a_switch_installs_the_target_and_publishes_its_frame,
        test_current_follows_the_switch,
        test_the_outgoing_frame_carries_the_value_its_syscall_will_return,
        test_a_slot_with_no_recorded_resume_point_is_refused,
        test_a_refused_switch_leaves_the_previous_map_installed,
        test_save_current_refuses_what_it_cannot_record
    };
    unsigned i;
    int rc;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup();
        rc = cases[i]();
        if (rc != 0) {
            /* case number * 10 + what went wrong, so the exit status names it */
            return (int)(i + 1) * 10 + rc;
        }
    }
    return 0;
}
