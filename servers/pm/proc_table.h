#ifndef MUZIX_PM_PROC_TABLE_H
#define MUZIX_PM_PROC_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "../../lib/proc_info.h"
#include "../../mm/mproc_model.h"

#define MUZIX_PROC_TABLE_MAX 8
#define MUZIX_PROC_TASK_Q 0
#define MUZIX_PROC_SERVER_Q 1
#define MUZIX_PROC_USER_Q 2
#define MUZIX_PROC_NQ 3
#define MUZIX_PROC_FREE 001u
#define MUZIX_PROC_WAIT_TICK 004u
#define MUZIX_PROC_NO_MAP 002u
#define MUZIX_PROC_OWNED_PAGES 2

typedef struct {
    uint8_t pid;
    uint8_t p_flags;
    uint8_t active;
    uint8_t parent_pid;
    uint32_t user_time;
    uint32_t sys_time;
    uint32_t child_user_time;
    uint32_t child_sys_time;
    uint8_t exit_status;
    uint8_t exited;
    uint8_t ready_q;
    uint8_t owned_count;
    char name[MUZIX_PROC_NAME_SIZE];
    uint16_t pending_signals;
    uint16_t resume_pc;
    uint16_t resume_sp;
    uint16_t resume_ret;
    uint8_t resume_valid;
    uint16_t stack_ptr;
    uint16_t entry_point;
    uint16_t argc;
    uint16_t argv_ptr;
    uint16_t uid;
    uint16_t gid;
    uint16_t euid;
    uint16_t egid;
    uint16_t umask;
    int next_ready;
    uint8_t owned_pages[MUZIX_PROC_OWNED_PAGES];
    muzix_mproc_t mproc;
    process_map_t map;
    process_map_t saved_context;
    uint16_t tick_stamp;
    uint8_t server_id;
} muzix_pm_proc_slot_t;

typedef struct {
    muzix_pm_proc_slot_t slots[MUZIX_PROC_TABLE_MAX];
    int current;
    int ready_head[MUZIX_PROC_NQ];
    int ready_tail[MUZIX_PROC_NQ];
    process_map_t kernel_map;
} muzix_pm_proc_table_t;

void muzix_pm_proc_table_init(muzix_pm_proc_table_t *table, const process_map_t *kernel_map);
void muzix_pm_proc_table_register(muzix_pm_proc_table_t *table,
                                 int slot,
                                 uint8_t pid,
                                 const muzix_mproc_t *mp,
                                 const process_map_t *map,
                                 uint8_t q,
                                 uint8_t flags);
void muzix_pm_proc_table_ready(muzix_pm_proc_table_t *table, int slot);

void muzix_pm_proc_table_park(muzix_pm_proc_table_t *table, int slot, uint16_t ticks);
int muzix_pm_proc_table_wake_expired(muzix_pm_proc_table_t *table);
int muzix_pm_proc_table_wake_input(muzix_pm_proc_table_t *table);
int muzix_pm_proc_table_any_parked(muzix_pm_proc_table_t *table);
int muzix_pm_proc_table_should_idle(muzix_pm_proc_table_t *table);

void muzix_pm_proc_set_accounted(muzix_pm_proc_slot_t *slot);
void muzix_pm_proc_halt_uncounted(void);
int muzix_pm_proc_table_fork(muzix_pm_proc_table_t *table, int parent_slot, int child_slot, uint8_t child_pid);
int muzix_pm_proc_table_exec(muzix_pm_proc_table_t *table, int slot, uint16_t stack_ptr);
void muzix_pm_proc_set_name(muzix_pm_proc_table_t *table, int slot, const char *path);
int muzix_pm_proc_table_exit(muzix_pm_proc_table_t *table, int slot);
void muzix_pm_proc_table_release_pages(muzix_pm_proc_table_t *table, int slot);
void muzix_pm_proc_table_set_owned(muzix_pm_proc_table_t *table, int slot, const uint8_t *pages, uint8_t count);
void muzix_pm_proc_table_set_resume(muzix_pm_proc_table_t *table, int slot, uint16_t pc, uint16_t sp, uint16_t retval);
int muzix_pm_proc_table_get_resume(const muzix_pm_proc_table_t *table, int slot, uint16_t *pc, uint16_t *sp, uint16_t *retval);
int muzix_pm_proc_table_exit_status(muzix_pm_proc_table_t *table, int slot, uint8_t parent_pid, uint8_t status);
int muzix_pm_proc_table_queue_signal(muzix_pm_proc_table_t *table, int slot, uint8_t signal);
int muzix_pm_proc_table_deliver_pending(muzix_pm_proc_table_t *table, int slot);
int muzix_pm_proc_table_pick_next(muzix_pm_proc_table_t *table);
int muzix_pm_proc_table_yield(muzix_pm_proc_table_t *table);
void muzix_pm_proc_table_switch_to_kernel(muzix_pm_proc_table_t *table);
int muzix_pm_proc_table_current_pid(const muzix_pm_proc_table_t *table);
void muzix_pm_proc_table_charge(muzix_pm_proc_slot_t *process, uint8_t kernel_time);

int muzix_pm_proc_find_by_pid(const muzix_pm_proc_table_t *table, uint8_t pid);
int muzix_pm_proc_find_free_slot(const muzix_pm_proc_table_t *table);

#endif