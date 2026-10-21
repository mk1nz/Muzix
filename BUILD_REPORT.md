# Muzix build and validation status

The supported build entry point is the Makefile in `Muzix/`.

## Verified commands

The following commands completed successfully against the current source:

```sh
make check
make clean && make rom
```

`make check` completed all source gates. Its host runner (`tools/host_tests.rb`,
which discovers `test_*.c` under `kernel/`, `fs/`, `mm/`, `pm/` and
`platform/zeta-v2/`) discovered 31 test
sources: 8 executed and passed, while 23 were not executed because they require
target-specific memory behavior or dependencies that are not available in the
host harness. The runner reports those cases as not executed, not as passing
coverage.

`make clean && make rom` built and validated the 512 KiB Zeta V2 ROM. The image
contains the reset stub, the kernel beginning at ROM offset `0x0098`, and the
MINIX-v1-style filesystem image beginning at `0x20000`. The ROM validator
checked the filesystem structure and its packaged files.

## Architecture Status

**Microkernel architecture implemented:**
- Kernel: `_CODE` 45,631 bytes (~44.6 KB) + `_DATA` 5,799 bytes, 13,657 bytes
  headroom to `0x10000` (source: `build/kernel.map`, figures as of 2026-10-14;
  re-read with `make test-kernel-layout` after any change to code size)
- IPC core: Minix-style send/receive/both with priority queues
- 3 userspace servers: FS (slot 2, `MUZIX_PROC_FS`), MM (slot 3, `MUZIX_PROC_MM`),
  PM (slot 4, `MUZIX_PROC_PM`) — registered with those server numbers in
  `include/muzix_ipc.h:206-208` and forked before the scheduler runs
  (`kernel/kernel_main.c:295-325`)

**Current boot state (verified 2026-10-14 against the tree):** the ROM boots to
the userspace shell. `kernel_main()` forks all three servers — FS, MM, PM — and
the shell (slot 1, PID 1) before entering `muzix_kernel_loop_run()` exactly
once (`kernel/kernel_main.c:295-348`), and the recorded emulator run reaches the
shell banner and runs external `uptime` with no `MNT FAIL` marker (see
`PROJECT_STATUS.md` and the emulator smoke test in `AGENTS.md`).

## Emulator smoke test

The built ROM runs in `emulator/z80pack-emu` with stdin forwarding
(`-s muzix build/muzix-zeta-v2.rom -i`). The kernel boots to the userspace
shell, accepts `uptime`, and the external program returns elapsed time; the
recorded run shows no `MNT FAIL` marker. That is recorded emulator evidence for
one boot and one external command, not a hardware test (recorded run summarized
in `AGENTS.md`; reconciled 2026-10-14).

**Historical snapshot (2026-10-13, microkernel-transition era — not the current
state):** the servers then failed to load at boot (`FS SVR FAIL`, `MM SVR FAIL`,
`PM SVR FAIL`) and the kernel fell back to the in-kernel shell; the trace ran
`KM`, `MNT`, `SVC`, `RDY`, `IPC` and then the server failures. The failure
strings survive as the failure branches of the boot forks
(`kernel/kernel_main.c:299`, `:310`, `:320`) and print only when a
`kernel_fork_server()` load fails. Kept as history.

## Build targets

- `make rom` — supported complete ROM build and validation.
- `make check` — source gates plus host-runnable tests; does not build the ROM.
- `make test-host` — run the host test discovery/runner directly.
- `make test-kernel-layout` — validate the linked kernel code/data layout.
- `make clean` — remove generated build artifacts.

The ROM includes the kernel, userspace shell, selected applications, three
userspace servers (FS, MM, PM), and a read-only ROM-backed filesystem image.
It does not include a suite of standalone Z80 `.ihx` test binaries. For the
current userspace inventory and operating limits, see
[`docs/userspace/README.md`](docs/userspace/README.md) and
[`PROJECT_STATUS.md`](PROJECT_STATUS.md).