#ifndef MUZIX_EXEC_LOADER_H
#define MUZIX_EXEC_LOADER_H

#include <stdint.h>

#include "../mm/mm_service.h"
#include "../platform/zeta-v2/rtc_ds1302.h"

struct muzix_fs_service;

/* Global arguments for muzix_exec_load - avoids calling convention issues */
typedef struct {
    struct muzix_fs_service *fs;
    muzix_mm_service_t *mm;
    void *processes;
    int slot;
    const char *path;
    const char *const *argv;
} muzix_exec_load_args_t;

extern muzix_exec_load_args_t g_muzix_exec_load_args;

/* Load a userspace binary from the filesystem into the given process slot.
 * Returns 0 on success, -1 on failure. */
int muzix_exec_load(void);

/* The form the syscall layer calls: same load, but the target slot is an
 * argument rather than whatever boot left in the global block. This is what
 * userspace.exec_loader is set to; see the comment above the definition. */
int muzix_exec_load_for_slot(const char *path, const char *const *argv,
                             int slot);

/*
 * The clock reading taken at the first exec after power-on, and the state of
 * that attempt.
 *
 * The DS1302 supplies calendar time; userspace converts its two-digit year to
 * seconds since the project's 2000-01-01 epoch. The kernel keeps the RTC
 * reading from boot so uptime can subtract it from a later reading. Scheduler
 * ticks are used for deadlines and process accounting, not as the wall-clock
 * source.
 *
 * Why the loader holds it, and not a syscall handler: the kernel reaches
 * userspace through muzix_kernel_loop_run(), which enters the first process
 * with muzix_zeta_enter_userspace() and no syscall on the way.  A baseline
 * recorded in the SYS_GETRTC handler would therefore never be taken for the
 * boot process at all, and the shell - the one program always on screen - would
 * have nothing to subtract.  This is the same trap as the process name two
 * lines above muzix_exec_load()'s use of these, and it is here for the same
 * reason: muzix_exec_load() is the one function every exec goes through, and
 * the one the kernel calls directly to start the shell.
 *
 * The state is three-valued rather than a flag because "not tried yet" and
 * "tried and the chip refused" must not be re-tried.  A second exec is not a
 * second boot, and quietly taking a fresh baseline on the first `ls` a user
 * runs an hour after power-on would report an hour of uptime for a machine
 * that has been up since morning.  So the attempt is made once and never
 * repeated, and a machine whose clock did not answer at boot reports that it
 * has no baseline rather than inventing one.
 */
#define MUZIX_BOOT_RTC_UNTRIED 0u
#define MUZIX_BOOT_RTC_GOOD    1u
#define MUZIX_BOOT_RTC_REFUSED 2u

extern muzix_rtc_time_t muzix_boot_rtc;
extern uint8_t muzix_boot_rtc_state;

#endif