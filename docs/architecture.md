# Muzix Architecture

Muzix is an operating system for the Z80 and Zeta SBC V2, built on ideas
from MINIX 1.0 (message-passing IPC, priority queues, cooperative scheduling)
and the MINIX-v1 filesystem format.

## Runtime Layers (Microkernel Architecture)

- **Kernel** (`kernel/`) - Minimal microkernel core (its size is not a fixed
  figure; read the current extents from `build/kernel.map` or
  `make test-kernel-layout` rather than from a byte count in this file):
  - IPC message passing (send/receive/both, priority queues)
  - Cooperative scheduler (3 queues: TASK_Q, SERVER_Q, USER_Q)
  - Syscall entry/trap handler with bank switching
  - Direct ROM reads for initial server loading (before FS server runs)
  - ROM filesystem interceptors for open/readdir/read/close — the kernel
    serves these syscalls directly from the ROM block device (`kernel/rom_read.c`)
    rather than routing to the FS server, using the same interceptor shape it
    uses for TIMES, PROCTAB, RTC, LOAD, and IDS. This is necessary because the
    userspace FS server links only `fs_server`, `fs_clock`, `tty_device`, and
    the bank primitives — the `fs/` service layer is not among them and the
    file-operation handlers in `servers/fs/fs_server.c` are stubs.

- **Userspace Servers** (launched at boot via fork/exec):
  - **FS Server** (slot 2, `MUZIX_PROC_FS`) - TTY console (descriptors 0/1/2)
    via the TTY line discipline; the ROM filesystem is served by the kernel
    directly (see below)
  - **MM Server** (slot 3, `MUZIX_PROC_MM`) - memory management via IPC
  - **PM Server** (slot 4, `MUZIX_PROC_PM`) - process management via IPC

  The server numbers are the `MUZIX_PROC_*` constants in `include/muzix_ipc.h`
  and are the same integers as the kernel slots; do not derive them from any
  other id (`kernel/kernel_main.c` registers each with `MUZIX_PROC_*` as both
  arguments, and `kernel/syscalls_ipc.c` no longer reads the legacy
  `server_id` field at all).

- **Platform** (`platform/zeta-v2/`) - Bank mapping, syscall entry, UART, RTC, CTC

- **Userspace** (`lib/`, `shell/`, `apps/`) - Runtime, PID 1 shell, programs

The kernel dispatches the custom Muzix syscall ABI. FS, MM, PM syscalls are
routed via IPC to userspace servers; kernel-internal syscalls (COPY, YIELD, EXIT)
are handled inline.

## Memory and Execution

The Zeta V2 board exposes four 16 KiB windows in the Z80's 64 KiB address
space. A process map keeps the kernel's window 0 and window 3 pages; the
process's allocated stack and text pages occupy windows 1 and 2. The physical
page identifiers are allocated by the MM server via IPC. Segment virtual
addresses, physical page identifiers, and CPU-visible window addresses are
distinct values.

`exec_loader.c` allocates pages through MM, builds the process map, loads the
program into its text page, and installs its linker-derived entry point.

**Argv layout on the process stack (window 1, below 0x8000):**

User memory layout constants:
- `MUZIX_EXEC_STACK_TOP = 0x7FFF` (top of window 1)
- `MUZIX_EXEC_SP_ADDR = 0x7FEC` (where SP is published to crt0)
- `MUZIX_EXEC_ARGV_ADDR = 0x7FEE` (where argv pointer array address is published)
- `MUZIX_EXEC_ARGC_ADDR = 0x7FF0` (where argc is published)

Layout (lowest to highest):
1. **Stack** (grows down from `stack_base - 16` where `stack_base = SP_ADDR - argc*2 - 256` minus string lengths)
2. **String data** (argv strings, written downward from `stack_base`)
3. **argv pointer array** (`argv_array_addr = SP_ADDR - argc*2`, each entry is 2 bytes pointing to a string)
4. **Control words** (0x7FEC=SP, 0x7FEE=argv ptr, 0x7FF0=argc — read by crt0.s)

The argv array and strings are placed **below** the control word region (0x7FEC-0x7FF1) to prevent overlap: the array is derived from `MUZIX_EXEC_SP_ADDR`, not `MUZIX_EXEC_STACK_TOP`. Earlier code derived it from `stack_top` (0x7FFF), which for argc >= 7 caused the argv array to collide with the control words, corrupting argc/argv/sp or the argument strings themselves.

**Before the FS server is running**, exec_loader uses direct ROM reads (via
`rom_read_block` → `mm_service` → `zeta_copy_rom_page_to_kernel`). **After
FS server starts**, exec_loader switches to IPC-based filesystem access.

Platform bank-switch routines are the only layer that manipulates the bank
registers.

## Time and Scheduling

The scheduler is cooperative; the 120 Hz CTC tick provides deadlines and
process-time accounting but does not preempt a running userspace process.
`time()` converts the DS1302 calendar to seconds since 2000-01-01, and
`uptime()` compares the current RTC reading with the reading saved at boot.
Process accounting values are tick counts, not seconds or CPU-utilization
percentages.

## I2C Bus Recovery

The kernel performs I2C bus recovery in `kernel_loop.c` during idle halt. The function
`muzix_i2c_bus_check()` directly accesses the КР580ВВ55А PPI ports (0x60-0x63) — it does
not depend on `lib/i2c.c`:

1. **Check before halt**: catches lockups from interrupted transfers during the last execution slice.
2. **Check after halt**: catches lockups that occurred during an interrupt handler (CTC tick or UART interrupt) that led to the halt. This is unique to systems with bit-bang I2C and cooperative scheduling.

**Recovery procedure** (following the Linux `i2c_generic_scl_recovery()` pattern):
1. Release both SDA and SCL (set to OUTPUT, write 1 to both lines on PC0/PC4).
2. Check SCL is high first — if stuck low, abort (matches Linux `-EBUSY`).
3. If SDA is stuck low: perform 9 SCL clock pulses (PC4), checking SDA after each pulse.
   A slave stuck mid-byte will complete its transmission and release SDA when clocked.
4. Progressive retry with longer delays (50μs, 100μs, 200μs) if still stuck after initial recovery.

Userspace I2C code in `lib/i2c.c` has its own `i2c_bus_check()` with the same recovery logic
for use by non-kernel code (e.g., `apps/i2ctest.c`, `lib/lcd.c`).

## Filesystem and Limits

The ROM contains an image using the MINIX-v1 filesystem format (384 KB at
offset 0x20000, 512-byte blocks), served by a read-only ROM block device
(`kernel/rom_read.c`). The kernel intercepts `open()`, `readdir()`,
`read()`, `seek()`, `stat()` and `close()` for this filesystem directly
(via the ROM filesystem interceptors in `kernel/syscalls_ipc.c`), rather
than routing them to the FS server — the userspace FS server serves only the
console TTY descriptors 0/1/2 through the line discipline it owns, and the
`fs/` service layer under `fs/` is format-aligned with `tools/make_minixfs.rb`
on disk but is not linked into that server. Pipes are bounded and non-blocking
in the library; full MINIX IPC, its 69-call ABI, full PM/MM/FS server behavior,
and its command suite are not part of Muzix.

## Hardware-Code Origins

Selected Zeta-specific low-level routines and implementation patterns were
adapted from FUZIX: DS1302 pin transactions and port-shadow handling in
`platform/zeta-v2/rtc_ds1302.c`; NS16550 divisor setup in
`platform/zeta-v2/uart_io.s`; and Z80 saved-stack, switch-out, and idle patterns
in `kernel/proc_table.c`, `kernel/kernel_loop.c`, and
`platform/zeta-v2/z80_io.s`. FUZIX's Zeta platform sources also served as a
reference for bank-register and peripheral conventions.

These are selective adaptations. Muzix's process model,
syscall ABI, CTC divisor chain, memory allocator, and filesystem are built
on MINIX-derived ideas rather than copied. The detailed component notes and attribution are in
[`hardware-notes.md`](hardware-notes.md). Preserve applicable upstream FUZIX
license notices when redistributing derived source or binaries.

Build and validation commands are documented in [`../BUILD_REPORT.md`](../BUILD_REPORT.md).
