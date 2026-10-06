/*
 * syscall_host - the userspace syscall trap, for a host test binary.
 *
 * WHY THIS EXISTS
 * ---------------
 * lib/syscall.c is in the host pool so that platform/zeta-v2/test_syscall_
 * runtime.c can be tested: what that test asserts is that BINDING the runtime
 * makes the entry reachable and RESETTING it makes the entry answer -1 again,
 * and the only way to observe that from the userspace side is to call a
 * wrapper - sys_getpid() in the test - and see the answer change.  A test that
 * provided its own sys_getpid() would pass while proving nothing.
 *
 * lib/syscall.c will not link without two more symbols, and both come from
 * lib/syscall.asm, which is Z80 assembly: `muzix_syscall_void`, the trap, and
 * `syscall_result`, the cell it writes the answer into.  This file supplies
 * them in C.  It is the same arrangement as test_host/z80_io_host.c supplying
 * muzix_tick_now() and muzix_tick_add_process_time() for platform/zeta-v2/
 * tick.s, and for the same reason: that module is assembly and cannot be
 * compiled here, but the C above it is ordinary and can be.
 *
 * THE CONTRACT, KEPT EXACTLY
 * --------------------------
 * lib/syscall.h is explicit about two things, and both matter:
 *
 *   - SYSCALL() ignores the trap's return value and reads `syscall_result`
 *     instead.  The comment there gives the reason: _syscall is hand-written
 *     assembly, its return convention is not the one this build applies
 *     consistently, and it was measured giving sys_read both a zero low word
 *     and 65536.  A memory cell has no convention to disagree about.  So the
 *     answer is stored in the cell AND returned; a caller reading either gets
 *     the same value, which is what makes this shim safe to substitute.
 *
 *   - The trap keeps an `int32_t` prototype even though its result is
 *     discarded, because declaring it void makes SDCC choose a different
 *     parameter layout - DE and HL are no longer taken by the first parameter -
 *     and lib/user_syscall.s is hand-written against the int32 layout.  The
 *     prototype here is int32_t for that reason and not for the return value.
 *
 * THE INSTALLED ENTRY IS ASKED FOR, NOT DUPLICATED
 * ------------------------------------------------
 * This file does NOT define muzix_set_syscall_entry(); lib/syscall.c does, and
 * `syscall_entry` is static there.  A shim that defined its own setter would
 * collide with it, and tools/host_tests.rb refuses to link a module that
 * redefines a global another selected module already defines - so the collision
 * would keep lib/syscall.c out of the closure and take every sys_* wrapper
 * with it.  muzix_get_syscall_entry() was added to lib/syscall.c for this.
 *
 * NO ENTRY INSTALLED MEANS -1, not a trap.
 * ---------------------------------------
 * With nothing installed, the target would execute `call 0x0030` and land in
 * whatever is at that address.  Here that is nothing at all, so this answers
 * -1 instead - which is what platform/zeta-v2/test_syscall_runtime.c asserts
 * after muzix_zeta_syscall_runtime_reset().  Answering -1 rather than
 * trapping is what lets that assertion be made at all.
 */

#include <stdint.h>

#include "../lib/syscall.h"

int32_t syscall_result = -1;

int32_t muzix_syscall_void(int32_t syscall_num, int32_t arg1, int32_t arg2,
                           int32_t arg3)
{
    muzix_syscall_entry_t entry = muzix_get_syscall_entry();

    if (!entry) {
        syscall_result = -1;
        return syscall_result;
    }
    syscall_result = entry(syscall_num, arg1, arg2, arg3);
    return syscall_result;
}