#ifndef MUZIX_ZETA_SYSCALL_ENTRY_H
#define MUZIX_ZETA_SYSCALL_ENTRY_H

#include <stdint.h>

#include "test_kernel/bank_model.h"

struct muzix_userspace_syscall_context;

/*
 * The user's own stack pointer while a system call is in flight, and - the word
 * AT it - the address in the process that returning to the trap hands back.
 *
 * Both are the resume point a parked process needs, and the trap computes them on
 * every call: it saves SP here on the way in and uses the cell to return on the way
 * out.  See muzix_z80_syscall_user_sp() in syscall_entry.c.
 */
uint16_t muzix_z80_syscall_user_sp(void);

/*
 * The argument block the assembly entry builds on the kernel stack.
 *
 * It is not a register frame: the trap clobbers the user registers and does not
 * save them, because the process stack it would have saved them in is in
 * window 1 and the handler runs with window 1 mapped to the kernel page.  What
 * the handler needs is the syscall number and the three 32-bit arguments, and
 * the entry stages exactly those in kernel memory before it changes maps.
 *
 * `number` is one byte; the entry writes a byte, and reading a word here would
 * pick up whatever the linker placed above it.
 */
typedef struct {
    uint8_t number;
    uint8_t pad;
    uint32_t arg[3];
} muzix_z80_syscall_args_t;

typedef uint16_t (*muzix_z80_syscall_handler_t)(muzix_z80_syscall_args_t *args);
typedef void (*muzix_z80_mapping_hook_t)(zeta_state_t *state);

void muzix_z80_set_syscall_handler(muzix_z80_syscall_handler_t handler);
void muzix_z80_set_mapping_hooks(muzix_z80_mapping_hook_t enter_kernel,
                                 muzix_z80_mapping_hook_t restore_user);
void muzix_z80_set_zeta_state(zeta_state_t *state);
void muzix_z80_set_zeta_state_ptr(zeta_state_t **ptr);
void muzix_z80_bind_userspace_context(
    struct muzix_userspace_syscall_context *context);
uint16_t muzix_z80_userspace_handler(muzix_z80_syscall_args_t *args);
int32_t muzix_z80_userspace_syscall_entry(int32_t syscall_num,
                                          int32_t arg1,
                                          int32_t arg2,
                                          int32_t arg3);
uint16_t muzix_z80_dispatch_frame(zeta_state_t *state,
                                  muzix_z80_syscall_args_t *args);
void muzix_z80_syscall_entry(void);

#endif
