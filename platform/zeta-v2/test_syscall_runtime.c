#include "syscall_runtime.h"

#include "../../lib/syscall.h"

#include <string.h>

static int test_syscall_runtime(void)
{
    muzix_kernel_loop_t loop;
    muzix_zeta_syscall_runtime_t runtime;
    muzix_fs_service_t fs;

    memset(&loop, 0, sizeof(loop));
    memset(&fs, 0, sizeof(fs));
    muzix_kernel_loop_bind_syscalls(&loop, &runtime, 0);
    /* `runtime.kernel.state` used to be asserted here.  There is no such field:
     * muzix_syscall_context_t is {loop, mm, process_table} and always has been,
     * so the test has not compiled since the type changed - which is why the
     * one thing it could have caught went unnoticed.
     *
     * What it can usefully assert now is the field that was silently left unset
     * and that SYS_LOAD read: platform/zeta-v2/syscall_runtime.c set
     * kernel.loop and kernel.mm and not kernel.process_table, so any handler
     * reaching the table through the context got NULL, and a load display read
     * 0.00 forever while the counters it was reading charged 100% of elapsed
     * time. */
    if (!runtime.bound || runtime.kernel.loop != &loop ||
        runtime.kernel.mm != loop.mm ||
        runtime.kernel.process_table != &loop.system.startup.proc_table ||
        runtime.userspace.kernel != &runtime.kernel ||
        runtime.userspace.fs != 0) {
        return 1;
    }

    muzix_zeta_syscall_runtime_reset(&runtime);
    if (runtime.bound) {
        return 2;
    }
    muzix_zeta_syscall_runtime_init(&runtime, 0, 0);
    if (runtime.bound) {
        return 3;
    }

    muzix_zeta_syscall_runtime_init(&runtime, &loop, &fs);
    if (!runtime.bound || sys_getpid() != 0 ||
        fs.tty != &runtime.tty || !runtime.tty.send ||
        !runtime.tty.recv || !runtime.tty.available) {
        return 4;
    }
    muzix_zeta_syscall_runtime_reset(&runtime);
    if (sys_getpid() != -1 || fs.tty != 0) {
        return 5;
    }
    return 0;
}

int main(void)
{
    return test_syscall_runtime();
}
