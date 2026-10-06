#include "../../lib/syscall.h"

static muzix_syscall_entry_t syscall_entry;

void muzix_set_syscall_entry(muzix_syscall_entry_t entry)
{
    syscall_entry = entry;
}

muzix_syscall_entry_t muzix_get_syscall_entry(void)
{
    return syscall_entry;
}
