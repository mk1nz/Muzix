#include "context_switch.h"

void zeta_init_process_table(zeta_process_table_t *table, const process_map_t *kernel_map)
{
    int i;
    if (!table) {
        return;
    }

    for (i = 0; i < ZETA_MAX_PROCS; i++) {
        table->slots[i].pid = 0;
        table->slots[i].active = 0;
        table->slots[i].map.pages[0] = 0;
        table->slots[i].map.pages[1] = 0;
        table->slots[i].map.pages[2] = 0;
        table->slots[i].map.pages[3] = 0;
    }

    table->current_slot = -1;
    table->has_saved_kernel_map = 0;
    table->saved_kernel_map.pages[0] = 0;
    table->saved_kernel_map.pages[1] = 0;
    table->saved_kernel_map.pages[2] = 0;
    table->saved_kernel_map.pages[3] = 0;

    if (kernel_map) {
        table->kernel_map = *kernel_map;
    }
}

void zeta_set_process_map(zeta_process_table_t *table, int slot, const process_map_t *map, uint8_t pid)
{
    if (!table || !map || slot < 0 || slot >= ZETA_MAX_PROCS) {
        return;
    }

    table->slots[slot].pid = pid;
    table->slots[slot].active = 1;
    table->slots[slot].map = *map;
}

void zeta_switch_to_process(zeta_state_t *state, zeta_process_table_t *table, int slot)
{
    if (!state || !table || slot < 0 || slot >= ZETA_MAX_PROCS) {
        return;
    }

    if (!table->slots[slot].active) {
        return;
    }

    if (table->current_slot == -1) {
        zeta_save_kernel_map(state, &table->saved_kernel_map);
        table->has_saved_kernel_map = 1;
    }

    table->current_slot = slot;
    zeta_map_process(state, &table->slots[slot].map);
}

void zeta_switch_to_kernel(zeta_state_t *state, zeta_process_table_t *table)
{
    if (!state || !table) {
        return;
    }

    table->current_slot = -1;

    if (table->has_saved_kernel_map) {
        zeta_restore_map(state, &table->saved_kernel_map);
        table->has_saved_kernel_map = 0;
    } else {
        zeta_map_process(state, &table->kernel_map);
    }
}
