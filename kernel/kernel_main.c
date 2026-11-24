/* * * Muzix kernel entry point: initialises core kernel services,
 * IPC, starts userspace servers (FS, MM, PM, cron), and
 * launches the userspace shell.
 */

#include "../platform/zeta-v2/syscall_runtime.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/syscall_entry.h"
#include "../platform/zeta-v2/syscall_vector.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/context_switch.h"
#include "../platform/zeta-v2/test_kernel/bank_model.h"
#include "kernel_loop.h"
#include "../include/muzix_ipc.h"
#include "proc_table.h"
#include "system_service.h"
#include "../include/muzix_message.h"
#include "system_task.h"
#include "socket_manager.h"
#include "libc.h"

#include "load.h"
#include "cron.h"
#include "exec_loader.h"
#include "rom_read.h"

extern uint8_t muzix_tick_probe;

extern void muzix_cron_init(void);
extern void i2c_calibrate(uint16_t ctc3_counts_per_tick);

extern void shell_task(void *fs);
extern int rom_read_block(struct muzix_block_device *device, uint16_t block,
 uint8_t *buffer);

#define ROMFS_BLOCK_COUNT MUZIX_ROMFS_BLOCK_COUNT

static void uart_putc(char c);
static void uart_puts(const char *s);
static void uart_puthex(uint8_t v);

/* Trace ROMFS_BLOCK_COUNT at startup. */
static void trace_romfs_block_count(void)
{
 uart_puts("ROMFS_BLOCK_COUNT=");
 uart_puthex((uint8_t)(ROMFS_BLOCK_COUNT >> 8));
 uart_puthex((uint8_t)(ROMFS_BLOCK_COUNT & 0xFF));
 uart_puts("\r\n");
}

static muzix_kernel_loop_t g_loop;
muzix_kernel_loop_t *g_loop_ptr = &g_loop;
static muzix_zeta_syscall_runtime_t g_runtime;
static muzix_system_service_t g_system_service;
static muzix_system_task_t g_system_task;
muzix_ipc_table_t g_ipc;
muzix_block_device_t g_block_dev;
zeta_state_t g_zeta_state;
muzix_mm_service_t g_mm_service;
extern uint8_t muzix_userspace_banks[4];

uint8_t g_muzix_trace_enabled = 0;

uint8_t muzix_fork_stack_scratch[MUZIX_ZETA_COPY_STAGING];

/* Window-0 variables (defined in kernel_entry.s) - accessed via inline asm. */
extern uint16_t muzix_resume_pc;
extern uint16_t muzix_resume_sp;
extern uint16_t muzix_resume_ret;
extern uint8_t muzix_resume_banks[4];
extern uint16_t muzix_switch_pending;

muzix_exec_load_args_t g_muzix_exec_load_args;

/* * Send one character to the UART, translating \n to \r\n.
 * c: character to send
 */
static void uart_putc(char c)
{
 if (c == '\n') {
 muzix_zeta_uart_send((uint8_t)'\r', 0);
 }
 muzix_zeta_uart_send((uint8_t)c, 0);
}

/* * Send a NUL-terminated string to the UART.
 * s: string to send
 */
static void uart_puts(const char *s)
{
 while (*s) {
 uart_putc(*s);
 s++;
 }
}

/* * Send one byte as two hex digits to the UART.
 * v: byte to print
 */
static void uart_puthex(uint8_t v)
{
 static const char hex[] = "0123456789ABCDEF";
 uart_putc(hex[v >> 4]);
 uart_putc(hex[v & 0x0F]);
}

/* * Getter for the global IPC table - used by other modules.
 * Pointer to the global IPC table
 */
muzix_ipc_table_t *muzix_ipc_get_global_impl(void)
{
 return &g_ipc;
}

/* * Fork and exec a server, then set its ready queue.
 * * The child's ready queue is overridden here, BEFORE exec dispatches, so that
 * muzix_proc_table_exec()'s ready() call enqueues in the correct queue on the
 * first try - no unlink/re-enqueue dance.
 * path: server binary path
 * argv: server arguments
 * slot: proc table slot for the server
 * server_id: server id number
 * q: ready queue to enqueue in
 * 0 on success, -1 on failure
 */
static int kernel_fork_server(const char *path, const char *const *argv,
 int slot, uint8_t server_id, uint8_t q)
{
 muzix_system_message_t msg;
 int rc;
 int load_rc;

 memset(&msg, 0, sizeof(msg));
 msg.m_type = MUZIX_SVC_FORK;
 msg.proc1 = 0; /* parent slot */
 msg.proc2 = slot;
 msg.pid = slot; /* child pid = slot number */

 rc = muzix_system_service_dispatch(&g_system_service, &msg);
 if (rc != 0) {
 return -1;
 }

 g_loop.system.startup.proc_table.slots[slot].ready_q = q;

 /* Load the server binary. */
 g_muzix_exec_load_args.fs = 0;
 g_muzix_exec_load_args.mm = &g_mm_service;
 g_muzix_exec_load_args.processes = &g_loop.system.startup.proc_table;
 g_muzix_exec_load_args.slot = slot;
 g_muzix_exec_load_args.path = path;
 g_muzix_exec_load_args.argv = argv;

 load_rc = muzix_exec_load();
 if (load_rc != 0) {
 return -1;
 }

 g_loop.system.startup.proc_table.slots[slot].saved_context =
 g_loop.system.startup.proc_table.slots[slot].map;

 memset(&msg, 0, sizeof(msg));
 msg.m_type = MUZIX_SVC_EXEC;
 msg.proc1 = slot;
 msg.stack_ptr =
 g_loop.system.startup.proc_table.slots[slot].stack_ptr;

 rc = muzix_system_service_dispatch(&g_system_service, &msg);
 if (rc == 0) {
 g_loop.system.startup.proc_table.slots[slot].server_id = server_id;
 }
 return rc;
}

/* * Kernel entry point: initialises core kernel services, IPC,
 * starts userspace servers (FS, MM, PM, cron), and launches
 * the userspace shell. Never returns.
 */
void kernel_main(void)
{
 /* Initialise UART FIRST - before any output. */
 muzix_zeta_uart_init();

 uart_puts("KM\n");

 /* Check trace enable flag set by emulator (-t flag). */
 g_muzix_trace_enabled = (*(volatile uint8_t *)0xFC00 != 0);
 uart_puts(g_muzix_trace_enabled ? "TRACE_ENABLED\n" : "trace_disabled\n");

 /* Patch IM1 vector at 0x0038 to tick handler BEFORE enabling interrupts. */
 {
 extern void muzix_tick_isr(void);
 uint16_t handler_addr = (uint16_t)muzix_tick_isr;
 volatile uint8_t *vec = (volatile uint8_t *)0x0038;
 vec[0] = 0xC3; /* JP nnnn */
 vec[1] = (uint8_t)(handler_addr & 0xFF);
 vec[2] = (uint8_t)(handler_addr >> 8);
 }

 /* Patch syscall vector at 0x0030: JP _muzix_z80_syscall_entry. */
 {
 extern void muzix_z80_syscall_entry(void);
 uint16_t entry_addr = (uint16_t)muzix_z80_syscall_entry;
 volatile uint8_t *vec = (volatile uint8_t *)0x0030;
 vec[0] = 0xC3; /* JP nnnn */
 vec[1] = (uint8_t)(entry_addr & 0xFF);
 vec[2] = (uint8_t)(entry_addr >> 8);
 }

 trace_romfs_block_count();

 {
 process_map_t kernel_map = {
 {MUZIX_ZETA_KERNEL_BANK_0, MUZIX_ZETA_KERNEL_BANK_1,
 MUZIX_ZETA_KERNEL_BANK_2, MUZIX_ZETA_KERNEL_BANK_3}
 };
 process_map_t init_map = {
 {MUZIX_ZETA_KERNEL_BANK_0, MUZIX_ZETA_KERNEL_BANK_1,
 MUZIX_ZETA_KERNEL_BANK_2, MUZIX_ZETA_KERNEL_BANK_3}
 };
 muzix_mproc_t init_mp;
 uint16_t ctc3_counts_per_tick;

 muzix_tick_init();

 /* * The 16550's receive interrupt, and only now.
 * * uart_init() deliberately leaves IER bit 0 clear: the interrupt
 * reaches the CPU as a rising edge on CTC channel 2, and this
 * function used to enable it before the CTC was programmed at all.
 * A byte waiting in the FIFO then burned its edge while ch2 was
 * still unarmed. muzix_tick_init() has now armed every CTC
 * channel, loaded the vector base, written the IM2 table and set
 * im 2, so the level this raises produces the vector it should.
 */
 muzix_zeta_uart_rx_irq_enable();

 /* * Seed the load windows here, immediately after the tick. The
 * windows measure wall time, and on this machine wall time is the
 * 120 Hz tick that muzix_tick_init() has just started.
 */
 ctc3_counts_per_tick = muzix_tick_calibrate_ctc3();
 if (ctc3_counts_per_tick == 0) {
 ctc3_counts_per_tick = MUZIX_LOAD_CTC3_COUNTS_PER_TICK;
 }
 muzix_load_init(muzix_tick_now(), ctc3_counts_per_tick);

 extern void zeta_context_stack_init(void);
 zeta_context_stack_init();

 muzix_mproc_init(&init_mp, 0x0000u, 0x1000u, 0x2000u,
 0x1000u, 0x1000u, 0x1000u, 1);

 muzix_kernel_loop_init(&g_loop, &kernel_map, 1, &init_map);
 muzix_kernel_loop_register(&g_loop, 0, 1, &init_mp, &init_map,
 MUZIX_PROC_TASK_Q,
 (uint8_t)MUZIX_PROC_NO_MAP);

 /* Initialise system service. */
 muzix_system_service_init(&g_system_service, &g_loop);
 g_system_service.kernel_map = kernel_map;

 /* Initialise Zeta state with kernel bank registers FIRST. */
 zeta_init(&g_zeta_state);
 zeta_load_map(&g_zeta_state, &kernel_map);

 /* Initialise global zeta state pointer for syscall entry assembly. */
 muzix_z80_set_zeta_state(&g_zeta_state);

 /* Initialise system task for MM service. */
 muzix_system_task_init(&g_system_task, &g_loop, &kernel_map);

 /* Initialise IPC. */
 muzix_ipc_init(&g_ipc);

 /* Initialise the socket manager - sole authority over W5100 socket
 * allocation, lifetime, and permissions. */
 muzix_socket_manager_init(&g_muzix_socket_manager);

 /* Register kernel process (slot 0) in IPC table. */
 if (muzix_ipc_register_server(&g_ipc, 0, 0) < 0) {
 uart_puts("K0 IPC FAIL\n");
 }

 /* Initialise MM service for ROM reads - zeta_init called internally. */
 muzix_mm_service_init(&g_mm_service, &g_system_task, 0);
 muzix_mm_memory_init(&g_mm_service, &kernel_map);

 /* Initialise block device for ROMFS (used by rom_read for
 * exec_loader). */
 g_block_dev.context = &g_mm_service;
 g_block_dev.block_count = ROMFS_BLOCK_COUNT;

 uart_puts("MNT\n");
 uart_puts("SVC\n");

 /* * Bind syscalls - now using IPC.
 * * The bind has to happen, and not just set_syscall_handler: bind is
 * what publishes userspace_context, and
 * muzix_z80_userspace_handler() returns 0xffff when it is NULL - it
 * used to be set only through bind, and with only
 * set_syscall_handler() called here it was never set, so every user
 * syscall took that early exit and the handler was never reached at
 * all. This has to be after the runtime fields are filled in below.
 */
 muzix_z80_set_syscall_handler(muzix_z80_userspace_handler);
 g_runtime.kernel.loop = &g_loop;
 g_runtime.kernel.mm = &g_mm_service;
 g_runtime.kernel.process_table = &g_loop.system.startup.proc_table;
 g_loop.mm = &g_mm_service;
 g_runtime.userspace.kernel = &g_runtime.kernel;
 g_runtime.userspace.fs = 0;
 g_runtime.userspace.exec_loader = muzix_exec_load_for_slot;
 g_runtime.userspace.vm_base = 0;
 muzix_z80_bind_userspace_context(&g_runtime.userspace);

 /* * Wire the restore hook, and only the restore hook.
 * * The trap installs the kernel map itself, in window 0, before it
 * calls any C dispatcher - so the enter hook is unused now and is
 * passed as NULL on purpose. The restore is the one that matters:
 * dispatch_frame() skips restoring while a context switch is pending,
 * and calls this on every other return. With it NULL the trap
 * returns to the process with the kernel map still installed, and
 * the process resumes executing kernel code under its own stack:
 * it wanders out of the kernel, into the boot stub at 0x0000, and
 * the whole machine reboots.
 */
 muzix_z80_set_mapping_hooks(NULL, muzix_z80_restore_user_map);

 uart_puts("MNT\n");
 uart_puts("SVC\n");
 uart_puts("RDY\n");

 /* Kernel process (slot 0). */
 g_loop.system.startup.proc_table.slots[0].active = 1;
 g_loop.system.startup.proc_table.slots[0].pid = 0;

 /* Start userspace servers from filesystem. */
 uart_puts("IPC\n");

 /* Fork and exec FS server (PID 2, slot 2) - MUST BE FIRST. */
 {
 const char *fs_argv[] = { "/sbin/fs_server", NULL };
 if (kernel_fork_server("/sbin/fs_server", fs_argv, 2, 1,
 MUZIX_PROC_SERVER_Q) != 0) {
 uart_puts("FS SVR FAIL\n");
 } else {
 if (muzix_ipc_register_server(&g_ipc, MUZIX_PROC_FS,
 MUZIX_PROC_FS) < 0) {
 uart_puts("FS IPC FAIL\n");
 }
 }
 }

 /* Fork and exec MM server (PID 3, slot 3). */
 {
 const char *mm_argv[] = { "/sbin/mm_server", NULL };
 if (kernel_fork_server("/sbin/mm_server", mm_argv, 3, 2,
 MUZIX_PROC_SERVER_Q) != 0) {
 uart_puts("MM SVR FAIL\n");
 } else {
 if (muzix_ipc_register_server(&g_ipc, MUZIX_PROC_MM,
 MUZIX_PROC_MM) < 0) {
 uart_puts("MM IPC FAIL\n");
 }
 }
 }

 /* Fork and exec PM server (PID 4, slot 4). */
 {
 const char *pm_argv[] = { "/sbin/pm_server", NULL };
 if (kernel_fork_server("/sbin/pm_server", pm_argv, 4, 3,
 MUZIX_PROC_SERVER_Q) != 0) {
 uart_puts("PM SVR FAIL\n");
 } else {
 if (muzix_ipc_register_server(&g_ipc, MUZIX_PROC_PM,
 MUZIX_PROC_PM) < 0) {
 uart_puts("PM IPC FAIL\n");
 }
 }
 }

 /* Fork and exec cron server (PID 5, slot 5). */
 {
 const char *cron_argv[] = { "/sbin/cron_server", NULL };
 if (kernel_fork_server("/sbin/cron_server", cron_argv, 5, 5,
 MUZIX_PROC_SERVER_Q) != 0) {
 uart_puts("CRON SVR FAIL\n");
 } else {
 if (muzix_ipc_register_server(&g_ipc, MUZIX_PROC_CRON,
 MUZIX_PROC_CRON) < 0) {
 uart_puts("CRON IPC FAIL\n");
 }
 }
 }

 /* Fork and exec shell (PID 1, slot 1). */
 {
 const char *shell_argv[] = { "/sbin/shell", NULL };
 if (kernel_fork_server("/sbin/shell", shell_argv, 1, 4,
 MUZIX_PROC_USER_Q) != 0) {
 uart_puts("FORK FAIL\n");
 muzix_trace_dump();
 shell_task(0);
 return;
 } else {
 if (muzix_ipc_register_server(&g_ipc, 1, 1) < 0) {
 uart_puts("SH IPC FAIL\n");
 }
 }
 }

 /* Enable IPC mode for exec_loader now that all servers exist. */
 muzix_exec_enable_ipc();

 /* Initialise the cron subsystem: records the boot tick instant so
 * the first CRON_IPC_TICK message goes out at the first whole
 * minute. */
 muzix_cron_init();

 uart_puts("SCHED\n");
 muzix_trace_stack_report();

 /* Enter scheduler forever - becomes idle process. */
 g_loop.in_boot_phase = 1;
 muzix_kernel_loop_run(&g_loop);
 }
}
