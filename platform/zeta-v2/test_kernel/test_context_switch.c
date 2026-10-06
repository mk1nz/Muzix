#include "context_switch.h"

static int test_context_switch(void)
{
    zeta_state_t state;
    zeta_process_table_t table;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t proc_a = {{1, 2, 3, 3}};
    process_map_t proc_b = {{4, 5, 6, 3}};

    zeta_init(&state);
    zeta_init_process_table(&table, &kernel_map);
    zeta_set_process_map(&table, 0, &proc_a, 1);
    zeta_set_process_map(&table, 1, &proc_b, 2);

    zeta_map_process(&state, &kernel_map);
    if (state.bank_reg[0] != 7 || state.bank_reg[1] != 7 || state.bank_reg[2] != 7) {
        return 1;
    }

    zeta_switch_to_process(&state, &table, 0);
    if (state.bank_reg[0] != 1 || state.bank_reg[1] != 2 || state.bank_reg[2] != 3) {
        return 2;
    }

    zeta_switch_to_process(&state, &table, 1);
    if (state.bank_reg[0] != 4 || state.bank_reg[1] != 5 || state.bank_reg[2] != 6) {
        return 3;
    }

    zeta_switch_to_kernel(&state, &table);
    if (state.bank_reg[0] != 7 || state.bank_reg[1] != 7 || state.bank_reg[2] != 7) {
        return 4;
    }

    zeta_switch_to_process(&state, &table, 0);
    if (state.bank_reg[0] != 1 || state.bank_reg[1] != 2 || state.bank_reg[2] != 3) {
        return 5;
    }

    zeta_switch_to_kernel(&state, &table);
    if (state.bank_reg[0] != 7 || state.bank_reg[1] != 7 || state.bank_reg[2] != 7) {
        return 6;
    }

    return 0;
}

int main(void)
{
    return test_context_switch();
}

/*
 * host-test-not-portable: not because of the memory model - this file never
 * touches zeta_read_visible(), zeta_write_visible() or state.ram, so unlike
 * test_bank_model.c and test_minix_adapter.c it has no raw 16-bit access in it
 * and would run on a host if it could reach the functions it needs.
 *
 * What it needs is platform/zeta-v2/test_kernel/bank_model.c, and the host
 * runner cannot currently prefer that over test_host/zeta_host.c.  Both define
 * zeta_init, zeta_map_process, zeta_copy_user_to_kernel and several more;
 * tools/host_tests.rb seeds every test_host/ module into the link before it
 * looks at anything else (`selected = STUBS.dup`), so the stub is always chosen
 * first and the model can never be added: adding it is refused by the overlap
 * check, which exists to stop two modules defining one global.  The
 * host-test-provides seam is no use here either - it DROPS pool modules that
 * define a named symbol, so naming the model would drop the model.
 *
 * So this needs a small addition to the runner: a way for a test to name a stub
 * it does not want, the mirror image of host-test-provides.  Everything else
 * about it is ready - the assertions read state.bank_reg only, and they are
 * about the map bookkeeping rather than about memory at all.
 */
