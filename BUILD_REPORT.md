# Muzix build and validation status

The supported build entry point is the Makefile in `Muzix/`.

## Verified commands

The following commands completed successfully against the current source:

```sh
make check
make clean && make rom
```

`make check` completed all source gates. Its host runner discovered 31 test
sources: 8 executed and passed, while 23 were not executed because they require
target-specific memory behavior or dependencies that are not available in the
host harness. The runner reports those cases as not executed, not as passing
coverage.

`make clean && make rom` built and validated the 512 KiB Zeta V2 ROM. The image
contains the reset stub, the kernel beginning at ROM offset `0x0098`, and the
MINIX-v1-style filesystem image beginning at `0x20000`. The ROM validator
checked the filesystem structure and its packaged files.

## Emulator smoke test

The built ROM was also run in `emulator/z80pack-emu` with stdin forwarding. The
shell reached its banner, accepted `uptime`, and printed the elapsed time; the
external command therefore loaded and returned from the ROM-backed filesystem.
The trace had no `MNT FAIL` marker. This verifies an emulator boot path and one
userspace command, not hardware behavior or the complete command set.

The build emitted non-fatal Ruby constant-redefinition warnings from
`tools/kernel_data_base.rb` during repeated layout checks. The gates and ROM
validation still passed.

## Build targets

- `make rom` — supported complete ROM build and validation.
- `make check` — source gates plus host-runnable tests; does not build the ROM.
- `make test-host` — run the host test discovery/runner directly.
- `make test-kernel-layout` — validate the linked kernel code/data layout.
- `make clean` — remove generated build artifacts.

The ROM includes the kernel, userspace shell, selected applications, and a
read-only ROM-backed filesystem image. It does not include a suite of
standalone Z80 `.ihx` test binaries. For the current userspace inventory and
operating limits, see [`docs/userspace/README.md`](docs/userspace/README.md)
and [`PROJECT_STATUS.md`](PROJECT_STATUS.md).
