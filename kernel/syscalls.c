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

/* Forward declarations for internal dispatch functions */
int muzix_sys_copy_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);
int muzix_sys_yield_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);
int muzix_sys_exit_dispatch(muzix_syscall_context_t *ctx, const muzix_syscall_t *call);

/* Kernel-side landing buffers for user-pointer copying.
 * Required for syscalls that are not routed through IPC (exec, etc.). */
char k_path[MUZIX_FS_PATH_MAX];
char k_path2[MUZIX_FS_PATH_MAX];
char k_argv_buf[256];
uint8_t k_ioctl[8];

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
 * `ctx->kernel->process_table` is a REAL field and it IS populated in the
 * running system: kernel/kernel_main.c assigns
 * `g_runtime.kernel.process_table = &g_loop.system.startup.proc_table`, and
 * kernel/syscalls_ipc.c's muzix_syscalls_deliver_parked() reads it.  An earlier
 * version of this comment claimed it was always NULL, on the grounds that
 * platform/zeta-v2/syscall_runtime.c never sets it - which was true of that one
 * producer and false of the system, so the comment sent a reader looking for a
 * defect that was not there while the accessor below stayed unexplained.
 *
 * `ctx->kernel->loop->system.startup.proc_table` is the SAME object by a
 * different route, and it is the route this file uses for one reason: it goes
 * through `loop`, which is the field the syscall-runtime binding actually
 * publishes, so the table cannot be reached here through a field some other
 * producer forgot to fill.  muzix_system_task_handle_syscall() passes the same
 * address into a kernel context of its own, which is why SYS_COPY was never
 * affected either. */
static muzix_kernel_proc_table_t *muzix_proc_table(
    muzix_userspace_syscall_context_t *ctx)
{
    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return 0;
    }
    return &ctx->kernel->loop->system.startup.proc_table;
}

int muzix_current_slot(muzix_userspace_syscall_context_t *ctx)
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

muzix_proc_slot_t *muzix_current_process(
    muzix_userspace_syscall_context_t *ctx)
{
    muzix_kernel_proc_table_t *table = muzix_proc_table(ctx);
    int slot = muzix_current_slot(ctx);

    if (!table || slot < 0) {
        return 0;
    }
    return &table->slots[slot];
}

int muzix_copy_user_from_current(
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

int muzix_copy_user_to_current(
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

/* Copy from a specific process slot to kernel buffer (for server copy from client) */
int muzix_copy_from_proc(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t target_slot,
    uint16_t target_addr,
    uint8_t *buffer,
    uint16_t length)
{
    muzix_mm_service_t *mm;
    if (target_slot >= MUZIX_PROC_TABLE_MAX || !ctx->kernel || !ctx->kernel->loop || !buffer) {
        return -1;
    }
    mm = ctx->kernel->loop->mm;
    if (!mm) {
        return -1;
    }
    return muzix_mm_copy_user_to_kernel(mm, target_slot, target_addr, buffer, length);
}

/*
 * =====================================================================
 * COPY BETWEEN TWO USERSPACE PROCESSES - the MM-mediated path.
 * =====================================================================
 *
 * These are what a server uses to move a buffer between itself and the client
 * that called it, and they are DIFFERENT from muzix_copy_from_proc() /
 * muzix_copy_to_proc() above.  The difference is not a detail.
 *
 * muzix_copy_from_proc() calls muzix_mm_copy_user_to_kernel(), whose
 * `kernel_buf` argument is documented as a KERNEL address.  It is not one: the
 * caller is a userspace server, and the buffer it passes is a local array in
 * its own address space.  zeta_copy_user_to_kernel() installs the CLIENT's page
 * into the window the virtual address falls in, then memcpy()s out of it into
 * `kernel_buf` - so when the buffer happens to live in that same window, the
 * source and the destination are the same physical page and the copy writes
 * onto itself.  Nothing fails, and nothing is transferred.
 *
 * That is not a bug in the primitive.  The primitive is correct: it is the
 * "user -> kernel" half, and it needs a kernel destination.  The bug was
 * reaching for it from the wrong side of the boundary - which is the same
 * mistake as the five process-identity conversions, in a different register:
 * an interface whose contract lives in a parameter name, with a caller on the
 * far side of the boundary who cannot see it.
 *
 * These two instead call muzix_mm_copy_maps(), which is the MM's own
 * map-to-map copy: user -> kernel staging -> user, with the staging buffer a
 * local on the MM's kernel stack.  The kernel owns the buffer, moves the bytes,
 * and interprets nothing - exactly what MINIX's sys_copy() does, and exactly
 * the work a microkernel should be allowed to do.  Neither process's page is
 * ever asked to be the other's.
 */
int muzix_copy_client_to_server(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t client_slot,
    uint16_t client_addr,
    uint8_t server_slot,
    uint16_t server_addr,
    uint16_t length)
{
    muzix_mm_service_t *mm;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }
    if (client_slot >= MUZIX_PROC_TABLE_MAX || server_slot >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }
    mm = ctx->kernel->loop->mm;
    if (!mm) {
        return -1;
    }
    return muzix_mm_copy_user_to_user(mm, client_slot, client_addr,
                                      server_slot, server_addr, length);
}

int muzix_copy_server_to_client(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t client_slot,
    uint16_t client_addr,
    uint8_t server_slot,
    uint16_t server_addr,
    uint16_t length)
{
    muzix_mm_service_t *mm;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }
    if (client_slot >= MUZIX_PROC_TABLE_MAX || server_slot >= MUZIX_PROC_TABLE_MAX) {
        return -1;
    }
    mm = ctx->kernel->loop->mm;
    if (!mm) {
        return -1;
    }
    /*
     * THE OPERANDS ARE SWAPPED, AND THAT IS THE WHOLE FUNCTION.
     *
     * This used to forward to muzix_copy_client_to_server() unchanged, which
     * copies client -> server.  A server asking to copy TO its client therefore
     * read the client's (empty) buffer and wrote it into its own - so
     * handle_read's payload never went anywhere, and the direction that serves
     * a reply was the mirror image of the direction that failed.
     *
     * muzix_mm_copy_user_to_user() takes (src_proc, src_vir, dst_proc, dst_vir,
     * bytes), so swapping the order here is what makes the data flow server ->
     * client.  Same two MM-mediated hops, opposite direction.
     */
    return muzix_mm_copy_user_to_user(mm, server_slot, server_addr,
                                      client_slot, client_addr, length);
}

/* Copy from kernel buffer to a specific process slot (for server copy to client) */
int muzix_copy_to_proc(
    muzix_userspace_syscall_context_t *ctx,
    uint8_t target_slot,
    uint16_t target_addr,
    const uint8_t *buffer,
    uint16_t length)
{
    muzix_mm_service_t *mm;
    if (target_slot >= MUZIX_PROC_TABLE_MAX || !ctx->kernel || !ctx->kernel->loop || !buffer) {
        return -1;
    }
    mm = ctx->kernel->loop->mm;
    if (!mm) {
        return -1;
    }
    return muzix_mm_copy_kernel_to_user(mm, target_slot, target_addr, buffer, length);
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

/* The syscall body, and the non-IPC fallback: kernel-internal numbers that no
 * server claimed.
 *
 * Where the body lives: the routed numbers are handled in
 * kernel/syscalls_ipc.c, which is also the only caller of this function - it
 * gets here when muzix_syscall_to_server() finds no server slot for the number.
 * The two tick charges that make UTIME and STIME real bracket the whole thing
 * in muzix_handle_userspace_syscall(), in that same IPC file, and they are
 * there rather than here because the body has a dozen `return` statements: a
 * charge placed at the end of it would have to be written twelve times and
 * would be one missed one away from silently not accounting for a whole class
 * of syscalls.
 *
 * The `(const muzix_syscall_t *)0` is deliberate, and it is the intended
 * default rather than an oversight.  These three dispatchers are written for
 * the OTHER caller - muzix_handle_syscall(), which kernel/system_task.c drives
 * with a real call record - and every one of them opens with a NULL check
 * (`!ctx || !call`), so a NULL record is their documented way of saying "no
 * call record was supplied" and it makes them answer MUZIX_SYSCALL_FAIL.
 *
 * That is the right answer HERE, and it is worth being explicit about why,
 * because it looks like a bug: this path is reached with a userspace syscall
 * NUMBER and three integer arguments, which is a different shape from the
 * record the internal dispatchers want.  A user SYS_YIELD (10) is not
 * MUZIX_SYS_YIELD (10) - the two namespaces collide numerically and are told
 * apart by the router, which has already tried the server ranges before
 * arriving here.  So a number that lands here genuinely has no internal
 * handler, and "fail" is the honest reply; the alternative - reaching through
 * to muzix_kernel_loop_yield() on a syscall the kernel never recognized -
 * would be worse.
 *
 * The three called with NULL therefore always return MUZIX_SYSCALL_FAIL today,
 * and that is the behaviour this comment records rather than a defect to be
 * fixed by handing them a record they were not written to take. */
/*
 * Microkernel version: only kernel-internal syscalls are handled here.
 * FS, MM, PM syscalls are routed via IPC to userspace servers.
 */
int32_t muzix_dispatch_userspace_syscall(
    muzix_userspace_syscall_context_t *ctx,
    int32_t syscall_num,
    int32_t arg1,
    int32_t arg2,
    int32_t arg3)
{
    (void)arg1; (void)arg2; (void)arg3;

    if (!ctx) {
        return -1;
    }

    switch (syscall_num) {
    case MUZIX_SYS_COPY:
        return muzix_sys_copy_dispatch(ctx, (const muzix_syscall_t *)0);
    case MUZIX_SYS_YIELD:
        return muzix_sys_yield_dispatch(ctx, (const muzix_syscall_t *)0);
    case MUZIX_SYS_EXIT:
        return muzix_sys_exit_dispatch(ctx, (const muzix_syscall_t *)0);
    default:
        /* All other syscalls (FS, MM, PM) should go through IPC.
         * If we reach here, the IPC routing failed. */
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
