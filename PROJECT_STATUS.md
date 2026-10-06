# Muzix Project Status

## Current State

Muzix is an independent Z80 operating-system implementation that reuses
selected MINIX 1.0 concepts and the MINIX-v1 filesystem format. It is not a
MINIX or FUZIX port. The source tree builds successfully with the focused Z80
target:

```text
Z80 target build: PASS
```

The repository includes the kernel process table and cooperative scheduler,
memory and filesystem services, userspace syscall wrappers, TTY/device
abstractions, and host-side regression tests. The current Makefile does not
build a suite of Z80 `.ihx` test binaries.

## Implemented Functionality

- Process lifecycle, scheduler state, fork/exec/exit/wait model, and system
  service dispatch.
- 120 Hz scheduler ticks, RTC-backed `time()`/`uptime()`, process accounting
  counters, child-time accumulation, and process-table credentials.
- Filesystem persistence with inode, directory, allocator, block cache, path,
  regular-file I/O, `creat`, `fstat`, hard links, `dup`, and link-aware unlink.
- Permission metadata and checks through `chmod`, `access`, open flags,
  `read`, and `write` descriptor modes.
- `getuid`, `getgid`, `setuid`, `setgid`, and per-process `umask` applied to
  `mkdir`, `creat`, and TTY `mknod`.
- Bounded non-blocking in-memory pipes with FIFO behavior and endpoint
  lifecycle.
- TTY character nodes, stateful `ioctl`, callback-backed non-blocking serial
  read/write, canonical editing with optional echo, and software XON/XOFF
  output flow control.
- Zeta V2 memory-bank and syscall-entry integration.
- Bootloader target binding for the Zeta V2 kernel bank map and NS16550 UART.
- Experimental UNA BIOS stage-1/stage-2 assembly and image scripts exist, but
  they are not wired into the Makefile build.
- `make rom` produces the 512 KiB Zeta V2 ROM: boot stub at offset `0`,
  kernel image beginning at `0x0098` in the 128 KiB kernel region, and the
  MINIX-v1-compatible filesystem image at `0x20000`.

## Validation

The following commands passed against the current source:

```sh
make check
make clean && make rom
```

`make rom` is the supported image build. `make check` completed all source
gates. The host runner discovered 31 test sources: 8 executed and passed; 23
were not executed because they require target-specific behavior or are not
portable to the host harness. These build commands do not prove an emulator
or hardware boot.

The built ROM was also smoke-tested in z80pack with stdin forwarding: the
userspace shell started, accepted `uptime`, and the external program printed a
result from the ROM-backed filesystem. The boot log had no `MNT FAIL` marker.
This is emulator evidence for one boot and command, not a hardware test or
proof of the entire userspace command set.

## Known Boundaries

- `time()` is converted from the DS1302 calendar to seconds since 2000-01-01;
  `uptime()` subtracts the RTC reading saved at boot. The scheduler tick is
  separate and runs at 120 Hz.
- `UTIME` and `STIME` are per-process accounting counters in 120 Hz ticks,
  charged around syscall boundaries. The cooperative scheduler does not
  time-slice a running process.
- Pipes are bounded and non-blocking; MINIX suspend/revive queues are not
  implemented.
- Zeta V2 UART register binding and the emulator serial bridge use
  NS16550-compatible ports. The line discipline covers canonical editing,
  echo, EOF, and XON/XOFF; job-control signal generation and delayed output
  queues are not implemented. Userspace `read()` parks and retries on empty
  input; this is not a per-device blocking queue.
- Bounded process-directed SIG/ABORT handling is wired: signals 1..16 are
  queued per process through the `SYS_SIGNAL` userspace ABI, delivered at the
  kernel loop boundary with default termination status, and ABORT enters
  shutdown. Delivery is permitted to root or a process with matching user
  credentials. User handlers, ignored signals, process groups, and signal
  frames are not implemented.
- The UNA scripts are separate from the ROM build; do not infer that the
  ordinary `make rom` path exercises UNA firmware handoff.
- Generic block special files, other character devices, full MINIX IPC, the
  complete MM service, and the full 69-call MINIX ABI remain out of scope for
  this source-level milestone.

## Historical Boot Failure Investigation (2026-09-18)

**Symptom:** System boots, shell_task runs locally, but `exec_load` crashes with
`cache_get: bad args` -> `_halt`. The log shows `kernel_main` is called multiple
times in succession.

### Observed execution trace (emulator/z80pack-emu -s muzix emulator/muzix.rom)

```
MUZIX kernel booting...        # kernel_main call #1
Mounting ROM filesystem...
DEBUG: Before volume init
DEBUG: After volume init
DEBUG: volume_mount: start
DEBUG: superblock_load: calling cache_get
DEBUG: cache_get: entry
DEBUG: cache_get: not in cache, finding empty slot
DEBUG: rom_read: entry
rom_read: done
MUZIX kernel booting...        # kernel_main call #2
Mounting ROM filesystem...
DEBUG: Before volume init       # interrupted inside muzix_fs_volume_init
MUZIX kernel booting...        # kernel_main call #3
Mounting ROM filesystem...
FS mount FAILED
Services initialized
Syscalls bound
Kernel ready
Calling exec_load shell...
exec_load: ENTRY
MUZIX kernel booting...        # kernel_main call #4
Mounting ROM filesystem...
DEBUG: cache_get: bad args     # crash
PC: 0x00B3 SP: 0xFE02
```

Final PC=0x00B3 is `halt` in `kernel_entry.s:34`, meaning the 4th `kernel_main`
returned normally into `_halt`.

### Original root-cause hypothesis: write-only bank selector readback

The original investigation attributed the failure to `rom_read_block` reading
port `0x79` and restoring the returned value. That explanation describes the
reported earlier snapshot, not the current production implementation.

### Current ROM-read path

`kernel/rom_read.c` obtains the MM service from `g_block_dev.context` and calls
`muzix_mm_copy_rom_page_to_kernel()`. That MM entry point delegates to the
platform copy primitive, whose temporary-map path restores the full saved
kernel map. The test-only `zeta_save_kernel_map()` and
`zeta_restore_map()` helpers are not the production ROM-read path.

The current emulator smoke test reaches the shell and runs the external
`uptime` program from the ROM-backed filesystem. This establishes the current
boot path; it does not retroactively prove every detail of the old trace.

## Historical Exec Loader Bank Corruption Investigation (2026-09-25)

**Symptom:** FS mount completes successfully, but `exec_load` crashes during the
`muzix_fs_service_fstat` setup phase. The emulator shows "EXEC: open handle=00"
but never reaches the fstat call. Spurious `OUT $79=0x03` and `OUT $7A=0x23`
bank-switch port writes occur between the debug print and the fstat call,
causing stack corruption that re-enters `kernel_main` (boot loop).

### Observed execution trace

```
FS mounted successfully
DEBUG: Before exec_load
EXEC: open handle=00
Z                                   # inline asm marker after exec_loader prints
[DEBUG] Read from 0x4260: banks=[20,21,22,23]
[PORT OUT $79] Bank 1 = 0x03       # <-- corrupts window 1 bank
[PORT OUT $7A] Bank 2 = 0x23       # <-- corrupts window 2 bank
[DEBUG] Read from 0x4260: banks=[20,03,23,23]
[PORT OUT $78] Bank 0 = 0x20       # restore attempt (partial)
[PORT OUT $79] Bank 1 = 0x21
[PORT OUT $7A] Bank 2 = 0x22
[PORT OUT $77] Bank 0 = 0x23       # restore attempt
KM: kernel_main ENTRY               # <-- re-entry, boot loop
```

### Original hypothesis

1. **Epilogue at 0x4261**: The CPU reads address `0x4260` then executes
   `OUT $79` and `OUT $7A`. Disassembly at 0x4261 reveals this is the
   epilogue of `_muzix_proc_table_exec` (`kernel/proc_table.c`):
   ```
   0x4261: LD SP, IX ; POP IX ; POP HL ; POP AF ; JP (HL)
   ```
   This function epilogue is being executed, indicating stack corruption.

2. **No OUT instructions in exec_loader.c**: The assembly between the inline
   asm marker and the `call _muzix_fs_service_fstat` contains only data loads
   and a stack push — none of which produce port I/O.

3. The original report attributed the bank writes to UART output using ports
   `0x79`/`0x7A`. That attribution is not current-source evidence:
   `platform/zeta-v2/uart_io.s` currently binds the NS16550-compatible UART to
   ports `0x68`–`0x6D`.

4. The old trace records a boot-loop symptom, but does not establish the
   currently active cause. Do not use it as a description of present runtime
   behavior.

The current ROM build and source checks pass. Emulator or hardware boot
behavior still requires a separate runtime check.
