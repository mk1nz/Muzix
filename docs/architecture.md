# Muzix architecture

Muzix is an independent Z80 operating system that reuses selected MINIX 1.0
concepts and the MINIX-v1 filesystem format. Its syscall ABI and implementation
are Muzix-specific; Muzix is not a MINIX port.

## Runtime layers

- `kernel/` owns boot, process lifecycle, cooperative scheduling, syscall
  dispatch, executable loading, and process accounting.
- `mm/` owns physical-page allocation and mediated user-memory copies.
- `pm/` provides the implemented process-manager operations over the kernel
  service boundary.
- `fs/` provides the ROM-backed filesystem, file and directory operations,
  and TTY service.
- `platform/zeta-v2/` implements bank mapping, syscall entry, UART, RTC, and
  CTC hardware integration.
- `lib/`, `shell/`, and `apps/` contain the userspace runtime, PID 1 shell, and
  selected programs.

The kernel dispatches the custom Muzix syscall ABI. Some internal service
message types follow MINIX design concepts, but they do not make up the
complete MINIX IPC or server architecture.

## Memory and execution

The Zeta V2 board exposes four 16 KiB windows in the Z80's 64 KiB address
space. A process map keeps the kernel's window 0 and window 3 pages; the
process's allocated stack and text pages occupy windows 1 and 2. The physical
page identifiers are allocated by the memory manager. Segment virtual
addresses, physical page identifiers, and CPU-visible window addresses are
distinct values.

`exec_loader.c` allocates pages through MM, builds the process map, loads the
program into its text page, and installs its linker-derived entry point.
Temporary user-memory mappings save and restore the complete kernel map.
Platform bank-switch routines are the only layer that manipulates the bank
registers.

## Time and scheduling

The scheduler is cooperative; the 120 Hz CTC tick provides deadlines and
process-time accounting but does not preempt a running userspace process.
`time()` converts the DS1302 calendar to seconds since 2000-01-01, and
`uptime()` compares the current RTC reading with the reading saved at boot.
Process accounting values are tick counts, not seconds or CPU-utilization
percentages.

## Filesystem and limits

The ROM contains an image using the MINIX-v1 filesystem format, served by a
read-only ROM block device. Muzix implements selected file, directory,
metadata, and descriptor operations. Pipes are bounded and non-blocking;
full MINIX IPC, its 69-call ABI, full PM/MM/FS server behavior, and its
command suite are not part of Muzix.

## Hardware-code origins

Selected Zeta-specific low-level routines and implementation patterns were
adapted from FUZIX: DS1302 pin transactions and port-shadow handling in
`platform/zeta-v2/rtc_ds1302.c`; NS16550 divisor setup in
`platform/zeta-v2/uart_io.s`; and Z80 saved-stack, switch-out, and idle patterns
in `kernel/proc_table.c`, `kernel/kernel_loop.c`, and
`platform/zeta-v2/z80_io.s`. FUZIX's Zeta platform sources also served as a
reference for bank-register and peripheral conventions.

These are selective adaptations, not a wholesale port. Muzix's process model,
syscall ABI, CTC divisor chain, memory allocator, and filesystem are distinct
implementations. The detailed component notes and attribution are in
[`hardware-notes.md`](hardware-notes.md). Preserve applicable upstream FUZIX
license notices when redistributing derived source or binaries.

Build and validation commands are documented in [`../BUILD_REPORT.md`](../BUILD_REPORT.md).
