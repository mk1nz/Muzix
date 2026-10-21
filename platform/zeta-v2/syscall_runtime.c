#include "syscall_runtime.h"
#include "bank_io.h"
#include "context_switch.h"
#include "trace.h"

#include "../../lib/syscall.h"

#include <string.h>

static process_map_t g_cached_map;
static int g_current_pid;
static int g_slot;
static zeta_state_t *g_runtime_state;

/* Bank map of the process that was running when the syscall trapped, held in
 * the MM's own saved-context type so the transition goes through the memory
 * manager rather than being hand-rolled here. */
static zeta_saved_context_t g_syscall_user_context;

/* syscall_entry.s contains no bank register writes whatsoever, so entering the
 * kernel left windows 1-3 mapped to the calling process for the whole handler
 * - including window 3, which is where the kernel's own stack and every
 * _DATA global live. These two hooks are the only bank bookkeeping on the
 * syscall path, and they delegate to zeta_kernel_enter()/zeta_kernel_exit()
 * rather than writing bank registers from this file: save what was installed,
 * install the kernel map, put the process map back on the way out. */
static void syscall_enter_kernel_hook(zeta_state_t *state)
{
    /* Prefer the state this runtime already owns.  The pointer the assembly
     * entry passes in is only trustworthy once syscall_entry.s stops
     * overwriting it with SP; until then this is what makes the hook safe. */
    zeta_state_t *zeta = g_runtime_state ? g_runtime_state : state;

    if (!zeta) {
        return;
    }
    zeta_kernel_enter(zeta, &g_syscall_user_context);
}

static void syscall_restore_user_hook(zeta_state_t *state)
{
    zeta_state_t *zeta = g_runtime_state ? g_runtime_state : state;

    if (!zeta) {
        return;
    }
    zeta_kernel_exit(zeta, &g_syscall_user_context);
}

void muzix_zeta_syscall_runtime_init(
    muzix_zeta_syscall_runtime_t *runtime,
    muzix_kernel_loop_t *loop,
    muzix_fs_service_t *fs)
{
    (void)fs;

    if (!runtime) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    if (!loop) {
        return;
    }

    g_cached_map = loop->system.startup.proc_table.kernel_map;
    g_current_pid = muzix_kernel_loop_current_pid(loop);
    g_slot = 0;

    while (g_slot < MUZIX_PROC_TABLE_MAX) {
        if (g_current_pid >= 0 &&
            loop->system.startup.proc_table.slots[g_slot].active &&
            loop->system.startup.proc_table.slots[g_slot].pid ==
                (uint8_t)g_current_pid) {
            g_cached_map = loop->system.startup.proc_table.slots[g_slot].map;
            break;
        }
        g_slot++;
    }
    runtime->kernel.loop = loop;
    runtime->kernel.mm = loop->mm;
    runtime->kernel.process_table = &loop->system.startup.proc_table;
    runtime->userspace.kernel = &runtime->kernel;
    runtime->userspace.fs = 0;
    runtime->userspace.vm_base = 0;
    runtime->fs = 0;
    runtime->userspace.pid = muzix_kernel_loop_current_pid(loop);
    runtime->userspace.ppid = -1;
    muzix_z80_set_mapping_hooks(syscall_enter_kernel_hook,
                                syscall_restore_user_hook);
    muzix_z80_bind_userspace_context(&runtime->userspace);
    runtime->bound = 1;
}

void muzix_zeta_syscall_runtime_reset(
    muzix_zeta_syscall_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }

    if (runtime->bound) {
        muzix_z80_bind_userspace_context(0);
        muzix_z80_set_mapping_hooks(0, 0);
        g_runtime_state = 0;
    }
    memset(runtime, 0, sizeof(*runtime));
}