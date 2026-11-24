/*
 * test_tick_deadline.c - focused T4 verification: the tick_deadline_valid
 * flag and the sys_park() timeout range policy.
 *
 * These cases run on the host through the same proc_table.c and tick stubs
 * the ROM build uses, so they cover the real code, not a restatement of it.
 *
 * Covered:
 *  - the six T4 modular-deadline arithmetic cases, each asserting
 *    tick_stamp, tick_deadline_valid == 1, not-yet-expired at the park time,
 *    expired exactly when the counter reaches the deadline, wake_expired()
 *    returning 1, and the valid flag cleared by the wake;
 *  - sys_park(65535): an infinite wait, tick_deadline_valid == 0, never
 *    woken by wake_expired() (exercised both directly via park() and through
 *    the real dispatcher);
 *  - park(0): a deadline equal to now, reported expired at once (the syscall
 *    handler clamps arg1 <= 0 to one tick before calling park, so sys_park(0)
 *    waits one tick - that clamp is exercised at the syscall level below);
 *  - park(1): expires exactly one tick later, the one-tick behaviour;
 *  - muzix_proc_table_register() clearing a stale tick_deadline_valid on slot
 *    reuse (a recycled slot must not inherit a parked deadline);
 *  - muzix_proc_table_fork() not inheriting the parent's pending deadline;
 *  - muzix_proc_table_wake_input() clearing tick_deadline_valid;
 *  - the syscall handler accepting finite timeouts through the real
 *    dispatcher: sys_park(0) and sys_park(1) clamp to a one-tick finite wait,
 *    sys_park(32767) is accepted as a finite wait, sys_park(65535) is an
 *    infinite wait;
 *  - the syscall handler rejecting an unsupported finite timeout
 *    (32768-65534) with -1 instead of scheduling a modular deadline, while
 *    leaving the slot unparked.
 */

#include "libc.h"

#include "proc_table.h"
#include "syscalls.h"
#include "kernel_loop.h"
#include "../platform/zeta-v2/tick.h"

/* From test_host/z80_io_host.c: the host's settable CTC tick. */
extern uint16_t muzix_host_tick;
extern void muzix_host_tick_set(uint16_t ticks);
extern void muzix_host_tick_advance(uint16_t ticks);

/* Symbols the modules under test reference that the real kernel_main / ipc /
 * syscalls / mm_service modules provide on the target.  The host runner
 * links the test_host stubs first, then resolves the rest from the module
 * pool; naming them here drops the real modules from this test's pool and
 * lets this test's own definitions win, so the link stays small and the test
 * does not pull in the whole (Z80-only) kernel web. */
 /* host-test-provides: muzix_resume_pc muzix_resume_sp g_loop_ptr g_block_dev g_ipc g_mm_service g_muzix_exec_load_args g_muzix_socket_manager muzix_boot_rtc_state muzix_fork_stack_len muzix_fork_stack_scratch muzix_rom_fd_is_synthetic rom_read_block ___stack_chk_guard ___stack_chk_fail muzix_boot_rtc muzix_copy_client_to_server muzix_copy_server_to_client muzix_copy_to_proc muzix_copy_user_from_current muzix_copy_user_to_current muzix_dispatch_userspace_syscall muzix_exec_load_for_slot muzix_exec_set_ipc_mode muzix_handle_netctl muzix_handle_netinfo muzix_handle_netstat muzix_ipc_proc muzix_ipc_receive muzix_ipc_register_server muzix_ipc_send muzix_ipc_unregister_server muzix_kernel_switchout muzix_load_sample muzix_load_window muzix_mm_alloc_page muzix_mm_copy_kernel_to_map muzix_mm_copy_map_to_kernel muzix_mm_free_page muzix_rom_fstat muzix_rom_lookup_inode muzix_rom_open_path muzix_rom_read muzix_rom_readdir muzix_rom_seek muzix_socket_manager_allocate muzix_trace_hex8 muzix_trace_write muzix_w5100_ensure_init muzix_zeta_rtc_get muzix_zeta_rtc_set muzix_current_process muzix_current_slot */

uint16_t muzix_resume_pc = 0;
uint16_t muzix_resume_sp = 0;
muzix_kernel_loop_t *g_loop_ptr = 0;

void *g_block_dev = 0;
void *g_ipc = 0;
void *g_mm_service = 0;
void *g_muzix_exec_load_args = 0;
void *g_muzix_socket_manager = 0;
int muzix_boot_rtc_state = 0;
int muzix_fork_stack_len = 0;
uint8_t muzix_fork_stack_scratch[256];
int muzix_rom_fd_is_synthetic = 0;
uint8_t ___stack_chk_guard[8] = { 0 };

void
___stack_chk_fail(void) {
    /* Never reached on the exercised path; the canary is zero and matches. */
    for (;;) {
    }
}

int
muzix_boot_rtc(void) {
    return -1;
}

int
muzix_copy_user_from_current(muzix_userspace_syscall_context_t *ctx,
                             uint16_t address,
                             uint8_t *buffer,
                             size_t length) {
    return -1;
}

int
muzix_copy_user_to_current(muzix_userspace_syscall_context_t *ctx,
                           uint16_t address,
                           const uint8_t *buffer,
                           size_t length) {
    return -1;
}

int
muzix_copy_to_proc(muzix_userspace_syscall_context_t *ctx,
                   uint8_t target_slot,
                   uint16_t target_addr,
                   const uint8_t *buffer,
                   uint16_t length) {
    return -1;
}

int
muzix_copy_client_to_server(muzix_userspace_syscall_context_t *ctx,
                            uint8_t client_slot,
                            uint16_t client_addr,
                            uint8_t server_slot,
                            uint16_t server_addr,
                            uint16_t length) {
    return -1;
}

int
muzix_copy_server_to_client(muzix_userspace_syscall_context_t *ctx,
                            uint8_t client_slot,
                            uint16_t client_addr,
                            uint8_t server_slot,
                            uint16_t server_addr,
                            uint16_t length) {
    return -1;
}

int32_t
muzix_dispatch_userspace_syscall(muzix_userspace_syscall_context_t *ctx,
                                 int32_t syscall_num,
                                 int32_t arg1,
                                 int32_t arg2,
                                 int32_t arg3) {
    return -1;
}

int
muzix_exec_load_for_slot(void) {
    return -1;
}

int
muzix_exec_set_ipc_mode(void) {
    return -1;
}

int
muzix_handle_netctl(void) {
    return -1;
}

int
muzix_handle_netinfo(void) {
    return -1;
}

int
muzix_handle_netstat(void) {
    return -1;
}

muzix_ipc_proc_t *
muzix_ipc_proc(muzix_ipc_table_t *ipc,
               int16_t nr) {
    (void)ipc;
    (void)nr;
    return 0;
}

int
muzix_ipc_receive(muzix_ipc_table_t *ipc,
                  int16_t receiver,
                  int16_t src,
                  muzix_ipc_message_t *msg) {
    (void)ipc;
    (void)receiver;
    (void)src;
    (void)msg;
    return -1;
}

int
muzix_ipc_register_server(muzix_ipc_table_t *ipc,
                          int16_t kernel_slot,
                          int16_t server_id) {
    (void)ipc;
    (void)kernel_slot;
    (void)server_id;
    return -1;
}

int
muzix_ipc_send(muzix_ipc_table_t *ipc,
               int16_t sender,
               int16_t dest,
               muzix_ipc_message_t *msg) {
    (void)ipc;
    (void)sender;
    (void)dest;
    (void)msg;
    return -1;
}

void
muzix_ipc_unregister_server(muzix_ipc_table_t *ipc,
                            int16_t kernel_slot) {
    (void)ipc;
    (void)kernel_slot;
}

void
muzix_kernel_switchout(muzix_kernel_loop_t *loop) {
    (void)loop;
}

void
muzix_load_sample(void) {
}

void
muzix_load_window(void) {
}

int
muzix_mm_alloc_page(muzix_mm_service_t *mm,
                    uint8_t *page) {
    (void)mm;
    (void)page;
    return -1;
}

int
muzix_mm_copy_kernel_to_map(muzix_mm_service_t *mm,
                            const process_map_t *map,
                            uint16_t vir,
                            const void *buf,
                            uint16_t len) {
    (void)mm;
    (void)map;
    (void)vir;
    (void)buf;
    (void)len;
    return -1;
}

int
muzix_mm_copy_map_to_kernel(muzix_mm_service_t *mm,
                            const process_map_t *map,
                            uint16_t vir,
                            void *buf,
                            uint16_t len) {
    (void)mm;
    (void)map;
    (void)vir;
    (void)buf;
    (void)len;
    return -1;
}

void
muzix_mm_free_page(muzix_mm_service_t *mm,
                   uint8_t page) {
    (void)mm;
    (void)page;
}

int
muzix_rom_fstat(void) {
    return -1;
}

int
muzix_rom_lookup_inode(void) {
    return -1;
}

int
muzix_rom_open_path(void) {
    return -1;
}

int
muzix_rom_read(void) {
    return -1;
}

int
muzix_rom_readdir(void) {
    return -1;
}

int
muzix_rom_seek(void) {
    return -1;
}

int
muzix_socket_manager_allocate(void) {
    return -1;
}

int
muzix_trace_hex8(void) {
    return -1;
}

int
muzix_trace_write(void) {
    return -1;
}

int
muzix_w5100_ensure_init(void) {
    return -1;
}

int
rom_read_block(struct muzix_block_device *device,
               uint16_t block,
               uint8_t *buffer) {
    (void)device;
    (void)block;
    (void)buffer;
    return -1;
}

int
muzix_zeta_rtc_get(muzix_rtc_time_t *time) {
    (void)time;
    return -1;
}

int
muzix_zeta_rtc_set(muzix_rtc_time_t *time) {
    (void)time;
    return -1;
}

/* muzix_current_process / muzix_current_slot live in kernel/syscalls.c,
 * which the host cannot link here (it pulls the IPC web).  Minimal
 * host stand-ins: the table's `current` is the answer, exactly as the real
 * functions use it. */
muzix_proc_slot_t *
muzix_current_process(muzix_userspace_syscall_context_t *ctx) {
    int slot = muzix_current_slot(ctx);
    if (slot < 0) {
        return 0;
    }
    return &ctx->kernel->loop->system.startup.proc_table.slots[slot];
}

int
muzix_current_slot(muzix_userspace_syscall_context_t *ctx) {
    muzix_kernel_proc_table_t *table;

    if (!ctx || !ctx->kernel || !ctx->kernel->loop) {
        return -1;
    }
    table = &ctx->kernel->loop->system.startup.proc_table;
    if (table->current >= 0 && table->current < MUZIX_PROC_TABLE_MAX &&
        table->slots[table->current].active) {
        return table->current;
    }
    return -1;
}

static int failures;

static void
check(int ok, int id, const char *what) {
    if (!ok) {
        printf("FAIL %d: %s\n", id, what);
        failures++;
    }
}

/* ---- fixture ---------------------------------------------------------- */

static void
setup_table(muzix_kernel_proc_table_t *t) {
    muzix_mproc_t proc_a;
    muzix_mproc_t proc_b;
    process_map_t kernel_map = {{7, 7, 7, 7}};
    process_map_t map_a = {{1, 2, 3, 3}};
    process_map_t map_b = {{4, 5, 6, 3}};

    memset(t, 0, sizeof(*t));
    muzix_proc_table_init(t, &kernel_map);
    muzix_mproc_init(&proc_a, 0x0000u, 0x1000u, 0x2000u, 0x1000u,
                     0x1000u, 0x1000u, 10);
    muzix_mproc_init(&proc_b, 0x3000u, 0x4000u, 0x5000u, 0x1000u,
                     0x1000u, 0x1000u, 20);
    muzix_proc_table_register(t, 0, 10, &proc_a, &map_a,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    muzix_proc_table_register(t, 1, 20, &proc_b, &map_b,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
}

/* ---- T4: the six modular-deadline arithmetic cases ------------------- */

typedef struct {
    uint16_t now;
    uint16_t ticks;
    uint16_t stamp;
} deadline_case_t;

static const deadline_case_t g_deadline_cases[] = {
    { 0xFFE0u, 30,  0xFFFEu },
    { 0xFFE1u, 30,  0xFFFFu },
    { 0xFFE2u, 30,  0x0000u },
    { 0xFFDCu, 36,  0x0000u },
    { 0xFFE8u, 24,  0x0000u },
    { 0xFFFFu, 30,  0x001Du },
};

static int
test_tick_deadline_arithmetic(void) {
    unsigned i;

    for (i = 0; i < sizeof(g_deadline_cases) / sizeof(g_deadline_cases[0]); i++) {
        muzix_kernel_proc_table_t t;
        uint16_t now = g_deadline_cases[i].now;
        uint16_t ticks = g_deadline_cases[i].ticks;
        uint16_t stamp = g_deadline_cases[i].stamp;
        int id = 100 + (int)i;

        setup_table(&t);
        muzix_host_tick_set(now);
        muzix_proc_table_park(&t, 0, ticks);

        /* Parked and marked. */
        check((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u, id,
              "slot parked (WAIT_TICK set)");
        check(t.slots[0].tick_stamp == stamp, id,
              "deadline stamp is the modular sum now + ticks");
        check(t.slots[0].tick_deadline_valid == 1u, id,
              "tick_deadline_valid == 1 for a finite deadline");

        /* Not expired at the park instant. */
        check(muzix_tick_expired(stamp) == 0, id,
              "deadline not expired at the instant it is set");

        /* Expires exactly when the counter reaches the deadline. */
        muzix_host_tick_advance(ticks);
        check(muzix_tick_expired(stamp) == 1, id,
              "deadline expires when the counter reaches it");

        /* wake_expired() readies it exactly once and clears the flag. */
        check(muzix_proc_table_wake_expired(&t) == 1, id,
              "wake_expired() returns 1 at the deadline");
        check((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) == 0u, id,
              "slot no longer parked after the wake");
        check(t.slots[0].tick_deadline_valid == 0u, id,
              "tick_deadline_valid cleared by wake_expired()");
    }
    return failures == 0 ? 0 : 1;
}

/* ---- sys_park(65535): infinite wait ---------------------------------- */

static int
test_park_65535_infinite_wait(void) {
    muzix_kernel_proc_table_t t;

    setup_table(&t);
    muzix_host_tick_set(1000);
    muzix_proc_table_park(&t, 0, 65535u);

    check((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u, 200,
          "park(65535) marks the slot parked");
    check(t.slots[0].tick_stamp == 0xFFFFu, 201,
          "park(65535) stamps 0xFFFF");
    check(t.slots[0].tick_deadline_valid == 0u, 202,
          "park(65535) leaves tick_deadline_valid == 0 (infinite wait)");

    /* Advancing thousands of ticks must never wake an infinite wait. */
    muzix_host_tick_advance(5000);
    check(muzix_proc_table_wake_expired(&t) == 0, 203,
          "wake_expired() never wakes an infinite wait");
    check((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u, 204,
          "slot still parked after the advance");
    return failures == 0 ? 0 : 1;
}

/* ---- zero retains its one-tick behaviour ----------------------------- */

static int
test_park_zero_and_one_tick(void) {
    muzix_kernel_proc_table_t t;

    /* park(0): the deadline is `now`, which muzix_tick_expired() reports as
     * reached at once.  The syscall handler clamps arg1 <= 0 to one tick
     * before calling park (syscalls_ipc.c), so sys_park(0) waits one tick
     * - exercised below at park(1). */
    {
        setup_table(&t);
        muzix_host_tick_set(1000);
        muzix_proc_table_park(&t, 0, 0);
        check(t.slots[0].tick_stamp == 1000u, 300,
              "park(0) stamps now");
        check(t.slots[0].tick_deadline_valid == 1u, 301,
              "park(0) sets tick_deadline_valid == 1");
        check(muzix_tick_expired(1000u) == 1, 302,
              "park(0) is expired at once (deadline == now)");
    }

    /* One tick is the smallest finite wait: park(1) expires exactly one tick
     * later.  sys_park(0) is clamped to this by the handler, so this is the
     * one-tick behaviour zero retains. */
    {
        setup_table(&t);
        muzix_host_tick_set(2000);
        muzix_proc_table_park(&t, 0, 1);
        check(t.slots[0].tick_stamp == 2001u, 310,
              "park(1) stamps now + 1");
        check(muzix_tick_expired(2001u) == 0, 311,
              "park(1) not expired yet");
        muzix_host_tick_advance(1);
        check(muzix_tick_expired(2001u) == 1, 312,
              "park(1) expires after exactly one tick");
        check(muzix_proc_table_wake_expired(&t) == 1, 313,
              "wake_expired() wakes the one-tick park");
    }
    return failures == 0 ? 0 : 1;
}

/* ---- slot (re)initialisation cannot retain a stale valid flag --------- */

static int
test_register_clears_stale_valid_flag(void) {
    muzix_kernel_proc_table_t t;
    muzix_mproc_t proc_a;
    process_map_t map_a = {{1, 2, 3, 3}};

    setup_table(&t);
    muzix_host_tick_set(1000);
    muzix_proc_table_park(&t, 0, 100);
    check(t.slots[0].tick_deadline_valid == 1u, 400,
          "slot 0 parked with a valid deadline");

    /* Reuse the slot: register() over a slot that held a deadline must not
     * retain tick_deadline_valid == 1. */
    muzix_mproc_init(&proc_a, 0x0000u, 0x1000u, 0x2000u, 0x1000u,
                     0x1000u, 0x1000u, 10);
    muzix_proc_table_register(&t, 0, 10, &proc_a, &map_a,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    check(t.slots[0].tick_stamp == 0, 401,
          "register() clears tick_stamp on slot reuse");
    check(t.slots[0].tick_deadline_valid == 0u, 402,
          "register() clears tick_deadline_valid on slot reuse");
    return failures == 0 ? 0 : 1;
}

/* ---- fork child does not inherit the parent's pending deadline --------- */

static int
test_fork_child_does_not_inherit_deadline(void) {
    muzix_kernel_proc_table_t t;
    muzix_mproc_t proc_a;
    muzix_mproc_t proc_b;
    process_map_t map_a = {{1, 2, 3, 3}};
    process_map_t map_b = {{4, 5, 6, 3}};

    memset(&t, 0, sizeof(t));
    muzix_proc_table_init(&t, &(process_map_t){{7, 7, 7, 7}});
    muzix_mproc_init(&proc_a, 0x0000u, 0x1000u, 0x2000u, 0x1000u,
                     0x1000u, 0x1000u, 10);
    muzix_mproc_init(&proc_b, 0x3000u, 0x4000u, 0x5000u, 0x1000u,
                     0x1000u, 0x1000u, 20);
    muzix_proc_table_register(&t, 0, 10, &proc_a, &map_a,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);
    muzix_proc_table_register(&t, 1, 20, &proc_b, &map_b,
                              MUZIX_PROC_USER_Q, MUZIX_PROC_FREE);

    muzix_host_tick_set(1000);
    muzix_proc_table_park(&t, 0, 200);
    check(t.slots[0].tick_deadline_valid == 1u, 500,
          "parent parked with a valid deadline");

    check(muzix_proc_table_fork(&t, 0, 2, 30) == 0, 501,
          "fork() creates the child");
    check(t.slots[2].tick_deadline_valid == 0u, 502,
          "child does not inherit the parent's pending deadline");
    return failures == 0 ? 0 : 1;
}

/* ---- wake_input() clears validity ------------------------------------- */

static int
test_wake_input_clears_validity(void) {
    muzix_kernel_proc_table_t t;

    setup_table(&t);
    muzix_host_tick_set(1000);
    muzix_proc_table_park(&t, 0, 200);
    check(t.slots[0].tick_deadline_valid == 1u, 600,
          "slot parked with a valid deadline");

    check(muzix_proc_table_wake_input(&t) == 1, 601,
          "wake_input() wakes the slot");
    check((t.slots[0].p_flags & MUZIX_PROC_WAIT_TICK) == 0u, 602,
          "slot no longer parked after wake_input()");
    check(t.slots[0].tick_deadline_valid == 0u, 603,
          "wake_input() clears tick_deadline_valid");
    return failures == 0 ? 0 : 1;
}

/* ---- syscall handler: 32768-65534 rejected, not silently rescheduled --- */

static int
test_syscall_park_rejects_out_of_range_timeout(void) {
    static muzix_kernel_loop_t g_loop;
    static muzix_syscall_context_t g_kctx;
    static muzix_userspace_syscall_context_t g_ctx;
    muzix_kernel_proc_table_t *table;
    int32_t rc;
    int id;

    /* A live context: the current slot is slot 0, pid 7.  No process table
     * activity is needed for the rejection - the handler returns -1 before
     * park() and before switchout(). */
    memset(&g_loop, 0, sizeof(g_loop));
    g_loop.system.startup.proc_table.slots[0].active = 1;
    g_loop.system.startup.proc_table.slots[0].pid = 7;
    g_loop.system.startup.proc_table.current = 0;
    memset(&g_kctx, 0, sizeof(g_kctx));
    g_kctx.loop = &g_loop;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.kernel = &g_kctx;

    table = &g_loop.system.startup.proc_table;
    id = 700;

    for (id = 700; id <= 702; id++) {
        int32_t arg = (id == 700) ? 32768 : (id == 701 ? 40000 : 65534);

        /* Reset any state a previous call could have touched (none should). */
        table->slots[0].p_flags = 0;
        table->slots[0].tick_deadline_valid = 0;

        rc = muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_PARK,
                                            arg, 0, 0);
        check(rc == -1, id, "sys_park() rejects an unsupported finite timeout");
        check((table->slots[0].p_flags & MUZIX_PROC_WAIT_TICK) == 0u, id + 100,
              "slot not parked on rejection");
        check(table->slots[0].tick_deadline_valid == 0u, id + 200,
              "tick_deadline_valid stays 0 on rejection");
    }
    return failures == 0 ? 0 : 1;
}

/* ---- syscall handler: accepted timeouts through the real dispatcher ----- */

typedef struct {
    int32_t arg;
    uint16_t now;
    uint16_t expected_stamp;
    uint16_t expected_valid;
    int is_infinite;
} park_case_t;

static int
test_syscall_park_accepted_timeouts(void) {
    static muzix_kernel_loop_t g_loop;
    static muzix_syscall_context_t g_kctx;
    static muzix_userspace_syscall_context_t g_ctx;
    muzix_kernel_proc_table_t *table;
    int32_t rc;
    int case_no;

    /* A live context: the current slot is slot 0, pid 7.  The handler clamps
     * arg1 <= 0 to one tick, accepts 1-32767 as finite waits, passes 65535
     * through as an infinite wait, and rejects 32768-65534. */
    memset(&g_loop, 0, sizeof(g_loop));
    g_loop.system.startup.proc_table.slots[0].active = 1;
    g_loop.system.startup.proc_table.slots[0].pid = 7;
    g_loop.system.startup.proc_table.current = 0;
    memset(&g_kctx, 0, sizeof(g_kctx));
    g_kctx.loop = &g_loop;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.kernel = &g_kctx;

    table = &g_loop.system.startup.proc_table;
    case_no = 800;

    /* sys_park(0): clamped to 1, one-tick finite wait */
    {
        table->slots[0].p_flags = 0;
        table->slots[0].tick_deadline_valid = 0;
        table->slots[0].tick_stamp = 0;
        table->current = 0;
        muzix_host_tick_set(2000);
        rc = muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_PARK, 0, 0, 0);
        check(rc == 0, case_no, "sys_park(0) returns 0");
        check(table->slots[0].tick_stamp == 2001u, case_no + 1,
              "sys_park(0) stamps now + 1 (clamped to one tick)");
        check(table->slots[0].tick_deadline_valid == 1u, case_no + 2,
              "sys_park(0) sets tick_deadline_valid == 1");
        check((table->slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u, case_no + 3,
              "sys_park(0) parks the slot");
        check(muzix_tick_expired(2001u) == 0, case_no + 4,
              "sys_park(0) not expired at park time");
        muzix_host_tick_advance(1);
        check(muzix_tick_expired(2001u) == 1, case_no + 5,
              "sys_park(0) expires after exactly one tick");
        check(muzix_proc_table_wake_expired(table) == 1, case_no + 6,
              "sys_park(0) woken by wake_expired()");
        check((table->slots[0].p_flags & MUZIX_PROC_WAIT_TICK) == 0u, case_no + 7,
              "sys_park(0) no longer parked after wake");
        check(table->slots[0].tick_deadline_valid == 0u, case_no + 8,
              "sys_park(0) valid cleared by wake");
    }

    /* sys_park(1): one-tick finite wait */
    {
        table->slots[0].p_flags = 0;
        table->slots[0].tick_deadline_valid = 0;
        table->slots[0].tick_stamp = 0;
        table->current = 0;
        muzix_host_tick_set(3000);
        rc = muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_PARK, 1, 0, 0);
        check(rc == 0, case_no + 10, "sys_park(1) returns 0");
        check(table->slots[0].tick_stamp == 3001u, case_no + 11,
              "sys_park(1) stamps now + 1");
        check(table->slots[0].tick_deadline_valid == 1u, case_no + 12,
              "sys_park(1) sets tick_deadline_valid == 1");
        check(muzix_tick_expired(3001u) == 0, case_no + 13,
              "sys_park(1) not expired at park time");
        muzix_host_tick_advance(1);
        check(muzix_tick_expired(3001u) == 1, case_no + 14,
              "sys_park(1) expires after exactly one tick");
        check(muzix_proc_table_wake_expired(table) == 1, case_no + 15,
              "sys_park(1) woken by wake_expired()");
        check(table->slots[0].tick_deadline_valid == 0u, case_no + 16,
              "sys_park(1) valid cleared by wake");
    }

    /* sys_park(32767): accepted finite wait (maximum valid forward deadline) */
    {
        table->slots[0].p_flags = 0;
        table->slots[0].tick_deadline_valid = 0;
        table->slots[0].tick_stamp = 0;
        table->current = 0;
        muzix_host_tick_set(1000);
        rc = muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_PARK, 32767, 0, 0);
        check(rc == 0, case_no + 20, "sys_park(32767) returns 0");
        check(table->slots[0].tick_stamp == 33767u, case_no + 21,
              "sys_park(32767) stamps now + 32767 (modular)");
        check(table->slots[0].tick_deadline_valid == 1u, case_no + 22,
              "sys_park(32767) sets tick_deadline_valid == 1");
        check(muzix_tick_expired(33767u) == 0, case_no + 23,
              "sys_park(32767) not expired at park time");
        /* Set the tick counter directly to the deadline instead of advancing
         * 32767 ticks one by one. */
        muzix_host_tick_set(33767);
        check(muzix_tick_expired(33767u) == 1, case_no + 24,
              "sys_park(32767) expired when counter reaches deadline");
        check(muzix_proc_table_wake_expired(table) == 1, case_no + 25,
              "sys_park(32767) woken by wake_expired()");
        check(table->slots[0].tick_deadline_valid == 0u, case_no + 26,
              "sys_park(32767) valid cleared by wake");
    }

    /* sys_park(65535): infinite wait */
    {
        table->slots[0].p_flags = 0;
        table->slots[0].tick_deadline_valid = 0;
        table->slots[0].tick_stamp = 0;
        table->current = 0;
        muzix_host_tick_set(5000);
        rc = muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_PARK, 65535, 0, 0);
        check(rc == 0, case_no + 30, "sys_park(65535) returns 0");
        check(table->slots[0].tick_stamp == 0xFFFFu, case_no + 31,
              "sys_park(65535) stamps 0xFFFF");
        check(table->slots[0].tick_deadline_valid == 0u, case_no + 32,
              "sys_park(65535) leaves tick_deadline_valid == 0 (infinite wait)");
        check((table->slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u, case_no + 33,
              "sys_park(65535) parks the slot");
        muzix_host_tick_advance(5000);
        check(muzix_proc_table_wake_expired(table) == 0, case_no + 34,
              "sys_park(65535) never woken by wake_expired()");
        check((table->slots[0].p_flags & MUZIX_PROC_WAIT_TICK) != 0u, case_no + 35,
              "sys_park(65535) still parked after the advance");
        check(table->slots[0].tick_deadline_valid == 0u, case_no + 36,
              "sys_park(65535) valid stays 0");
    }

    return failures == 0 ? 0 : 1;
}

/* ---- main ------------------------------------------------------------- */

int
main(void) {
    int rc;

    rc = test_tick_deadline_arithmetic();
    if (rc != 0) return rc;
    rc = test_park_65535_infinite_wait();
    if (rc != 0) return 100 + rc;
    rc = test_park_zero_and_one_tick();
    if (rc != 0) return 200 + rc;
    rc = test_register_clears_stale_valid_flag();
    if (rc != 0) return 300 + rc;
    rc = test_fork_child_does_not_inherit_deadline();
    if (rc != 0) return 400 + rc;
    rc = test_wake_input_clears_validity();
    if (rc != 0) return 500 + rc;
    rc = test_syscall_park_rejects_out_of_range_timeout();
    if (rc != 0) return 600 + rc;
    rc = test_syscall_park_accepted_timeouts();
    if (rc != 0) return 700 + rc;

    if (failures) {
        printf("test_tick_deadline: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_tick_deadline: all cases pass\n");
    return 0;
}
