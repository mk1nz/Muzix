/*
 * host-test-not-portable: zeta_read_visible() and zeta_write_visible() are
 * `*((volatile uint8_t *)addr)` at a real 16-bit address
 * (platform/zeta-v2/kernel_bank_model.c:133-143), because on the target the
 * window is a hardware register the Z80 addresses and there is nothing to
 * model.  On a 64-bit host that is a read or a write of address 0x1234, which
 * faults, and it faults in the first case: this binary has never executed, and
 * the run_all_tests.sh that reported it as BUILT did not run it either.  It also
 * asserts against zeta_state_t.ram, which zeta_init() does not allocate and
 * which only the emulator ever sets - so even the bank arithmetic here would be
 * reading a null pointer.  Running it on the target means building a Z80 test
 * binary and feeding it a memory model, which is a different harness from this
 * one; tools/host_tests.rb will keep it out of the host gate and print why.
 */
#include "minix_adapter.h"

static int test_minix_adapter(void)
{
    zeta_state_t state;
    minix_proc_map_t proc = {
        {
            {0x100, 0x000, 0x200},
            {0x200, 0x000, 0x200},
            {0x300, 0x000, 0x200}
        }
    };
    minix_proc_map_t address_proc = {
        {
            {0, 0, 2},
            {0, 1, 2},
            {0, 2, 2}
        }
    };
    process_map_t map = {{0, 0, 0, 0}};

    zeta_init(&state);
    muzix_minix_to_process_map(&proc, &map);

    if (map.pages[0] != 0 || map.pages[1] != 0 || map.pages[2] != 0 || map.pages[3] != 0) {
        return 1;
    }
    if (muzix_umap_segment(&address_proc, MINIX_SEG_DATA,
                           0x0100u, 0x0100u) != 0x1100u ||
        muzix_umap_segment(&address_proc, MINIX_SEG_DATA,
                           0x1f00u, 0x0200u) != 0 ||
        muzix_umap_segment(&address_proc, MINIX_SEG_DATA, 0x2000u, 1) != 0) {
        return 2;
    }
    state.ram[0x1100u] = 0xa5;
    state.ram[0x2100u] = 0;
    muzix_minix_sys_copy(&state, &address_proc, MINIX_SEG_DATA, 0x0100u,
                         &address_proc, MINIX_SEG_STACK, 0x0100u, 1);
    if (state.ram[0x2100u] != 0xa5) {
        return 3;
    }
    state.ram[0x2100u] = 0;
    muzix_minix_sys_copy(&state, &address_proc, MINIX_SEG_DATA, 0x1f00u,
                         &address_proc, MINIX_SEG_STACK, 0x0100u, 0x0200u);
    if (state.ram[0x2100u] != 0) {
        return 4;
    }

    return 0;
}

int main(void)
{
    return test_minix_adapter();
}
