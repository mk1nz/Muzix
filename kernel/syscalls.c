#include "syscalls.h"
#include "exec_loader.h"
#include <string.h>

#include "../fs/fs_service.h"
#include "proc_context.h"
#include "../platform/zeta-v2/context_switch.h"
#include "../platform/zeta-v2/rtc_ds1302.h"
#include "../fs/tty_device.h"
#include "../mm/mm_service.h"
#include "../mm/mm_copy.h"
#include "load.h"
/* muzix_trace_stack_mark() (syscalls.c:259) and muzix_zeta_enter_userspace()
 * (syscalls.c:866) are called here and were never declared in this file.  SDCC
 * accepts an implicit declaration, so it compiled and linked; a host compiler
 * does not, which made the whole of syscalls.c unbuildable off-target and
 * untestable.  exec_loader.c, which calls both of the same functions, already
 * includes these two headers. */
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/uart_io.h"

/* Resolve the running process to its SLOT index.
 *
 * The MM copy handlers take a slot, not a pid: they index the process table
 * with it and bound it against MUZIX_PROC_TABLE_MAX.  The two only coincide
 * today because boot happens to assign slot 1 -> pid 1; passing a pid there
 * silently reads and writes another process's memory as soon as a pid >= 4
 * exists.  Every caller of a copy helper has to go through this. */
/* The one place the process table is reached from, so there is one answer to
 * whether it can be reached at all.
 *
 * There are two spellings in this file and only one of them worked.
 * `ctx->kernel->process_table` is a real field of muzix_syscall_context_t and
 * SYS_LOAD used to read it - but platform/zeta-v2/syscall_runtime.c, which is
 * the only producer of that context, sets `kernel.loop` and `kernel.mm` and
 * never `kernel.process_table`, so the field was always NULL in the running
 * system.  Nothing else read it, so nothing else broke; SYS_LOAD did, and
 * muzix_load_sample() takes a NULL table as "nothing to do" and returns, so the
 * display sat at 0.00 while the counters it was reading charged 100% of
 * elapsed time.
 *
 * `ctx->kernel->loop->system.startup.proc_table` is the same object and the
 * table the scheduler and the context switch maintain.  muzix_system_task_
 * handle_syscall() passes the same address into a kernel context of its own,
 * which is why SYS_COPY was never affected. */
static muzix_kernel_proc_table_t *muzix_proc_table(
    muzix_userspace_syscall_context_t *ctx)
{
    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return 0;
    }
    return &ctx->kernel->loop->system.startup.proc_table;
}

static int muzix_current_slot(muzix_userspace_syscall_context_t *ctx)
{
    muzix_kernel_proc_table_t *table;
    int current_pid;
    int slot;

    table = muzix_proc_table(ctx);
    if (!table) {
        return -1;
    }

    /* The table's `current` is the answer, and it is maintained by both the
     * scheduler and the context switch, so it moves with the CPU. Going via
     * muzix_kernel_loop_current_pid() instead read a pid cached once at boot
     * and never updated, so every syscall after a context switch was attributed
     * to the old process - including the fork's exec, which loaded the new
     * program into the parent's slot.
     *
     * The pid match is kept as a fallback for the window between a process
     * becoming current and the table's `current` agreeing, which is how the
     * first process at boot is picked up. */
    slot = table->current;
    if (slot >= 0 && slot < MUZIX_PROC_TABLE_MAX &&
        table->slots[slot].active) {
        return slot;
    }

    current_pid = muzix_kernel_loop_current_pid(ctx->kernel->loop);
    if (current_pid < 0) {
        return -1;
    }
    for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
        if (table->slots[slot].active &&
            table->slots[slot].pid == (uint8_t)current_pid) {
            return slot;
        }
    }
    return -1;
}

static muzix_proc_slot_t *muzix_current_process(
    muzix_userspace_syscall_context_t *ctx)
{
    muzix_kernel_proc_table_t *table = muzix_proc_table(ctx);
    int slot = muzix_current_slot(ctx);

    if (!table || slot < 0) {
        return 0;
    }
    return &table->slots[slot];
}

static int muzix_copy_user_from_current(
    muzix_userspace_syscall_context_t *ctx,
    uint16_t address,
    uint8_t *buffer,
    size_t length)
{
    int slot = muzix_current_slot(ctx);
    muzix_mm_service_t *mm;
    if (slot < 0 || !ctx->kernel || !ctx->kernel->loop || !buffer) {
        return -1;
    }
    mm = ctx->kernel->loop->mm;
    if (!mm) {
        return -1;
    }
    return muzix_mm_copy_user_to_kernel(mm, (uint8_t)slot,
                                         address, buffer, (uint16_t)length);
}

static int muzix_copy_user_to_current(
    muzix_userspace_syscall_context_t *ctx,
    uint16_t address,
    const uint8_t *buffer,
    size_t length)
{
    int slot = muzix_current_slot(ctx);
    muzix_mm_service_t *mm;
    if (slot < 0 || !ctx->kernel || !ctx->kernel->loop || !buffer) {
        return -1;
    }
    mm = ctx->kernel->loop->mm;
    if (!mm) {
        return -1;
    }
    return muzix_mm_copy_kernel_to_user(mm, (uint8_t)slot,
                                         address, buffer, (uint16_t)length);
}

static muzix_proc_slot_t *muzix_slot_process(
    muzix_userspace_syscall_context_t *ctx,
    int32_t index)
{
    muzix_kernel_proc_table_t *table = muzix_proc_table(ctx);

    if (!table || index < 0 || index >= MUZIX_PROC_TABLE_MAX) {
        return 0;
    }
    return &table->slots[index];
}

int muzix_handle_syscall(muzix_syscall_context_t *ctx, const muzix_syscall_t *call)
{
    if (!ctx || !call) {
        return MUZIX_SYSCALL_FAIL;
    }

    switch (call->call) {
    case MUZIX_SYS_COPY:
        return muzix_sys_copy_dispatch(ctx, call);
    case MUZIX_SYS_YIELD:
        return muzix_sys_yield_dispatch(ctx, call);
    case MUZIX_SYS_EXIT:
        return muzix_sys_exit_dispatch(ctx, call);
    default:
        return MUZIX_SYSCALL_FAIL;
    }
}

int muzix_sys_copy_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call)
{
    const muzix_proc_slot_t *src_slot = 0;
    const muzix_proc_slot_t *dst_slot = 0;
    int slot;

    if (!ctx || !call || !call->copy_request || !ctx->mm ||
        !ctx->process_table) {
        return MUZIX_SYSCALL_FAIL;
    }

    for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
        const muzix_proc_slot_t *candidate =
            &ctx->process_table->slots[slot];
        if (!candidate->active) {
            continue;
        }
        if (candidate->pid == call->copy_request->src_proc) {
            src_slot = candidate;
        }
        if (candidate->pid == call->copy_request->dst_proc) {
            dst_slot = candidate;
        }
    }

    if (!src_slot || !dst_slot) {
        return MUZIX_SYSCALL_FAIL;
    }

    return muzix_mm_copy_maps(
               ctx->mm, &src_slot->mproc,
               call->copy_request->src_space, call->copy_request->src_vir,
               &dst_slot->mproc, call->copy_request->dst_space,
               call->copy_request->dst_vir, call->copy_request->bytes) ==
               MUZIX_COPY_OK
           ? MUZIX_SYSCALL_OK
           : MUZIX_SYSCALL_FAIL;
}

int muzix_sys_yield_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call)
{
    if (!ctx || !ctx->loop || !call) {
        return MUZIX_SYSCALL_FAIL;
    }
    muzix_kernel_loop_yield(ctx->loop);
    return MUZIX_SYSCALL_OK;
}

int muzix_sys_exit_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call)
{
    if (!ctx || !ctx->loop || !call) {
        return MUZIX_SYSCALL_FAIL;
    }
    muzix_kernel_loop_switch_to_kernel(ctx->loop);
    return MUZIX_SYSCALL_OK;
}

/* Copy a NUL-terminated path from user memory into a kernel buffer.
 *
 * Every filesystem entry point takes a `const char *path` and walks it itself.
 * Passing a user address straight through means the kernel dereferences it with
 * the KERNEL bank map installed: a path below 0x4000 reads the boot stub, a
 * path in windows 1-3 reads kernel code, and the filesystem then operates on
 * whatever it found. The bound is also what makes this safe - an unterminated
 * user string is a fault, not an unbounded read. */
static int muzix_copy_user_string(
    muzix_userspace_syscall_context_t *ctx,
    uint16_t address,
    char *buffer,
    size_t max)
{
    size_t n = 0;

    if (!buffer || max == 0) {
        return -1;
    }
    while (n < max - 1) {
        uint8_t c;

        if (muzix_copy_user_from_current(ctx, (uint16_t)(address + n), &c, 1) != 0) {
            return -1;
        }
        buffer[n++] = (char)c;
        if (c == 0) {
            return 0;
        }
    }
    buffer[max - 1] = 0;
    return -1;
}

/* The syscall body.  Everything is in muzix_handle_userspace_syscall() below,
 * which wraps this to bracket it with the two tick charges that make UTIME and
 * STIME real; the body has a dozen `return` statements, so a charge placed at
 * the end of it would have to be written twelve times and would be one missed
 * one away from silently not accounting for a whole class of syscalls. */
static int32_t muzix_dispatch_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3)
{
    /* Kernel-side landing pads. The filesystem entry points below all take a
     * char* and walk it themselves, and exec_loader dereferences both the path
     * and each argv element, so none of those pointers may be the user's. */
    char k_path[MUZIX_FS_NAME_MAX + 2];
    char k_path2[MUZIX_FS_NAME_MAX + 2];
    const char *k_argv[3];
    char k_argv_buf[2 * (MUZIX_FS_NAME_MAX + 1)];
    uint8_t k_ioctl[8];          /* muzix_tty_params_t is 4 bytes, chars is 6 */
    muzix_rtc_time_t rtc_time;
    muzix_syscall_t internal_call;
    muzix_fs_inode_t stat;
    uint16_t inode;
    size_t result;
    int status;
    muzix_kernel_proc_table_t *processes;
    int parent_slot = -1;
    int child_slot = -1;
    uint8_t child_pid = 1;
    int slot;

    if (!ctx) {
        return -1;
    }
    muzix_trace_stack_mark();

    if (syscall_num == MUZIX_USER_SYS_EXIT) {
        if (!ctx->kernel) {
            return -1;
        }
        processes = ctx->kernel->loop
                        ? &ctx->kernel->loop->system.startup.proc_table
                        : 0;
        if (!processes || ctx->pid < 0) {
            return -1;
        }
        for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
            if (processes->slots[slot].active &&
                processes->slots[slot].pid == (uint8_t)ctx->pid) {
                parent_slot = slot;
                break;
            }
        }
        if (slot == MUZIX_PROC_TABLE_MAX) {
            return -1;
        }

        if (muzix_proc_table_exit_status(
                processes, parent_slot, (uint8_t)ctx->ppid,
                (uint8_t)arg1) != 0) {
            return -1;
        }

        /* The exiting process's own pages go back to the allocator now that
         * the teardown has succeeded. If exit_status failed the slot is still
         * active and must keep its pages. */
        muzix_proc_table_release_pages(processes, parent_slot,
                                       ctx->kernel->loop->mm);

        /* Hand the CPU back to whoever forked this process.
         *
         * Its parent still holds the frame it was given by the fork - the
         * instruction after the call, with the child pid in DE - so resuming it
         * lands the parent back at `pid = fork()` with the pid it was told,
         * which is where it was when it went off to wait(). That is the other
         * half of what makes fork work, and without it the parent's wait() is
         * never satisfied: the child runs to completion and the shell simply
         * stops, with no output and no prompt.
         *
         * The process that called exit() is gone, so this call does not return
         * for it; the trap's exit path performs the switch.
         *
         * muzix_sys_exit_dispatch() is not used. It called
         * muzix_kernel_loop_switch_to_kernel(), which clears the loop's current
         * pid and puts the loop back in its IDLE state - correct for a system
         * with one process, wrong here, because it throws away the table's
         * `current` that the switch to the parent is about to set. */
        if (ctx->ppid >= 0) {
            int parent = -1;
            int probe;
            for (probe = 0; probe < MUZIX_PROC_TABLE_MAX; probe++) {
                if (processes->slots[probe].active &&
                    processes->slots[probe].pid == (uint8_t)ctx->ppid) {
                    parent = probe;
                    break;
                }
            }
            if (parent >= 0 &&
                muzix_context_switch_to(processes, parent) != 0) {
                /* The parent cannot be resumed - it has no recorded frame, or
                 * is gone too. Fall through to the old behaviour so the exit
                 * still completes rather than looping. */
                memset(&internal_call, 0, sizeof(internal_call));
                internal_call.call = MUZIX_SYS_EXIT;
                return muzix_sys_exit_dispatch(ctx->kernel, &internal_call) ==
                               MUZIX_SYSCALL_OK
                           ? 0
                           : -1;
            }
        }
        return 0;
    }

    if (syscall_num == MUZIX_USER_SYS_GETPID) {
        return ctx->pid;
    }
    if (syscall_num == MUZIX_USER_SYS_GETPPID) {
        return ctx->ppid;
    }
    /* SYS_TIME has no direct dispatch branch. The userspace time() wrapper
     * obtains DS1302 calendar fields through SYS_GETRTC and converts them to
     * seconds since 2000-01-01 in libc. SYS_GETRTC can also return the RTC
     * reading saved at boot, allowing uptime() to subtract the two readings
     * without duplicating calendar conversion in the kernel.
     *
     * SYS_GETRTC rather than a new call number, for three reasons.  The handler
     * is already here, and the kernel's dispatch is a chain of compares that a
     * new number would have to be threaded through.  The two ends of a
     * difference have to come from the same source, and returning them together
     * makes it impossible for a caller to pair a live reading with a stale
     * baseline by accident.  And there is no second time source to be confused
     * with, because the boot reading is not a clock: it is one reading of the
     * clock the caller already has in hand.
     *
     * There is no baseline if the chip refused at boot, and this returns -1
     * rather than handing over a struct full of a dead chip's bytes - a caller
     * that subtracted those would report a large and entirely fictional uptime,
     * and "no baseline" is a state it can be told about.
     */
    if (syscall_num == MUZIX_USER_SYS_GETRTC ||
        syscall_num == MUZIX_USER_SYS_TIME) {
        int32_t rtc_rc;

        if (!arg1) {
            return -1;
        }
        if (arg2 && muzix_boot_rtc_state != MUZIX_BOOT_RTC_GOOD) {
            return -1;
        }
        /* The struct is handed over whether or not the read was accepted.
         * muzix_zeta_rtc_get leaves the chip's own bytes in it when it refuses
         * - BCD, unmasked, seconds first - so a caller that failed can be told
         * what actually came back rather than only that something went wrong.
         * "ff ff ff" is a board with no chip answering; an hour byte without
         * bit 6 is a board in 12-hour mode; anything else is a wire that did
         * not do what was asked.  None of that is visible from the return
         * value, and a hardware report that carries only the return value is
         * the report that costs a whole session to interpret. */
        rtc_rc = muzix_zeta_rtc_get(&rtc_time);
        if (muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                        (const uint8_t *)&rtc_time,
                                        MUZIX_RTC_STRUCT_SIZE) != 0) {
            return -1;
        }
        if (arg2 && muzix_copy_user_to_current(ctx, (uint16_t)arg2,
                                                (const uint8_t *)&muzix_boot_rtc,
                                                MUZIX_RTC_STRUCT_SIZE) != 0) {
            return -1;
        }
        return rtc_rc == 0 ? 0 : -1;
    }
    if (syscall_num == MUZIX_USER_SYS_SETRTC) {
        muzix_rtc_time_t k_time;
        uint16_t done = 0;
        int set_rc;

        if (!arg1) {
            return -1;
        }

        /* In MUZIX_ZETA_COPY_STAGING-sized pieces, for the same reason the read
         * path chunks: the cap is a refusal and not a split.  The struct is
         * seven bytes and the cap is 256, so this is one transfer today, but
         * written as a loop because a struct that outgrew the buffer would
         * otherwise be rejected outright - leaving the clock silently unset,
         * which is the failure this path exists to avoid. */
        while (done < MUZIX_RTC_STRUCT_SIZE) {
            uint16_t chunk = (uint16_t)(MUZIX_RTC_STRUCT_SIZE - done);

            if (chunk > MUZIX_ZETA_COPY_STAGING) {
                chunk = (uint16_t)MUZIX_ZETA_COPY_STAGING;
            }
            if (muzix_copy_user_from_current(ctx, (uint16_t)(arg1 + done),
                                            (uint8_t *)&k_time + done,
                                            chunk) != 0) {
                return -1;
            }
            done = (uint16_t)(done + chunk);
        }

        set_rc = muzix_zeta_rtc_set(&k_time);

        /* Hand the answer back over the caller's struct, which is the whole
         * reason SYS_SETRTC is worth having a copy-out for.
         *
         * muzix_zeta_rtc_set() does not stop at the write.  It reads the chip
         * back over the same struct, so on return k_time holds what the chip
         * says - decoded to a time, or the chip's own bytes if the read-back was
         * refused - and not what it was asked to store.  This used to return the
         * driver's answer and drop the struct on the floor, so the caller's
         * buffer was left holding the request.
         *
         * That is invisible to the caller that most needs it.  `date` saves what
         * it asked for, sets, and compares the two: with nothing written back
         * the comparison was the requested time against itself, so "the clock
         * did not take the new time" could not be produced by any sequence of
         * events.  A set that went nowhere and a set that worked looked
         * identical, and a caller had no way to tell a clock that disagreed from
         * a clock that had been written.
         *
         * The copy happens whether or not the set was accepted, for the same
         * reason the read path hands the struct over either way: on a -1 the
         * struct is the only report there is.  If the copy itself fails the
         * caller's struct cannot be trusted to hold even that, so that failure is
         * reported as the -1 it is. */
        if (muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                       (const uint8_t *)&k_time,
                                       MUZIX_RTC_STRUCT_SIZE) != 0) {
            return -1;
        }
        return set_rc == 0 ? 0 : -1;
    }
    /* SYS_TIMES and SYS_PROCTAB are one call with two ways of naming the
     * process: SYS_TIMES is the caller's own slot, which is what it has always
     * meant, and SYS_PROCTAB takes a slot number so a program can enumerate the
     * table.  They share this body because the kernel does not have the bytes
     * for two of them.
     *
     * SYS_TIMES used to answer with a four-counter muzix_tms_t that it filled
     * one field at a time, and that handler measured 225 bytes of _CODE - more
     * than four times what is left below _DATA's page.  struct proc_info is
     * those four counters with the identity columns in front of them, laid out
     * so the kernel can hand the front of the slot straight to the copy with no
     * per-field code at all (lib/proc_info.h).  So the old handler is not an
     * extra cost kept next to the new one: it is the new one, and retiring it is
     * what pays for the slot argument.
     *
     * arg1 is the user buffer for both, so SYS_TIMES is unchanged for a
     * caller.  arg2 is the slot, and is only read for SYS_PROCTAB. */
    if (syscall_num == MUZIX_USER_SYS_TIMES ||
        syscall_num == MUZIX_USER_SYS_PROCTAB) {
        muzix_proc_slot_t *process = (syscall_num == MUZIX_USER_SYS_TIMES)
                                         ? muzix_current_process(ctx)
                                         : muzix_slot_process(ctx, (int)arg2);

        if (!process || !arg1) {
            return -1;
        }
        return muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                          (const uint8_t *)process,
                                          MUZIX_PROC_INFO_SIZE);
    }
    if (syscall_num == MUZIX_USER_SYS_LOAD) {
        /* Three Q14 windows out.  The update happens HERE, on the way in, and
         * not on a timer: the figure wanted is the average over an interval, and
         * an interval is only known once it has ended.  A caller that never
         * reads pays nothing, and a caller that reads often gets an average over
         * a short recent interval rather than the same figure over and over.
         *
         * arg1 is the user buffer; the fold touches kernel state only.
         *
         * The table comes from muzix_proc_table() above, which is the single
         * spelling of that lookup in this file.  Reading ctx->kernel->
         * process_table instead - the other spelling, and a real field - gave a
         * NULL in the running system for the reason given above, and this is
         * the only handler that was reading it.  Only the emulator showed it:
         * the host test drives kernel/load.c directly with a table it fills
         * itself, so it cannot see a wrong lookup in the dispatcher. */
        uint32_t windows[3];
        muzix_kernel_proc_table_t *table = muzix_proc_table(ctx);
        int i;

        if (!table || !arg1) {
            return -1;
        }
        muzix_load_sample(table, muzix_tick_now());
        for (i = 0; i < 3; i++) {
            windows[i] = muzix_load_window(i);
        }
        return muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                          (const uint8_t *)windows,
                                          (uint16_t)sizeof(windows));
    }
    if (syscall_num == MUZIX_USER_SYS_GETUID) {
        return ctx->uid;
    }
    if (syscall_num == MUZIX_USER_SYS_GETGID) {
        return ctx->gid;
    }
    if (syscall_num == MUZIX_USER_SYS_SETUID) {
        if (arg1 < 0 || arg1 > 0xffff ||
            (ctx->uid != (uint16_t)arg1 && ctx->euid != 0)) {
            return -1;
        }
        ctx->uid = (uint16_t)arg1;
        ctx->euid = (uint16_t)arg1;
        {
            muzix_proc_slot_t *process = muzix_current_process(ctx);
            if (process) {
                process->uid = ctx->uid;
                process->euid = ctx->euid;
            }
        }
        return 0;
    }
    if (syscall_num == MUZIX_USER_SYS_SETGID) {
        if (arg1 < 0 || arg1 > 0xffff ||
            (ctx->gid != (uint16_t)arg1 && ctx->euid != 0)) {
            return -1;
        }
        ctx->gid = (uint16_t)arg1;
        ctx->egid = (uint16_t)arg1;
        {
            muzix_proc_slot_t *process = muzix_current_process(ctx);
            if (process) {
                process->gid = ctx->gid;
                process->egid = ctx->egid;
            }
        }
        return 0;
    }
    if (syscall_num == MUZIX_USER_SYS_UMASK) {
        muzix_proc_slot_t *process = muzix_current_process(ctx);
        int32_t old_mask = ctx->umask;
        ctx->umask = (uint16_t)arg1 & 0777u;
        if (process) {
            process->umask = ctx->umask;
        }
        return old_mask;
    }

    if (syscall_num == MUZIX_USER_SYS_SIGNAL) {
        muzix_proc_slot_t *target = 0;

        if (!ctx->kernel || !ctx->kernel->loop || ctx->pid < 0 ||
            arg1 <= 0 || arg1 > 0xff || arg2 <= 0 || arg2 > 16) {
            return -1;
        }
        processes = &ctx->kernel->loop->system.startup.proc_table;
        for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
            if (processes->slots[slot].active &&
                processes->slots[slot].pid == (uint8_t)arg1) {
                target = &processes->slots[slot];
                break;
            }
        }
        if (!target ||
            (ctx->euid != 0 && ctx->uid != target->uid &&
             ctx->uid != target->euid)) {
            return -1;
        }
        return muzix_proc_table_queue_signal(processes, slot,
                                             (uint8_t)arg2) == 0
                   ? 0
                   : -1;
    }

    if (syscall_num == MUZIX_USER_SYS_WAIT) {
        if (!ctx->kernel || !ctx->kernel->loop || ctx->pid < 0) {
            return -1;
        }
        processes = &ctx->kernel->loop->system.startup.proc_table;
        for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
            /* One address for the five uses below, for the reason given at the
             * fork handler's slot scan: `processes->slots[slot]` is a base plus
             * an index times sizeof(muzix_proc_slot_t), and SDCC recomputes that
             * multiplication and every field offset at each mention. */
            muzix_proc_slot_t *p = &processes->slots[slot];

            if (!p->exited || p->parent_pid != (uint8_t)ctx->pid) {
                continue;
            }
            if (arg1) {
                int32_t st = p->exit_status;

                if (muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                               (const uint8_t *)&st,
                                               sizeof(st)) != 0) {
                    return -1;
                }
            }
            p->exited = 0;
            p->pid = 0;
            return p->mproc.mp_pid;
        }
        return -1;
    }

    if (syscall_num == MUZIX_USER_SYS_FORK) {
        if (!ctx->kernel || !ctx->kernel->loop || ctx->pid < 0) {
            return -1;
        }
        processes = &ctx->kernel->loop->system.startup.proc_table;
        for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
            /* One address for the eight uses below.
             *
             * `processes->slots[slot]` is the base of the table plus the slot
             * index times sizeof(muzix_proc_slot_t), and SDCC recomputes that
             * for every mention: eight times here is eight multiplies, eight
             * pairs of loads and eight sets of field offsets. Binding it once
             * measured 34 bytes of _CODE back, which is more than the process
             * table's command names cost to publish. */
            muzix_proc_slot_t *p = &processes->slots[slot];

            if (p->active && p->pid == (uint8_t)ctx->pid) {
                parent_slot = slot;
            }
            if (!p->active && !p->exited && child_slot < 0) {
                child_slot = slot;
            }
            if (p->active && p->pid >= child_pid) {
                child_pid = (uint8_t)(p->pid + 1);
            }
        }
        if (parent_slot < 0 || child_slot < 0 || child_pid == 0 ||
            muzix_proc_table_fork(processes, parent_slot, child_slot,
                                  child_pid) != 0) {
            return -1;
        }
        processes->slots[child_slot].parent_pid = (uint8_t)ctx->pid;

        /* Give each side its own resume point, then let the child have the CPU.
         *
         * Both resume at the same instruction - the one after this call returns
         * in the shell - because that is where each of them left off. What
         * differs is the value in DE, and that is the entire observable
         * difference between the two: the child's is zero, which is how fork()
         * tells it apart from the parent, and the parent's is the child pid.
         *
         * The child has no resume point of its own yet - proc_table_fork copied
         * the parent's and cleared it - so seeding it here is what makes it
         * resumable at all. Without this the child is a table entry that never
         * runs: fork() returns to the parent, the parent reaches wait(), and
         * nothing ever gives the child the CPU, so the command produces no
         * output and the prompt just comes back.
         *
         * Both slots' frames are recorded before the switch is armed, because
         * the switch is picked up by the trap on its way out and the parent must
         * already have its own frame saved by then. */
        muzix_context_save_current(processes, parent_slot, child_pid);
        {
            uint16_t pc = 0;
            uint16_t sp = 0;
            uint16_t ignored = 0;

            if (muzix_proc_table_get_resume(processes, parent_slot, &pc, &sp,
                                             &ignored) == 0) {
                return -1;
            }

            /* Give the child a stack of its own, holding a copy of the part of
             * the parent's stack that is in use.
             *
             * The length is measured by the syscall entry, not here: that is the
             * only point where the parent's page is still mapped, so that is
             * where the size of the active stack can be had. The bytes are not
             * taken there any more, because the copy below can reach the same
             * page from the kernel side. If the entry declined - the stack was
             * empty, or its pointer was not in window 1 - the child keeps
             * sharing the parent's stack, which is what every implementation did
             * before this existed.
             *
             * The addresses are deliberately the parent's, because the child
             * resumes at the same stack pointer: its stub has to find the same
             * return addresses the parent's would have found. A page of zeros
             * is not a substitute - a child starting mid-stub pops its return
             * address from the stack, and unwritten memory sends it to the boot
             * stub. */
            {
                extern uint8_t muzix_fork_stack_scratch[MUZIX_ZETA_COPY_STAGING];
                extern uint16_t muzix_fork_stack_len;
                uint8_t child_stack = 0;

                if (muzix_fork_stack_len == 0 ||
                    !ctx->kernel->loop->mm ||
                    muzix_mm_alloc_page(ctx->kernel->loop->mm,
                                        &child_stack) != 0) {
                    muzix_proc_table_set_resume(processes, child_slot, pc, sp,
                                                0);
                } else {
                    muzix_proc_slot_t *child = &processes->slots[child_slot];
                    process_map_t child_map = child->map;
                    process_map_t parent_map = processes->slots[parent_slot].map;
                    uint16_t done = 0;
                    int copy_ok = 1;

                    child_map.pages[1] = child_stack;

                    /* In MUZIX_ZETA_COPY_STAGING-sized pieces, each one read
                     * out of the parent's page and written into the child's.
                     *
                     * Two transfers per piece, because the two sides are two
                     * different windows and the memory manager can only have
                     * one of them mapped at a time: the read is
                     * muzix_mm_copy_map_to_kernel(), the write is the same
                     * muzix_mm_copy_kernel_to_map() this loop always used, and
                     * muzix_fork_stack_scratch is what is in transit between
                     * them.
                     *
                     * The chunking is not new - the primitive stages through a
                     * buffer of that size and refuses anything longer - but it
                     * used to be invisible here, because the entry had already
                     * gathered the whole stack into the scratch and the loop's
                     * job was only ever the writing half. The trap computed
                     * 0x15D (349) bytes for an ordinary fork of the shell, so a
                     * single unchunked copy came back -1 and the code quietly
                     * fell back to letting the child share the parent's page.
                     * The child then ran its own `call exec` on top of the
                     * frames its parent was suspended in, overwriting the
                     * return address the parent was going to resume at. When the
                     * exec'd program exited, the parent came back not at its own
                     * `pid = fork()` but at the instruction after the *child's*
                     * exec() call - inside execute_external's `pid == 0` branch
                     * - so it printed "shell: cannot execute: <cmd>" for every
                     * command that had actually succeeded, called exit(127),
                     * and took the shell down with it. That is the hang after
                     * `ls` as well as after `cat`.
                     *
                     * A stack deeper than one piece is now an extra trip round
                     * the loop rather than a refusal, which is what lets the
                     * scratch be one piece instead of 1024 bytes. */
                    while (done < muzix_fork_stack_len) {
                        uint16_t chunk = (uint16_t)(muzix_fork_stack_len - done);

                        if (chunk > MUZIX_ZETA_COPY_STAGING) {
                            chunk = (uint16_t)MUZIX_ZETA_COPY_STAGING;
                        }
                        if (muzix_mm_copy_map_to_kernel(
                                ctx->kernel->loop->mm, &parent_map,
                                (uint16_t)(sp + done),
                                muzix_fork_stack_scratch, chunk) != 0 ||
                            muzix_mm_copy_kernel_to_map(
                                ctx->kernel->loop->mm, &child_map,
                                (uint16_t)(sp + done),
                                muzix_fork_stack_scratch, chunk) != 0) {
                            copy_ok = 0;
                            break;
                        }
                        done = (uint16_t)(done + chunk);
                    }

                    if (!copy_ok) {
                        muzix_mm_free_page(ctx->kernel->loop->mm, child_stack);
                        muzix_proc_table_set_resume(processes, child_slot, pc,
                                                    sp, 0);
                    } else {
                        uint8_t owned[1];
                        child->map = child_map;
                        owned[0] = child_stack;
                        muzix_proc_table_set_owned(processes, child_slot, owned,
                                                   1);
                        muzix_proc_table_set_resume(processes, child_slot, pc,
                                                    sp, 0);
                    }
                }
            }
        }

        if (muzix_context_switch_to(processes, child_slot) != 0) {
            /* The child cannot be resumed. Leave the parent its own frame and
             * let it carry on; the child is inert rather than a wild jump. */
            muzix_context_save_current(processes, parent_slot, child_pid);
        }
        return child_pid;
    }

    if (syscall_num == MUZIX_USER_SYS_EXEC) {
        if (!ctx->kernel || !ctx->kernel->loop || ctx->pid < 0 || !arg1) {
            return -1;
        }
        processes = &ctx->kernel->loop->system.startup.proc_table;
        for (slot = 0; slot < MUZIX_PROC_TABLE_MAX; slot++) {
            muzix_proc_slot_t *process = &processes->slots[slot];

            if (process->active && process->pid == (uint8_t)ctx->pid) {
                int argc = 0;
                size_t used = 0;

                if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path,
                                           sizeof(k_path)) != 0) {
                    return -1;
                }
                /* The command name is not filled here.  It is filled by the
                 * loader, from the path it is already given, because that is
                 * the one place every exec passes through - including the exec
                 * the kernel performs at boot, which never reaches this handler
                 * and which otherwise leaves the shell, the one process always
                 * on screen, unnamed.  See muzix_proc_set_name(). */
                /* argv is a user array of user pointers; copy the strings
                 * themselves into kernel storage. Bounded at 2 arguments,
                 * which covers every binary in the tree. */
                if (arg2) {
                    while (argc < 2) {
                        uint16_t up = 0;

                        if (muzix_copy_user_from_current(
                                ctx,
                                (uint16_t)(arg2 + (uint32_t)argc * 2),
                                (uint8_t *)&up, sizeof(up)) != 0) {
                            return -1;
                        }
                        if (up == 0) {
                            break;
                        }
                        if (muzix_copy_user_string(
                                ctx, up, &k_argv_buf[used],
                                sizeof(k_argv_buf) - used) != 0) {
                            return -1;
                        }
                        k_argv[argc] = &k_argv_buf[used];
                        used += strlen(&k_argv_buf[used]) + 1;
                        argc++;
                    }
                }
                k_argv[argc] = 0;

                if (ctx->exec_loader &&
                    ctx->exec_loader(k_path, argc ? k_argv : 0, slot) != 0) {
                    return -1;
                }
                if (muzix_proc_table_exec(processes, slot,
                                          (uint16_t)arg3) != 0) {
                    return -1;
                }

                /* Hand the new program the CPU; do not return to the caller.
                 *
                 * Without this the exec simply fell out of the handler and
                 * control went back to the *old* program - the shell's own
                 * text, on the parent's page. So the child of fork() ran the
                 * line after its exec() call, printed nothing, and exited, and
                 * `ls` never executed at all: the shell printed no output and
                 * the prompt just came back.
                 *
                 * muzix_exec_loader() has already published the entry point and
                 * the map into muzix_userspace_entry and muzix_userspace_banks,
                 * so the handoff is the same window-0 entry the first process
                 * uses at boot and the same one the yield path uses. It is
                 * assembly in window 0, and it installs the map and jumps
                 * without returning, which is what lets it be called from a
                 * handler in window 1: this call chain is simply abandoned at
                 * the jump. The new program starts on its own stack, which
                 * exec_loader has already filled with argv.
                 *
                 * Unreachable: the process that returns here is not this one
                 * any more. */
                muzix_zeta_enter_userspace();
                return 0;
            }
        }
        return -1;
    }

    if (!ctx->fs) {
        return -1;
    }

    if (syscall_num == MUZIX_USER_SYS_PIPE) {
        int fds[2];
        if (!arg1 || muzix_fs_service_pipe(ctx->fs, fds) != 0) {
            return -1;
        }
        return muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                          (const uint8_t *)fds, sizeof(fds));
    }
    if (syscall_num == MUZIX_USER_SYS_IOCTL) {
        size_t ioctl_len;

        switch ((uint16_t)arg2) {
        case MUZIX_TTY_IOCTL_GETP:
        case MUZIX_TTY_IOCTL_SETP:
            ioctl_len = sizeof(muzix_tty_params_t);
            break;
        case MUZIX_TTY_IOCTL_GETC:
        case MUZIX_TTY_IOCTL_SETC:
            ioctl_len = sizeof(muzix_tty_chars_t);
            break;
        case MUZIX_TTY_IOCTL_PENDING:
            /* PENDING has an argument it does not read - a program passes
             * whatever it likes and the device overwrites the low two bytes -
             * but it has one it must write, so it needs a length here like
             * every other request.  The user->kernel copy that follows is
             * wasted work for a getter rather than wrong work, and avoiding it
             * would mean a second, differently shaped path through this branch
             * for one request. */
            ioctl_len = sizeof(uint16_t);
            break;
        default:
            return -1;
        }
        if (ioctl_len > sizeof(k_ioctl) ||
            muzix_copy_user_from_current(ctx, (uint16_t)arg3, k_ioctl,
                                         ioctl_len) != 0) {
            return -1;
        }
        if (muzix_fs_service_ioctl(ctx->fs, (int)arg1, (uint16_t)arg2,
                                   (void *)k_ioctl) != 0) {
            return -1;
        }
        return muzix_copy_user_to_current(ctx, (uint16_t)arg3, k_ioctl,
                                          ioctl_len);
    }
    if (syscall_num == MUZIX_USER_SYS_MKNOD) {
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_mknod_path(
            ctx->fs, k_path, (uint16_t)arg2,
            (uint16_t)arg3, ctx->euid, ctx->umask);
    }

    switch (syscall_num) {
    case MUZIX_USER_SYS_OPEN:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_open_path_flags(
            ctx->fs, k_path, (uint16_t)arg2,
            ctx->euid, ctx->egid);
    case MUZIX_USER_SYS_CLOSE:
        return muzix_fs_service_close(ctx->fs, (int)arg1);
    case MUZIX_USER_SYS_READ:
        if (arg3 < 0 || (uint32_t)arg3 > 65535u) {
            return -1;
        }
        /* Chunked, because a single transfer cannot exceed the MM's staging
         * buffer.  That cap is MUZIX_ZETA_COPY_STAGING bytes per call, and
         * refusing anything longer outright meant the length a program asked
         * for was capped at 256 however small the copy: `cat` reads into a
         * 512-byte buffer, so its very first read came back -1, the loop in
         * cat's main never ran, and the file printed nothing at all - with no
         * error message to say why.  The same cap sat on the user->kernel
         * direction, so a 512-byte write would have failed the same way.
         *
         * Read() semantics: keep going until the caller's count is satisfied or
         * the file gives nothing more, so a short read is not mistaken for the
         * end of the file. */
        {
            uint8_t buffer[MUZIX_ZETA_COPY_STAGING];
            size_t total = 0;

            while (total < (size_t)arg3) {
                size_t want = (size_t)arg3 - total;

                if (want > sizeof(buffer)) {
                    want = sizeof(buffer);
                }
                status = muzix_fs_service_read(ctx->fs, (int)arg1, buffer,
                                               want, &result);
                if (status != 0) {
                    /* MUZIX_READ_AGAIN is passed through rather than folded
                     * into -1, and this is the only place in the read path
                     * that could fold it.
                     *
                     * A character device answers an idle read with
                     * MUZIX_READ_AGAIN, which means "nothing yet, ask again" -
                     * and it is lib/libc.c's read() that acts on that, by
                     * parking until the device has something.  Turning it into
                     * -1 here made it indistinguishable from a bad descriptor,
                     * so read() could not wait on it, and every console caller
                     * fell into a retry loop around a read that never waited.
                     * That loop is where the machine's 100% went.
                     *
                     * It is not a special case in the copy either: result is 0
                     * when the answer is MUZIX_READ_AGAIN, so there is nothing
                     * to copy and nothing is copied. */
                    if (status == MUZIX_READ_AGAIN) {
                        return status;
                    }
                    return -1;
                }
                if (result == 0) {
                    break;
                }
                if (muzix_copy_user_to_current(
                        ctx, (uint16_t)((uint32_t)arg2 + total), buffer,
                        result) != 0) {
                    return -1;
                }
                total += (size_t)result;
            }
            return (int32_t)total;
        }

    case MUZIX_USER_SYS_WRITE:
        if (arg3 < 0 || (uint32_t)arg3 > 65535u) {
            return -1;
        }
        {
            uint8_t buffer[MUZIX_ZETA_COPY_STAGING];
            size_t total = 0;

            while (total < (size_t)arg3) {
                size_t want = (size_t)arg3 - total;

                if (want > sizeof(buffer)) {
                    want = sizeof(buffer);
                }
                if (muzix_copy_user_from_current(
                        ctx, (uint16_t)((uint32_t)arg2 + total), buffer,
                        want) != 0) {
                    return -1;
                }
                status = muzix_fs_service_write(ctx->fs, (int)arg1, buffer,
                                                want, &result);
                if (status != 0) {
                    return -1;
                }
                total += (size_t)result;
                if (result != want) {
                    break;
                }
            }
            return (int32_t)total;
        }
    case MUZIX_USER_SYS_SEEK:
        if (arg2 < 0 || arg3 != 0) {
            return -1;
        }
        return muzix_fs_service_seek(ctx->fs, (int)arg1, (uint32_t)arg2);
    case MUZIX_USER_SYS_MKDIR:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_mkdir_path(
            ctx->fs, k_path, (uint16_t)arg2,
            ctx->umask, &inode);
    case MUZIX_USER_SYS_RMDIR:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_rmdir_path(
            ctx->fs, k_path);
    case MUZIX_USER_SYS_CHDIR:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_chdir(
            ctx->fs, k_path);
    case MUZIX_USER_SYS_GETCWD:
        if (!arg1 || arg2 <= 0) {
            return -1;
        }
        {
            char buffer[128];
            size_t length = (size_t)arg2;
            if (length > sizeof(buffer)) {
                length = sizeof(buffer);
            }
            if (muzix_fs_service_getcwd(ctx->fs, buffer, length) != 0 ||
                muzix_copy_user_to_current(ctx, (uint16_t)arg1,
                                           (const uint8_t *)buffer,
                                           strlen(buffer) + 1) != 0) {
                return -1;
            }
        }
        return 0;
    case MUZIX_USER_SYS_UNLINK:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_unlink_path(
            ctx->fs, k_path);
    case MUZIX_USER_SYS_RENAME:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        if (muzix_copy_user_string(ctx, (uint16_t)arg2, k_path2, sizeof(k_path2)) != 0) {
            return -1;
        }
        return muzix_fs_service_rename_path(
            ctx->fs, k_path, k_path2);
    case MUZIX_USER_SYS_STAT:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        status = muzix_fs_service_stat_path(ctx->fs, k_path, &stat);
        if (status != 0 || !arg2) {
            return -1;
        }
        return muzix_copy_user_to_current(ctx, (uint16_t)arg2,
                                          (const uint8_t *)&stat,
                                          sizeof(stat));
    case MUZIX_USER_SYS_FSTAT:
        status = muzix_fs_service_fstat(ctx->fs, (int)arg1, &stat);
        if (status != 0 || !arg2) {
            return -1;
        }
        return muzix_copy_user_to_current(ctx, (uint16_t)arg2,
                                          (const uint8_t *)&stat,
                                          sizeof(stat));
    case MUZIX_USER_SYS_CREAT:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_creat_path(
            ctx->fs, k_path, (uint16_t)arg2,
            ctx->umask);
    case MUZIX_USER_SYS_LINK:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        if (muzix_copy_user_string(ctx, (uint16_t)arg2, k_path2, sizeof(k_path2)) != 0) {
            return -1;
        }
        return muzix_fs_service_link_path(
            ctx->fs, k_path, k_path2);
    case MUZIX_USER_SYS_CHMOD:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_chmod_path(
            ctx->fs, k_path, (uint16_t)arg2,
            ctx->euid);
    case MUZIX_USER_SYS_ACCESS:
        if (muzix_copy_user_string(ctx, (uint16_t)arg1, k_path, sizeof(k_path)) != 0) {
            return -1;
        }
        return muzix_fs_service_access_path(
                   ctx->fs, k_path, (uint16_t)arg2,
                   ctx->uid, ctx->gid) == 0
                   ? 0
                   : -1;
    case MUZIX_USER_SYS_DUP:
        return muzix_fs_service_dup(ctx->fs, (int)arg1);
    case MUZIX_USER_SYS_READDIR:
        muzix_fs_dirent_t k_dirent;
        int drc;

        if (muzix_copy_user_from_current(ctx, (uint16_t)arg3,
                                         (uint8_t *)&k_dirent,
                                         sizeof(k_dirent)) != 0) {
            return -1;
        }
        drc = muzix_fs_service_readdir(ctx->fs, (int)arg1, (uint16_t)arg2,
                                       &k_dirent);
        /* muzix_fs_service_readdir returns 0 for an entry it filled in, 1 for
         * the end of the directory and -1 for a failure, and the entry is only
         * worth copying back when it is 0.
         *
         * This tested `drc <= 0` and so took the *success* path straight out:
         * the nine entries of the root directory were all reported to ls as
         * read, and every one of them arrived as whatever the caller's
         * `struct dirent` already held - uninitialised stack - so `ls` printed
         * nine blank lines. The copy ran only at the end of the directory,
         * where there is nothing to copy. The condition has to reject the
         * error, not the success.
         *
         * Measured: the emulator's PC watchpoint on the trap's `ret`
         * (0x224E) shows the child making 21 syscalls - open returning fd 3,
         * nine READDIRs returning 0, one returning 1, and ten single-character
         * writes - so the ten directory entries were produced and the ten new
         * characters were written, while not one byte of any name reached the
         * process. */
        if (drc < 0) {
            return drc;
        }
        if (muzix_copy_user_to_current(ctx, (uint16_t)arg3,
                                      (const uint8_t *)&k_dirent,
                                      sizeof(k_dirent)) != 0) {
            return -1;
        }
        return drc;
    case MUZIX_USER_SYS_SYNC:
        return muzix_fs_service_sync(ctx->fs);
    default:
        return -1;
    }
}

/* The syscall, with the process's clock read at both ends.
 *
 * This is the only place the kernel runs on a process's behalf, so bracketing
 * it accounts for every tick a process spends anywhere:
 *
 *   the interval that ends here started when the kernel last handed this
 *   process the CPU, so it is USER time; and the interval that ends when the
 *   body returns is the kernel working for this process, so it is SYSTEM time.
 *
 * The slot is resolved once, on the way in, and the same one is charged on the
 * way out even if the syscall changed which process is current - a fork or an
 * exec is work this process asked for, so it is work this process pays for.
 * Resolving it again on the way out would move a fork's cost onto whichever
 * process the fork left running, which is the one that did not ask for it.
 *
 * What this does NOT do is switch processes.  The tick handler counts and
 * returns; nothing here runs from the handler, and nothing here is what a
 * future timer-driven scheduler would have to reason about.  On a system with
 * no preemption every tick is charged at a boundary the process itself
 * crossed, which is why this is a small change and a preemptive one is not. */
int32_t muzix_handle_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3)
{
    muzix_proc_slot_t *process;
    int32_t result;

    if (!ctx) {
        return -1;
    }

    /* The running process, resolved once.  Two things need it and they need it
     * to be the same answer: the credentials the handler runs under, and the
     * account the two tick charges below are booked to.  Resolving it twice
     * would be a race with a context switch in between.
     *
     * Removed from here: a second pass that set ctx->pid from
     * muzix_kernel_loop_current_pid() again, immediately after the block below
     * had set it from the process table.
     *
     * The loop caches its current pid once, at boot, and nothing updated it - so
     * that undid the table lookup on the very next line, and every syscall was
     * attributed to whichever process was running at boot. The table is the one
     * source of truth for that now, and both the scheduler and the context
     * switch maintain it, so re-reading the loop's cache was not merely
     * redundant: it discarded the correct answer. The ppid it also set is
     * already set by the block below, from the same slot.
     *
     * This is what made fork/exec fail with the context switch already working:
     * the child's exec was executed against the parent's pid, so the new program
     * was loaded into the parent's slot and the parent was overwritten. */
    process = muzix_current_process(ctx);
    if (process) {
        ctx->pid = process->pid;
        ctx->uid = process->uid;
        ctx->gid = process->gid;
        ctx->euid = process->euid;
        ctx->egid = process->egid;
        ctx->umask = process->umask;
        ctx->ppid = process->parent_pid;
    }

    /* The interval that just ended is this process's own: the kernel handed it
     * the CPU at the end of its last syscall and it has been running ever since.
     * It is USER time. */
    /* Tell the accounting which slot is in flight, so that a `halt` inside any
     * bounded wait below can stop the processor without billing the interval to
     * this process.  See muzix_proc_halt_uncounted(). */
    muzix_proc_set_accounted(process);
    muzix_proc_table_charge(process, 0);

    /* Park BEFORE dispatching, because after dispatching this does not return.
     *
     * The one call that parks, and the only one whose continuation belongs to the
     * process: park() takes the user's own stack pointer out of the syscall trap's
     * SYSCALL_USER_SP cell, so the resume jumps back into this caller rather than
     * into kernel code the process's map is about to replace.  A park taken from
     * inside the kernel's own read would resume inside the kernel, and that address
     * is not in the process's windows.
     *
     * It cannot return, because park() has already taken this slot out of every
     * ready queue.  Returning would run the process once more and then strand it -
     * holding the processor, in no queue, never scheduled again.  switchout() wakes
     * whatever is due, stops the CPU if nothing else is runnable, and switches to
     * whatever is: FUZIX's plt_switchout, which ends `call _getproc` then
     * `call _switchin`, and neither comes back.
     *
     * It sits here, after the interval up to now has been booked as user time and
     * before the dispatcher runs, so the parking is charged to what came before it
     * and not to whatever happens after. */
    if (syscall_num == MUZIX_USER_SYS_PARK) {
        muzix_kernel_proc_table_t *t;

        if (!process || !ctx->kernel || !ctx->kernel->loop) {
            return -1;
        }
        t = &ctx->kernel->loop->system.startup.proc_table;
        if (arg1 <= 0) {
            arg1 = 1;                     /* a zero deadline would never come */
        }
        /* Book the interval that just ended BEFORE the park, and re-stamp.
         *
         * The charge is otherwise made at the two ends of a system call, and a
         * park abandons this one without ever reaching the closing charge - so
         * the parked interval would be added to the next call the process makes
         * on waking, and the figure would report a machine at 100% while it stood
         * still.  This is FUZIX's arrangement, and it says so:
         *
         *     "We do the accounting in switchout as it's cheaper and easier to do
         *      once.  Useful trick borrowed from Linux"
         *
         * charging here books everything up to the park and re-stamps, so what is
         * left uncharged is exactly the parked interval. */
        muzix_proc_table_charge(process, 1);
        /* `ctx`, not `t`.  This was `muzix_current_slot(t)`, and it is the
         * single reason the machine never idled.
         *
         * muzix_current_slot() takes a syscall context and resolves the table
         * through it - muzix_proc_table() at the top of this file dereferences
         * ctx->kernel and ctx->kernel->loop.  Handed `t`, which is already the
         * table, it read slots[0].pid as a kernel pointer and walked code: the
         * slot it returned was not a slot, muzix_proc_table_park() rejected it
         * at its own bounds check and returned having parked nothing, and the
         * line below ran regardless.
         *
         * So the parking never happened.  The process stayed current, nothing was
         * requeued, and muzix_kernel_switchout() sat in its dispatch loop forever
         * with should_idle() saying there was work and pick_next() finding none -
         * no HALT, no parked interval, and 100% of every tick charged to a process
         * that was doing nothing.  That is the whole of the reported saturation,
         * and no amount of work on the park path, the HALT path or the load
         * windows could have shown it, because the park never ran.
         *
         * The other three call sites in this file pass `ctx` and were always right;
         * this was the only site that did not.  SDCC 4.5 emits nothing for the
         * mismatch, and a host test cannot see it either, because on the host the
         * walk runs off the object.  kernel/test_proc_table.c now pins the
         * consequence - that this loop must converge - rather than the type. */
        muzix_proc_table_park(t, muzix_current_slot(ctx), (uint16_t)arg1);
        muzix_kernel_switchout(ctx->kernel->loop);
        return 0;                         /* not reached */
    }

    result = muzix_dispatch_userspace_syscall(
        ctx, syscall_num, (int32_t)arg1, (int32_t)arg2, (int32_t)arg3);

    /* And the interval that just ended is the kernel's, spent on this process's
     * behalf.  SYSTEM time.
     *
     * Booked to the slot resolved above rather than to whatever is current on
     * the way out: a fork or an exec inside the body changes which process is
     * current, and that work is work this process asked for, so it is work this
     * process pays for.  The alternative moves a fork's cost onto whichever
     * process the fork left running, which is the one that did not ask for it.
     *
     * Between these two charges every tick of a process's life is accounted
     * exactly once, and neither of them runs from the interrupt handler - the
     * tick counts and returns.  That is what makes this a small change and a
     * timer-driven scheduler a large one: a preemptive version has to sample a
     * process that is not running, from a context where the map is half
     * installed, and the argument in platform/zeta-v2/tick.s about why this
     * handler may touch only window 0 and window 3 is exactly what such a
     * switch would invalidate. */
    muzix_proc_table_charge(process, 1);

    return result;
}
