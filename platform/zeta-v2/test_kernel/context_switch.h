#ifndef MUZIX_CONTEXT_SWITCH_H
#define MUZIX_CONTEXT_SWITCH_H

#include <stdint.h>

#include "bank_model.h"

#define ZETA_MAX_PROCS 4

typedef struct {
    uint8_t pid;
    process_map_t map;
    uint8_t active;
} zeta_proc_slot_t;

typedef struct {
    zeta_proc_slot_t slots[ZETA_MAX_PROCS];
    int current_slot;
    process_map_t kernel_map;
    process_map_t saved_kernel_map;
    uint8_t has_saved_kernel_map;
} zeta_process_table_t;

void zeta_init_process_table(zeta_process_table_t *table, const process_map_t *kernel_map);
void zeta_set_process_map(zeta_process_table_t *table, int slot, const process_map_t *map, uint8_t pid);
void zeta_switch_to_process(zeta_state_t *state, zeta_process_table_t *table, int slot);
void zeta_switch_to_kernel(zeta_state_t *state, zeta_process_table_t *table);

#endif
