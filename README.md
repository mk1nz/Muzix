# Muzix

Muzix is an independent operating system for the Zilog Z80 and the Zeta SBC
V2. It draws on selected ideas from MINIX 1.0 and uses the MINIX-v1 filesystem
format, but it is not a MINIX port and does not implement the MINIX ABI.
Some low-level Zeta V2 support routines and techniques have been adapted from
FUZIX; Muzix is not a FUZIX port or fork.

https://github.com/user-attachments/assets/4791f19b-e926-4950-8006-2b37b8837c56

## What's included

- `kernel/` — entry point, kernel loop, process table, scheduling, system
  calls, and user-program loading.
- `mm/` — physical-page allocation and user-memory copying.
- `fs/` — filesystem, ROM-backed block device, and TTY service.
- `pm/` — a limited process-management service.
- `platform/zeta-v2/` — banked memory, context switching, UART, RTC, and CTC.
- `lib/`, `shell/`, `apps/` — user library, shell, and the `cat`, `echo`, `ls`,
  `date`, `top`, `uptime`, `loadavg`, and `hwdiag` programs.
- `tools/` — image converters and build checks.

The scheduler is cooperative. The CTC runs at 120 Hz and handles time
accounting and `park` deadlines, but does not preempt a running userspace
process. UART interrupts wake waiting processes. `time()` reads the calendar
from the DS1302 and converts it to seconds since 2000-01-01; `uptime()` uses an
RTC reading captured at boot.

Muzix does not provide the complete MINIX ABI or its full set of servers and
drivers. For example, `wait()` currently reaps an already-exited child once
or returns `-1`; pipes are bounded and do not block the calling process. For
implementation limits and platform rules, see [`AGENTS.md`](./AGENTS.md)

## Low-level code origins

Selected Zeta V2 hardware routines and patterns were adapted from FUZIX:
DS1302 transactions and write-only port shadowing, NS16550 divisor setup, and
Z80 stack-saving, switch-out, and idle patterns. FUZIX was also used as a
reference when checking the bank map and peripheral behavior. These are
selective adaptations, not a wholesale FUZIX port: Muzix has its own process
model, syscall ABI, CTC divisor chain, allocator, and filesystem. The
components and their attribution are documented in
[`docs/hardware-notes.md`](./docs/hardware-notes.md). Preserve applicable
upstream license notices when redistributing derived source or binaries. This
document is included in the ROM filesystem as `hw-notes`.

## AI use

Muzix is, in part, a “vibe-frankensteined” project: AI tools were used
extensively in developing the system, individual components, and
documentation. The code was reviewed and tested iteratively, but AI
involvement is no guarantee that it is bug-free. Before using it on hardware,
verify the build and system behavior.

## Building and testing

### Prerequisites

The ROM build and tests require:

- SDCC **4.5.0**: `sdcc`, `sdasz80`, and `sdldz80` (the Z80 assembler and
  linker). Install them as system commands available on `PATH`.
- `make`, `gcc` (the system clang compiler is also suitable), `sed`, `grep`,
  and standard Unix utilities.
- Ruby for build and validation scripts. Only Ruby's standard library is
  used; no third-party gems are required.

On macOS, install the Command Line Tools, SDCC, and Ruby:

```sh
xcode-select --install
brew install sdcc ruby
```

`xcode-select --install` provides `make`, clang (available as `gcc`), `sed`,
`grep`, and the tools needed to build the emulator. If SDCC 4.5.0 or Ruby is
already installed and available on `PATH`, you can omit that package from the
Homebrew command.

On Linux, install the system build tools and Ruby (for example,
`sudo apt install build-essential make ruby sdcc`). Make sure the installed
SDCC version is **4.5.0**; if your distribution provides a different version,
install or build SDCC 4.5.0 for your OS. Other versions have not been validated
for this project.

Check that the required tools are available:

```sh
sdcc --version
command -v sdasz80 sdldz80 make gcc ruby
```

The expected compiler version is `4.5.0`.

### Build

Run all commands from the `Muzix/` root:

```sh
make rom
make check
```

`make rom` builds and validates the 512 KiB ROM image. `make check` runs the
source checks and the subset of host tests that can run with the host's memory
model. The Makefile is the supported build entry point.

To build the emulator separately, you also need a C compiler and POSIX
pthreads (provided by the Command Line Tools on macOS or `build-essential` on
Debian/Ubuntu):

```sh
make -C emulator
```

Then launch it from the emulator directory:

```sh
cd emulator
./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -r -i
```
