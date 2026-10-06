/*
 * host-test-not-portable: this test drives the Z80 memory model, which has no host
 * equivalent.  It hands the copy and newmap entry points uint16_t addresses such
 * as 0x0100 and 0x0200, and an mproc pointer truncated to sixteen bits.  Those
 * reach zeta_read_visible() and zeta_write_visible(), which dereference a volatile
 * byte pointer at a REAL 16-bit address (see
 * platform/zeta-v2/kernel_bank_model.c around line 133).  On the target the window
 * is a hardware register the CPU addresses and there is nothing to model it with.
 * On a 64-bit host the same expression is a read or a write at address 0x0100,
 * which faults immediately.  A host pointer truncated to uint16_t is not a Z80
 * address at all: it is an offset into the first page of the process.
 *
 * No stubbing makes this honest, because the thing under test IS the address
 * translation.  Running it means a Z80 binary on the target, which is what
 * platform/zeta-v2/test_kernel/test_bank_model.c does and why it says the same
 * thing.  tools/host_tests.rb prints this reason with the test on every run.
 *
 * It never compiled until kernel/kernel_loop.h was added on 2026-10-04, so the
 * type muzix_kernel_loop_t was undeclared and a stale muzix_system_task_init()
 * call had never been reached.  That call is fixed in the file below.
 */
    muzix_system_task_init(&task, &loop, &kernel_map);
    muzix_pm_service_init(&pm, &task, 1);
    muzix_mm_service_init(&mm, &task, 2);
    muzix_fs_service_init(&fs, &task, 3);

    if (muzix_pm_fork(&pm, 0, 1, 2) != MUZIX_SYS_SERVICE_OK) {
        return 1;
    }
    if (muzix_pm_exec(&pm, 1, 0x1f00u) != MUZIX_SYS_SERVICE_OK ||
        loop.system.startup.proc_table.slots[1].p_flags != 0) {
        return 2;
    }
    if (muzix_mm_newmap(&mm, 0, (uint16_t)(uintptr_t)&init_mp) !=
        MUZIX_SYS_SERVICE_OK) {
        return 3;
    }
    if (muzix_mm_copy(&mm, 0, MUZIX_SEG_DATA, 0x0100u,
                      0, MUZIX_SEG_DATA, 0x0200u, 0x0010u) != MUZIX_SYS_SERVICE_OK) {
        return 4;
    }
    if (muzix_fs_process_exit(&fs, 0, 1) != MUZIX_SYS_SERVICE_OK) {
        return 5;
    }

    return 0;
}

int main(void)
{
    return test_service_clients();
}
