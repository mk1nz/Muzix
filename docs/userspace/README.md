# Muzix userspace

Muzix has its own small userspace for the Zeta V2 kernel. The kernel creates
the first user process by loading
`shell`; there is no separate `init` executable in the ROM image.

## Shell

The userspace shell is implemented in `shell/shell.c`. Its built-ins are
`cd`, `pwd`, `echo`, `help`, and `exit`. Other commands use the custom Muzix
fork/exec path.

The ROM build includes these external programs:

- `echo`, `cat`, and `ls`
- `date`, which reads or sets the DS1302 clock
- `top`, `uptime`, and `loadavg`
- `hwdiag`, for the diagnostics available through the current kernel ABI

The shell is PID 1 in this image. The separate `tools/bootloader.c` target is
not the ROM reset path; the image's reset stub is
`platform/zeta-v2/rom_boot.s`.

## Runtime and interfaces

`lib/` provides the userspace runtime, startup code, and wrappers for Muzix's
custom syscall ABI, not MINIX's. File operations use the mounted ROM-backed
filesystem, and terminal I/O uses the kernel TTY path. These interfaces are
Muzix implementations and do not provide MINIX message-passing, device-task,
or blocking-I/O semantics.

The scheduler is cooperative. Its 120 Hz CTC tick accounts for time and drives
deadlines, but does not preempt a running userspace process. `time()` reads the
DS1302 calendar, while `uptime()` compares it with the reading saved at boot.

## Build and validation

Run commands from the `Muzix/` directory:

```sh
make rom
make check
```

`make rom` builds and validates the 512 KiB ROM, including the kernel and
MINIX-v1-format filesystem image. `make check` runs the source gates and the
host-portable test subset. Host tests that cannot run on the host are reported
as not executed; neither command substitutes for an emulator or hardware boot
check.
