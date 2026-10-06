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
#include "bank_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_bank_mapping(void)
{
    zeta_state_t state;
    zeta_init(&state);

    zeta_set_bank(&state, 0, 1);
    zeta_set_bank(&state, 1, 2);
    zeta_set_bank(&state, 2, 3);
    zeta_set_bank(&state, 3, 3);

    memset(&state.ram[1 * ZETA_BANK_SIZE], 0xAA, 0x100);
    memset(&state.ram[2 * ZETA_BANK_SIZE], 0xBB, 0x100);
    memset(&state.ram[3 * ZETA_BANK_SIZE], 0xCC, 0x100);
    zeta_refresh_visible(&state);

    assert(zeta_read_visible(&state, 0x0000) == 0xAA);
    assert(zeta_read_visible(&state, 0x4000) == 0xBB);
    assert(zeta_read_visible(&state, 0x8000) == 0xCC);
}

static void test_process_isolation(void)
{
    zeta_state_t state;
    process_map_t proc_a = { {1, 2, 3, 3} };
    process_map_t proc_b = { {2, 2, 3, 3} };
    zeta_init(&state);

    zeta_map_process(&state, &proc_a);
    zeta_write_visible(&state, 0x1234, 0xA5);

    zeta_map_process(&state, &proc_b);
    zeta_write_visible(&state, 0x1234, 0x5A);

    zeta_map_process(&state, &proc_a);
    assert(zeta_read_visible(&state, 0x1234) == 0xA5);
    assert(state.ram[1 * ZETA_BANK_SIZE + 0x1234] == 0xA5);

    zeta_map_process(&state, &proc_b);
    assert(zeta_read_visible(&state, 0x1234) == 0x5A);
    assert(state.ram[4 * ZETA_BANK_SIZE + 0x1234] == 0x5A);
}

static void test_copyin_copyout(void)
{
    zeta_state_t state;
    process_map_t proc_a = { {1, 2, 3, 3} };
    uint8_t buffer[16];
    uint8_t outbuf[16];
    zeta_init(&state);

    for (int i = 0; i < 16; i++) {
        state.ram[1 * ZETA_BANK_SIZE + 0x3000 + i] = (uint8_t)(0x40 + i);
    }

    zeta_copy_user_to_kernel(&state, &proc_a, 0x3000, buffer, sizeof(buffer));
    for (int i = 0; i < 16; i++) {
        assert(buffer[i] == (uint8_t)(0x40 + i));
    }

    for (int i = 0; i < 16; i++) {
        outbuf[i] = (uint8_t)(0x80 + i);
    }

    zeta_copy_kernel_to_user(&state, &proc_a, 0x3500, outbuf, sizeof(outbuf));
    for (int i = 0; i < 16; i++) {
        assert(state.ram[1 * ZETA_BANK_SIZE + 0x3500 + i] == (uint8_t)(0x80 + i));
    }
}

int main(void)
{
    test_bank_mapping();
    test_process_isolation();
    test_copyin_copyout();

    puts("bank_model tests: PASS");
    return 0;
}
