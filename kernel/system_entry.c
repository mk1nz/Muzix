#include "system_entry.h"

#include <string.h>

void muzix_system_entry_init(muzix_system_entry_t *entry,
                            const process_map_t *kernel_map,
                            uint8_t init_pid,
                            const process_map_t *init_map)
{
    if (!entry) {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->init_pid = init_pid;
    entry->booted = 0;
    entry->current_pid = -1;
    muzix_kernel_startup_init(&entry->startup, kernel_map, init_pid, init_map);
}

void muzix_system_entry_register(muzix_system_entry_t *entry,
                                int slot,
                                uint8_t pid,
                                const muzix_mproc_t *mp,
                                const process_map_t *map,
                                uint8_t q,
                                uint8_t flags)
{
    if (!entry || !mp || !map) {
        return;
    }

    muzix_kernel_startup_register(&entry->startup, slot, pid, mp, map, q, flags);
}

void muzix_system_entry_start(muzix_system_entry_t *entry)
{
    if (!entry) {
        return;
    }

    if (!entry->booted) {
        muzix_kernel_startup_boot(&entry->startup);
        entry->current_pid = muzix_kernel_startup_current_pid(&entry->startup);
        entry->booted = 1;
    }
}

void muzix_system_entry_switch_to_kernel(muzix_system_entry_t *entry)
{
    if (!entry) {
        return;
    }

    muzix_kernel_startup_switch_to_kernel(&entry->startup);
    entry->current_pid = -1;
    entry->booted = 0;
}

int muzix_system_entry_current_pid(const muzix_system_entry_t *entry)
{
    if (!entry) {
        return -1;
    }

    if (entry->booted) {
        return entry->current_pid;
    }

    return muzix_kernel_startup_current_pid(&entry->startup);
}
