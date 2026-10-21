# Userspace shell status

The ROM starts the userspace shell as PID 1: `kernel/kernel_main.c` forks the
`shell` binary into slot 1 (`kernel/kernel_main.c:329`), and the loader reads
its image from the ROM-backed filesystem over its direct-ROM path —
`exec_open_path()` selects `exec_rom_open_path()` (`kernel/exec_loader.c:92-99`,
`:226-263`) because loader IPC mode is enabled only after all four boot forks
(`kernel/kernel_main.c:341`). The userspace FS server (slot 2) serves the
filesystem operations of running processes from that point on. The shell's
built-ins are `cd`, `pwd`, `echo`, `help`, and `exit`, and external commands
use fork/exec.

Verified 2026-10-14 by reading `kernel/kernel_main.c` and `kernel/exec_loader.c`.
An audit note of the 2026-10-13 era claims the boot shell load necessarily
routes through the FS server (a direct-ROM `exec_rom_stat("")` returning -1 in
the boot state); that spelling exists nowhere in the current loader, and the
boot forks run with `g_exec_use_ipc == false` (`kernel/exec_loader.c:23`,
flipped at `kernel/kernel_main.c:341`), so a working boot uses the direct-ROM
path. The claim is kept here as history, not as a description of the current
tree.

An earlier report dated 2026-09-21 described a boot loop on the first userspace
write and listed build targets that are not present in the current Makefile.
That report is superseded and is not evidence of a current failure.

## Current validation

The current source passed:

```sh
make check
make clean && make rom
```

The host runner executed 8 of 31 discovered test sources and all 8 passed; 23
target-specific or host-incompatible tests were not executed.

The built ROM was then run in z80pack with stdin forwarding. The shell printed
its banner, accepted `uptime`, and the external program returned an uptime
value; the trace contained no `MNT FAIL` marker. This exercises ROM-backed
filesystem loading and an external userspace command. It is not a hardware
test or proof of every command. (Smoke-test claims checked against the current
tree and the recorded emulator run in `AGENTS.md`, 2026-10-14.)

The ROM build is driven by `make rom` from this directory. It uses the
Makefile's application, shell, filesystem-image, and ROM targets; legacy
commands such as `make shell` and `make kernel-bin` are not supported targets.
