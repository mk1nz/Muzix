/* * * Kernel scheduler loop header: loop state machine, process
 * table integration, and the scheduler entry points.
 */

#ifndef MUZIX_KERNEL_LOOP_H
#define MUZIX_KERNEL_LOOP_H

#include "ztypes.h"

#include "system_entry.h"
#include "../platform/zeta-v2/test_kernel/bank_model.h"

struct muzix_mm_service;
struct muzix_zeta_syscall_runtime;
struct muzix_fs_service;

/* Kernel loop state: idle (nothing runnable). */
#define MUZIX_KERNEL_LOOP_IDLE 0
/* Kernel loop state: running (a userspace process is active). */
#define MUZIX_KERNEL_LOOP_RUNNING 1
/* Kernel loop state: shutdown. */
#define MUZIX_KERNEL_LOOP_SHUTDOWN 2

/* * Kernel loop state machine.
 */
typedef struct muzix_kernel_loop {
 muzix_system_entry_t system; /*< System entry sub-struct */
 uint8_t state; /*< Loop state (IDLE/RUNNING/SHUTDOWN) */
 uint32_t tick; /*< Loop tick counter */
 int current_pid; /*< Current process id */
 struct muzix_mm_service *mm; /*< MM service link */
 uint8_t in_boot_phase; /*< 1 while booting */
} muzix_kernel_loop_t;

/* * Initialise the kernel loop.
 * \param[in,out] loop: loop to initialise
 * kernel_map: the kernel's own bank map
 * init_pid: initial process id
 * init_map: initial process bank map
 */
void muzix_kernel_loop_init(muzix_kernel_loop_t *loop,
 const process_map_t *kernel_map,
 uint8_t init_pid,
 const process_map_t *init_map);

/* * Register a slot in the loop's process table.
 * \param[in,out] loop: kernel loop
 * slot: slot number
 * pid: process id
 * mp: memory model
 * map: bank map
 * q: ready queue (TASK/SERVER/USER)
 * flags: slot flags
 */
void muzix_kernel_loop_register(muzix_kernel_loop_t *loop,
 int slot,
 uint8_t pid,
 const muzix_mproc_t *mp,
 const process_map_t *map,
 uint8_t q,
 uint8_t flags);

/* * The permanent idle process - it NEVER returns. Runs the
 * scheduler loop forever, entering userspace processes and
 * halting when nothing is runnable.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_loop_run(muzix_kernel_loop_t *loop);

/* * Give up the processor and never come back: wake whatever is
 * due, stop the CPU if nothing is runnable, and switch to
 * whatever is.
 * * The point of it being unreturning is that a caller cannot do anything
 * sensible afterwards - it has just made itself unrunnable, and returning to
 * it would run the process once more and then strand it.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_switchout(muzix_kernel_loop_t *loop);

/* * Yield to the next ready process.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_loop_yield(muzix_kernel_loop_t *loop);

/* * Switch from userspace to kernel context.
 * \param[in,out] loop: kernel loop
 */
void muzix_kernel_loop_switch_to_kernel(muzix_kernel_loop_t *loop);

/* * Return the current process's pid.
 * loop: kernel loop
 * Current process id, or -1 if none
 */
int muzix_kernel_loop_current_pid(const muzix_kernel_loop_t *loop);

/* * Bind syscalls: initialise the syscall runtime.
 * \param[in,out] loop: kernel loop
 * \param[in,out] runtime: syscall runtime
 * fs: filesystem service
 */
void muzix_kernel_loop_bind_syscalls(
 muzix_kernel_loop_t *loop,
 struct muzix_zeta_syscall_runtime *runtime,
 struct muzix_fs_service *fs);

#endif /* MUZIX_KERNEL_LOOP_H */
