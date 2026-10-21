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
custom syscall ABI. File operations on the ROM-backed filesystem
are served by the kernel directly (via the ROM filesystem interceptors in
`kernel/syscalls_ipc.c`), not routed to the FS server — the kernel owns the ROM
block device and the MINIX-v1 on-disk walk. Console I/O (descriptors 0/1/2)
is still handled by the FS server's TTY line discipline (`servers/fs/fs_server.c`).
The syscall wrappers in `lib/user_syscall.s` trap through `CALL 0x0030` with a custom ABI.

The entry contract every program is built against: `lib/crt0.s` loads `argv`
into DE and `argc` into HL before calling `_main` (a plain C call, not stack
arguments), the syscall wrappers in `lib/user_syscall.s` trap through
`CALL 0x0030`, and `read()` in `lib/libc.c` blocks by parking — on
`MUZIX_READ_AGAIN` (-11, `lib/tty_ioctl.h`) it calls `sys_park(240)`, two
seconds at a time, and returns every other answer, errors included. The ROM
image also carries the three boot servers `fs_server`, `mm_server` and
`pm_server`, which `kernel/kernel_main.c` forks and execs before the shell.

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
