# Userspace shell status

The ROM starts the userspace shell as PID 1. `kernel/kernel_main.c` loads
`shell` from the ROM-backed filesystem; the shell's built-ins are `cd`, `pwd`,
`echo`, `help`, and `exit`, and external commands use fork/exec.

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
test or proof of every command.

The ROM build is driven by `make rom` from this directory. It uses the
Makefile's application, shell, filesystem-image, and ROM targets; legacy
commands such as `make shell` and `make kernel-bin` are not supported targets.
