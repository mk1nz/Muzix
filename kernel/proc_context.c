#include "../platform/zeta-v2/context_switch.h"

#include <string.h>

#include "../kernel/proc_table.h"
#include "../mm/mm_service.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/syscall_entry.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/uart_io.h"

/* Published to window-0 assembly, which reads them immediately before the map
 * changes.  Defined in kernel_main.c alongside the entry-point pair that
 * muzix_zeta_enter_userspace() already uses. */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;
extern uint16_t muzix_resume_ret;
extern uint8_t muzix_resume_banks[4];
extern uint16_t muzix_switch_pending;

int muzix_context_switch_to(muzix_kernel_proc_table_t *table, int slot)
{
    muzix_proc_slot_t *target;
    uint16_t pc = 0;
    uint16_t sp = 0;
    uint16_t retval = 0;

    if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }

    target = &table->slots[slot];
    if (!target->active) {
        return -1;
    }

    /* A slot that has never been recorded cannot be resumed: its program counter
     * would be whatever the table was zeroed to, and jumping there lands the
     * machine in the boot stub.  Refuse instead. */
    if (!muzix_proc_table_get_resume(table, slot, &pc, &sp, &retval)) {
        return -1;
    }

    /* Re-stamp before the jump, so the resumed process's next charge starts HERE
     * rather than where it left.  Together with the charge at the park side -
     * which books up to the park and re-stamps - this makes charged time and
     * parked time two disjoint halves of the interval between two calls, and the
     * parked one is left out.  Without this half the interval would be charged to
     * the call after the resume, and a process that slept for an hour would look
     * like one that worked for an hour.
     *
     * It has to be here: muzix_zeta_resume_from() does not come back, so there
     * is no "after" to put it in. */
    muzix_tick_stamp_store(&target->tick_stamp);

    muzix_resume_pc = pc;
    muzix_resume_sp = sp;
    muzix_resume_ret = retval;
    muzix_resume_banks[0] = target->map.pages[0];
    muzix_resume_banks[1] = target->map.pages[1];
    muzix_resume_banks[2] = target->map.pages[2];
    muzix_resume_banks[3] = target->map.pages[3];

    /* The table's idea of "who is running" has to move with the CPU, or the
     * next syscall is attributed to whoever used to be running. That is not
     * hypothetical: the child of a fork runs exec() immediately, and with the
     * old value still in place the exec loaded the new program into the
     * *parent's* slot, so `ls` reloaded the shell and the parent got
     * overwritten. The table is the single source of truth for this, which is
     * what the scheduler consolidation was for. */
    table->current = slot;

    /* Set last. The trap tests this before acting on it, so a switch must not
     * be visible until everything it needs is already published. */
    muzix_switch_pending = 1;
    return 0;
}

int muzix_context_save_current(muzix_kernel_proc_table_t *table,
                               int slot,
                               uint16_t retval)
{
    uint16_t pc = 0;
    uint16_t sp = 0;

    if (!table || slot < 0 || slot >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }

    /* The trap has already captured the outgoing process's resume point into
     * these two cells on the way in - while window 1 still held the process's
     * own page, which is the only moment it can be read. Copy them onto the slot
     * now, tagged with the value this syscall is about to return. */
    pc = muzix_resume_pc;
    sp = muzix_resume_sp;
    muzix_proc_table_set_resume(table, slot, pc, sp, retval);
    return 0;
}
