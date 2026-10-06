/*
 * host-test-not-portable: this test drives the Z80 memory model, which has no host
 * equivalent.  It hands the copy entry points uint16_t addresses such as 0x0100 and
 * 0x0200, which reach zeta_read_visible() and zeta_write_visible() - a volatile
 * byte dereference at a REAL 16-bit address, hardware on the target and a fault at
 * address 0x0100 on a 64-bit host.  A host pointer truncated to uint16_t is an
 * offset into the first page, not an address.  The thing under test is the address
 * translation itself, so no stub is honest.
 *
 * It never compiled until platform/zeta-v2/test_kernel/context_switch.h was added
 * on 2026-10-04, and a stale muzix_mm_copy_maps() call - which had since lost two
 * parameters - had never been reached.  That call is fixed in the file below.
 */
#include "../mm/mm_copy.h"
#include "../platform/zeta-v2/test_kernel/context_switch.h"

static int test_system_copy(void)
{
    zeta_state_t state;
    zeta_process_table_t table;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    muzix_mproc_t src;
    muzix_mproc_t dst;
    uint8_t src_buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    zeta_init(&state);
    zeta_init_process_table(&table, &kernel_map);
    muzix_mproc_init(&src, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u, 0x1000u, 1);
    muzix_mproc_init(&dst, 0x3000u, 0x4000u, 0x5000u, 0x1000u, 0x1000u, 0x1000u, 2);

    state.ram[0x0100u] = 1;
    state.ram[0x0101u] = 2;
    state.ram[0x0102u] = 3;
    state.ram[0x0103u] = 4;
    state.ram[0x0104u] = 5;
    state.ram[0x0105u] = 6;
    state.ram[0x0106u] = 7;
    state.ram[0x0107u] = 8;

    if (muzix_mm_copy_maps(&state, &table, &src, MUZIX_SEG_DATA, 0x0100u, &dst, MUZIX_SEG_DATA, 0x0200u, 8) != MUZIX_COPY_OK) {
        return 1;
    }

    if (state.ram[0x0200u + 0x1000u] != 1 || state.ram[0x0201u + 0x1000u] != 2) {
        return 2;
    }

    return 0;
}

int main(void)
{
    return test_system_copy();
}
