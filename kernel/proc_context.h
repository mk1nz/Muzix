#ifndef MUZIX_PROC_CONTEXT_H
#define MUZIX_PROC_CONTEXT_H

#include <stdint.h>

#include "../kernel/proc_table.h"

/* Arm a context switch to `slot`.
 *
 * Publishes the target's resume point and map and raises the flag the syscall
 * trap's exit path tests. The switch itself is then performed by window-0
 * assembly, which is the only code that survives having its own window remapped
 * underneath it. Returns 0 if the switch was armed, -1 if the slot cannot be
 * resumed - which includes a slot that has never had a resume point recorded.
 *
 * This is a request, not a switch: the caller keeps running until the trap it
 * is inside returns. */
int muzix_context_switch_to(muzix_kernel_proc_table_t *table, int slot);

/* Record the running process's resume point on the slot, tagged with `retval`.
 *
 * The trap has already captured the program counter and stack pointer on the
 * way in, while the process's own stack was still mapped; this copies them onto
 * the slot so a later switch can find them. `retval` is what the syscall the
 * process is inside is going to return, which is what makes fork work: the
 * parent records the child pid and the child records zero, and each gets its own
 * value back when it is next resumed. */
int muzix_context_save_current(muzix_kernel_proc_table_t *table,
                               int slot,
                               uint16_t retval);

#endif
