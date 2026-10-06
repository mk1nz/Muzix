#ifndef MUZIX_SYSTEM_ENTRY_H
#define MUZIX_SYSTEM_ENTRY_H

#include <stdint.h>

#include "startup.h"

#define MUZIX_SYSTEM_ENTRY_INIT_SLOT 0

typedef struct {
    muzix_kernel_startup_t startup;
    uint8_t init_pid;
    uint8_t booted;
    int current_pid;
} muzix_system_entry_t;

void muzix_system_entry_init(muzix_system_entry_t *entry,
                            const process_map_t *kernel_map,
                            uint8_t init_pid,
                            const process_map_t *init_map);
void muzix_system_entry_register(muzix_system_entry_t *entry,
                                int slot,
                                uint8_t pid,
                                const muzix_mproc_t *mp,
                                const process_map_t *map,
                                uint8_t q,
                                uint8_t flags);
void muzix_system_entry_start(muzix_system_entry_t *entry);
void muzix_system_entry_switch_to_kernel(muzix_system_entry_t *entry);
int muzix_system_entry_current_pid(const muzix_system_entry_t *entry);

#endif
