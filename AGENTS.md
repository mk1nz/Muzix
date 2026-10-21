# AGENTS.md — Muzix Z80 Operating System

## Project Overview

**Muzix** is an operating system for the Zeta SBC V2, built on ideas from
MINIX 1.0 (message-passing IPC, priority queues, MINIX-v1 filesystem format)
and adapted Zeta V2 hardware routines from FUZIX (DS1302, NS16550, bank-switch
patterns). Muzix has its own process model, syscall ABI, scheduler, allocator,
and filesystem.

Run build and validation commands from this directory (`Muzix/`). The
workspace layout is:

| Path               | Purpose                                      |
|--------------------|----------------------------------------------|
| `Muzix/` | This project: kernel, MM, PM, FS, userspace, tools, and emulator |
| `docs/` | Workspace-level hardware, process-memory, architecture, and parity notes |

---

## Platform Reference (authoritative, this file)

### Z80 Platform

- **CPU**: Zilog Z80, 8-bit registers, 16-bit address space (64 KB visible).
- **SDCC**: 4.5.0; the Makefile uses `-mz80 --opt-code-size` for kernel C and
  `-mz80` for userspace C. Assembly and linker output are part of the target.
- **Memory**: 512 KiB of 16 KiB physical pages, with four 16 KiB windows in
  the 64 KiB Z80 address space. Physical page IDs `0x20–0x3F` are allocator
  RAM; `0x40+` is unmapped/ROM in the platform model. Do not infer ownership
  or availability from IDs outside the allocator API.
- **Bank switching**: I/O ports `0x78`–`0x7B` (Bank 0–3 selectors); `0x7C` =
  MPGENA (paging enable, bit 0). Ports are **write-only** in strict hardware
  mode — never read them back for saved state.
- **ROM image**: 524 288 bytes (512 KB), kernel at offset `0x0098` in a 128 KiB
  kernel region, MINIX-v1-format filesystem at `0x20000` (384 KB). Boot stub copies ROM
  pages 0–3 → RAM pages 32–35 via `LDIR`, then `jp 0x0098`.
- Writes to ROM-mapped addresses are silently dropped.

### SDCC Calling Convention

The project's normal C modules are compiled with plain `-mz80`; the kernel's
linked SDCC runtime also contains `sdcccall(1)` entry points. Do not apply one
convention to both.

For plain `-mz80` C calls, verified by
`tools/check_z80_call_convention.rb`:

- `param1` → HL, `param2` → DE, `param3` → BC; later parameters use the stack.
- 8-bit return → A; 16-bit return → DE.
- 32-bit return is not verified for this target. Probe the exact function before
  relying on its return registers.

Assembly that calls a plain `-mz80` C function must load parameters in those
registers and call the function normally. Assembly implementations called by C
must return values in the registers expected by the compiled caller. The
convention check is part of `make check` and `make rom`.

The runtime's `sdcccall(1)` indirect-call helpers (`___sdcc_call_iy` and
`___sdcc_call_hl`) are distinct from the plain C calling convention. A C
function pointer in kernel memory is not a user syscall mechanism; userspace
wrappers must construct the platform syscall frame.

In `lib/crt0.s`, `_main` is a normal C call. Its arguments must be loaded into
the compiled convention's registers; pushing them alone does not pass
`argc`/`argv` to the callee.

### Memory Model

- `process_map_t` = 4 bank pages (`uint8_t` each).
- `muzix_mem_map_t` = `mem_vir`, `mem_phys` (32-bit), `mem_len`.
- Physical page IDs, segment virtual addresses, and Z80 window addresses are
  separate concepts.
- Temporary bank mappings must save and restore the **complete** kernel map
  (including the common/kernel window).
- `_DATA` base must be **above the end of `_CODE`**, not merely past the SDCC
  library symbols. The kernel's code and data share one 64 KiB address space,
  and `sdldz80` silently overlaps the two areas instead of erroring when they
  do not fit — the ROM can build, then the kernel can zero its own code at
  runtime. `_DATA` is derived from the linked `_CODE` size by
  `tools/kernel_data_base.rb`; do not copy old map addresses or headroom figures
  from notes into code or documentation. `make test-kernel-layout` reports the
  current extents, requires at least 1024 bytes of headroom, and rejects overlap
  or a populated `_HOME`.
- **The kernel holds no 32-bit multiply, and pulling in SDCC's `__mullong`
  would be worse than a size cost.**  Verified against `build/kernel.map`: the
  image contains no `__mulint`, `__mullong`, `__divuint`, `__divulong` or
  `__moduint`, and `KERNEL_LINK_ORDER` in the `Makefile` links `.rel` files
  only - no `.lib` - so a call to one of them resolves to nothing.  The
  plausible workaround is worse than the problem, and it is worth writing down
  because it passes both gates:

  `sdz80_runtime.s` is the tempting place to look and it does **not** help - it
  defines `___sdcc_call_hl`, `___sdcc_call_iy` and the `--stack-auto` frame
  entry `___sdcc_enter_ix`, and no multiply helper, so `__mullong` does not
  resolve there.  Supply it from SDCC's own
  `lib/z80/z80.lib` and the link succeeds, and produces this:

      _CODE  0x0098  56 bytes
      _HOME  0xEA00  227 bytes      <-- s__HOME == s__DATA

  `_HOME` is a third area, and `sdldz80` places it at the `_DATA` base.  So the
  227 bytes of multiply land **inside the data segment**, where `gsinit` zeroes
  them.  Reproduce it by linking any module that needs `__mullong` with
  `-b _CODE=0x0098 -b _DATA=0xEA00` and reading the map.

  **This is now a build failure, not a note.**  `tools/check_kernel_layout.rb`
  reads `l__HOME` and fails if it is non-zero, printing `_HOME 227 bytes (CODE
  OUTSIDE _CODE)` among its other lines; the check runs from
  `make test-kernel-layout`, which is a dependency of `test-rom-image` and of
  `check`.  The rule is that the kernel link must not populate `_HOME` at all,
  rather than a size limit: even placed correctly it would be code below
  `_DATA`, which every figure in this file - the headroom, the overlap check,
  the "_CODE ends here" claim - assumes does not exist.

  What does work is the decomposition `platform/zeta-v2/tick.s` already uses for
  its 32-bit accumulate: four byte-sized partial products, each small enough to
  compute in a 16-bit register.  SDCC emits `__mulint` (17 bytes, and it
  resolves when a `.lib` is linked) for a `uint16_t * uint16_t`, so a hand-built
  product out of four `__mulint` calls costs about 158 bytes - and drags in the
  helper, which the kernel link cannot do.

  **Cut both operands into `uint8_t` halves instead and none of that is
  needed.**  `load_mul()` in `kernel/load.c` is the working example: four
  `uint8_t * uint8_t` products that SDCC expands inline as shift-add, recombined
  with carries that are also 16-bit, so a whole 28-bit Q14 product exists only in
  registers and `build/kernel.map` still shows no `__mulint`.  The two traps in
  the recombination, both of which were got wrong here before they were caught:
  `p1 + p2` can exceed 65535 (both are up to 65025, because the operands are
  limited to Q14 but their halves are full bytes), so the carry out of it has
  weight `65536 >> 8 = 256` in the high half rather than 1; and the final carry
  is detected as `t < p0` after `t = low + p0`.  `kernel/test_load.c` checks the
  result against the host compiler over 256 x 256 operand pairs.  The
  same routine written in Z80 does not do much better: the obvious 16-step
  shift-add needs a 32-bit accumulator, and on this Z80 only HL, IX and IY
  accept `add rr,rr` - not `adc` - while `ld dd,ss` accepts only BC, DE and SP,
  so BC cannot be loaded from HL and IX cannot be loaded from DE.  The assembly
  falls back to the same four-partial-product form, around 130 bytes, in code no
  host test can reach.
- **The kernel DOES hold the three load windows.  An earlier version of this
  file said it could not, and it was wrong.**  `kernel/load.c` is in
  `KERNEL_LINK_ORDER`, and `SYS_LOAD` reads it.

  `kernel/load.c` is linked into the kernel and `SYS_LOAD` reads its three Q14
  busy-fraction windows. Its code size and available headroom vary with the
  build; inspect the current `.rel` and `build/kernel.map`, and use
  `make test-kernel-layout` as the enforced check rather than relying on old
  byte counts. `load_mul()` splits operands into byte-sized partial products to
  avoid unavailable wide multiply helpers.

  Three things this cost that are worth knowing before anyone makes them again:

  - **One multiply per window is not the same recurrence as two, and the cheap
    one is wrong.**  `acc += (busy - acc) * d` is `acc*(1-d) + busy*d`: it puts
    the retention `d` on the NEW sample and the complement on the old one.  It
    is a plausible-looking saving and it is the recurrence backwards.  Over a
    short interval nobody can see it; over a two-minute interval the 1-minute
    window must fall to `exp(-2)` = 0.135 while the 15-minute window stays near
    `exp(-2/15)` = 0.875, and the inverted form does exactly the reverse - the
    long window collapses and the short one barely moves, so a stale reading
    looks like a fresh one.  The real step is `acc*d + busy*(1-d)`, two
    multiplies.  `kernel/test_load.c` case 3 is what caught it.
  - **The decay exponent is SECONDS, not ticks, and the two differ by 120.**
    The factors are `exp(-1/tau)` per second; raising them to a power measured in
    120 Hz ticks raises them 120 times too often and the windows decay to nothing
    within seconds - the 15-minute window reaches its final value inside 30.
    The correct reason a per-tick factor cannot work at all is the one recorded
    below: `exp(-1/108000)` in Q14 is 16384, exactly 1.0.
  - **`KERNEL_LINK_ORDER` links `.rel` files only, never a `.lib`, so a helper
    SDCC emits and the tree does not carry does not resolve at all.**  Writing
    the ticks-to-seconds conversion as `(span + 120u/2) / 120u` compiled to a
    single `call __divulong`, because `span` is a `uint32_t` and SDCC widens
    `unsigned int` to 32 bits.  The link printed
    `Undefined Global '__divulong' referenced by module 'load'` and the second
    pass of the `build/kernel.map` rule is not allowed to fail quietly, so
    `make` stopped with no further explanation.  No host test sees this, because
    gcc inlines the divide.  `load_seconds()` does it by subtraction instead -
    about 25 bytes, at most 546 iterations, once per read.

  The windows are Q14 busy fractions of wall time, not the Unix load average,
  and that is a property of the machine rather than a shortcut: this scheduler
  is cooperative, so a running process is not waiting for the processor, and a
  process parked waiting for a keystroke is not waiting for the processor
  either.  What the kernel can measure is time, and it already accounts for it.
  The state is in `_DATA`, so the figures survive every process exit, and the
  update happens on read rather than in the tick handler - `tick.s` argues at
  length why the handler's data cells all sit in window 0 (they are linked in
  `_CODE`, which is what `build/kernel.map` and `tools/check_window0.rb`
  verify), while the pushes it makes land on window 3 under the kernel map or
  on window 1 when a process is interrupted.

- **The `GRANULE = 0x100` alignment was never implicated.** An earlier
  version of this file blamed it: set `GRANULE = 1` and the machine broke 3 runs
  out of 3, cause not found. That was wrong. The fault was
  `muzix_tick_add_process_time()` in `platform/zeta-v2/tick.s`, which restamped
  through `DE` after `DE` had been reused to hold the tick difference, so the
  two bytes of the new stamp were stored at address `elapsed` - a small 16-bit
  number - instead of at `*stamp`. With `elapsed = 0` the writes landed at
  0x0000 and 0xFFFF and nothing happened; with a live tick `elapsed` swept
  0,1,2,... and the routine walked two bytes at a time up window 0, through the
  boot stub, the IM1 vector at 0x0038 and `tick.s` itself, and finally into
  `_DATA` at the tick counter, which amplified it. With the restamp fixed both
  settings are clean, so the granule is unmeasured rather than known-bad.
  `tools/kernel_data_base.rb` carries the full correction.

- `make test-kernel-layout` (a dependency of `test-rom-image`) enforces this and
  fails the build on any overlap. Run it after any change to code size.
- Global structs (`g_volume`, `g_loop`, `g_fs`) must not overlap with code.
- The kernel does **not** have room for inline tracing: code + data must fit in
  64 KiB. Use the ring buffer in `platform/zeta-v2/trace.c` (dumped with the
  fallback shell's `trace` command), not scattered `dbg_puts` call sites.

### Memory Access Constraints

- **User-page transfers and exec image placement go through the
  memory-manager API.** Kernel code may access its currently mapped kernel
  memory directly; boot code and designated platform transition/copy routines
  are deliberate low-level exceptions.
- Do not add ad-hoc bank-register writes or bank-crossing trampolines in higher
  layers. Use the established MM server and platform routines; the assembly
  transition in `kernel_entry.s` and copy primitives in `context_switch.c` are
  intentional parts of that mechanism.
- **The platform copy primitives are the MM server's alone.** `zeta_copy_user_to_kernel`,
  `zeta_copy_kernel_to_user`, `zeta_copy_page_to_kernel` and
  `zeta_copy_rom_page_to_kernel` program the bank registers, so they are reached
  only through the `muzix_mm_*` entry points in `mm/`. The **MM server runs in
  userspace (slot 3)** and exposes these via IPC. `tools/check_memory_manager.rb`
  enforces this and derives both the primitive set and the MM module set from the
  sources, so adding a caller is a build failure rather than a review comment.

### MINIX 1 Reference Architecture

The following describes MINIX 1 for comparison; it is not Muzix's kernel
architecture.

- **Message-passing kernel**: SEND=1, RECEIVE=2, BOTH=3, ANY.
- **Three scheduling queues**: TASK_Q, SERVER_Q, USER_Q.
- `struct proc`: `p_reg`, `p_sp`, `p_pcpsw`, `p_flags`, `p_map[NR_SEGS]`.
- `struct mem_map`: `mem_vir`, `mem_phys`, `mem_len` (in clicks).
- `umap()` converts virtual → physical.
- `sys_task` handles: `SYS_FORK`, `SYS_NEWMAP`, `SYS_EXEC`, `SYS_XIT`,
  `SYS_GETSP`, `SYS_TIMES`, `SYS_ABORT`, `SYS_SIG`, `SYS_COPY`.
- Muzix MM instead has a page allocator (`muzix_mm_alloc_page` / `muzix_mm_free_page`) over
  physical page IDs, plus `muzix_mm_newmap()` and the `muzix_mm_load_map*()`
  pair. **Not `alloc_mem`/`free_mem`** — those MINIX names appear nowhere in the
  tree — and there is no `mm/exec.c`: header parsing and segment loading are
  `kernel/exec_loader.c`.
- MINIX executable header: 8 longs, magic `0x04000301`, text/data/bss/total
  sizes. Muzix's loader consumes this file format.

### Muzix Subsystems

| Subtree       | Contents                                                       |
|---------------|----------------------------------------------------------------|
| `kernel/`     | main, loop, startup, proc_table, **ipc**, syscalls, exec_loader, bootstrap, init_task, load, shell. **There is no `kernel/scheduler.c`** — the priority queues and the round-robin pick are `muzix_proc_table_pick_next()` in `proc_table.c` |
| `mm/`         | mm_service (kernel-linked), mm_copy, mproc_model, mem_map, memory — the userspace MM server at slot 3 is `servers/mm/mm_server.c` |
| `fs/`         | fs_service, volume, block_device, block_cache, directory, inode_table, allocator, file_io, path, superblock, tty_device, fs_clock — the userspace FS server at slot 2 is `servers/fs/fs_server.c`; ROM block reads are `kernel/rom_read.c`, not `fs/` |
| `pm/`         | **pm_server.c** (userspace server at slot 4) |
| `platform/zeta-v2/` | bank_io, **context_switch** (owns the copy primitives), kernel_bank_model, kernel_entry, rom_boot, rtc_ds1302, sdz80_runtime, syscall_entry, syscall_runtime, syscall_vector, trace, uart_io, una_boot, una_stage2, z80_io, **tick** |
| `lib/`        | libc, syscall, user_syscall, crt0, math, sdcc_runtime, tty_ioctl, proc_info, romfs_data, **server** |
| `servers/`    | fs_server, mm_server, pm_server — userspace microkernel servers |
| `tools/`      | ihx_to_bin.rb, make_minixfs.rb, add_exec_header.rb, bootloader, and the `check_*.rb` gates |

### MINIX Reference Model in Tests

- `platform/zeta-v2/test_kernel/minix_adapter.*` models MINIX-style segment
  mapping for isolated tests; it is not part of the production ROM link.
- The production process mapping and copy paths are implemented by
  `mm/mproc_model.c`, `mm/mm_copy.c`, `kernel/exec_loader.c`, and the **MM server**
  userspace (slot 3) platform copy primitives. Do not treat the test adapter as
  the live path.

### Build System

- `make rom` is the build: kernel link, userspace, MINIX-v1-format filesystem, 512 KB Zeta
  V2 flash image, and the validators. `make all` is an alias for it.
- `make check` runs every gate (`test-kernel-layout`, `test-window0`,
  `test-syscall-context`, `test-user-pointers`, `test-link-scripts`,
  `test-emulator-sources`, `test-userspace-layout`, `test-userspace-runtime`,
  `test-memory-manager`, `test-park-site`, `test-proc-identity`,
  `test-host`). The Z80 calling convention check (`test-z80-call-convention`)
  is included in `test-rom-image`, which runs as part of `make rom`, not in
  `make check`.
- ROM image: 524 288 bytes, kernel at `0x0098`.
- Boot stub: copies ROM pages 0–3 → RAM pages 32–35 via LDIR, then `jp 0x0098`.
- `ihx_to_bin.rb` handles SDCC's `00:` prefix Intel HEX records.
- User binaries carry linker-derived `_main` entry metadata (via `add_exec_header.rb`).

---

## Architecture Summary

### Architecture Summary

#### System Stack

```
Muzix userspace (shell + selected applications; custom syscall ABI)
        │
        ▼
Kernel (minimal microkernel: IPC, scheduler, syscall entry, exec loader)
        │
        ▼ IPC
Userspace Servers (launched at boot via fork/exec)
  ├─ FS Server (slot 2) — filesystem operations
  ├─ MM Server (slot 3) — physical-page allocation and mediated user-memory copies
  └─ PM Server (slot 4) — process-manager operations
        │
        ▼
Zeta V2 backend (bank mapping, syscall entry, UART, RTC, CTC)
        │
        ▼
Zeta SBC V2 (four 16 KiB windows; bank selectors $78–$7B; MPGENA $7C)
```

The kernel's image size is not a fixed figure and this file deliberately
carries no byte count: read the current extents from `build/kernel.map` or
`make test-kernel-layout`, and do not copy a size out of notes into code or
documentation. As of this writing the map reports `_CODE` 45,631 bytes (≈44.6
KB) and `_DATA` 5,799 bytes, with `_DATA` derived from the linked `_CODE`
extent by `tools/kernel_data_base.rb`.

### Z80 Memory Layout (Zeta SBC V2)

| Address range  | Bank register | Notes                                    |
|----------------|---------------|------------------------------------------|
| $0000–$3FFF    | $78           | Bank-controlled window                   |
| $4000–$7FFF    | $79           | Bank-controlled window                   |
| $8000–$BFFF    | $7A           | Bank-controlled window                   |
| $C000–$FFFF    | $7B           | Fixed kernel/common page window at runtime |

**Key invariant**: User process maps retain kernel page `0x20` in window 0 and
page `0x23` in window 3; windows 1 and 2 use process pages. Pages `0x20–0x23`
together form the four-window kernel map, not one common page. See
`docs/hardware-notes.md` in this tree and `docs/process-memory.md` in the
workspace-level documentation for current component and mapping summaries.

### Kernel Linker Layout

The kernel link is driven by `Makefile` (`build-kernel`), **not** by
`kernel/kernel.lk`, which is dead config that nothing invokes. Edit the
`-b` flags in the `sdldz80` line instead.

```
-b _CODE = 0x0098     (kernel entry point, fixed)
-b _DATA = derived from the linked _CODE extent
```

**Do not type that `_DATA` value.** `tools/kernel_data_base.rb` prints it: it
takes `l__CODE` from `build/kernel.map`, adds a 0x40 margin, and rounds up to
the configured `GRANULE`. The link runs twice and the second pass uses that
value.
Editing the number by hand is how `_DATA` gets pinned below the end of `_CODE`,
which `sdldz80` accepts silently.

`kernel/kernel.lk` also lists `bank_call.s`, which no longer exists, and pins
`-b _DATA = 0xDC00`, which is a third wrong number in a file nothing reads. The
authoritative module list is `KERNEL_S_MODULES` / `KERNEL_LINK_ORDER` in the
Makefile; keep all three in step when adding or removing an assembly module.

### Userspace Linker Layout (apps/shell `.lk` files)

```
-b _CODE = 0x8000
(no -b _DATA: _DATA follows _CODE by the linker default)
```

**There is no `-b _DATA = 0x9000` any more, and pinning one is a
memory-corruption bug.** `shell/shell.lk` says so in the file: a fixed 0x9000
sat inside the shell's 0x1833-byte text and silently overwrote `_sys_times`,
`_sys_signal` and `_sys_open` at exec time. The text is window 2 of the process
map and `_DATA` is window 3, so a `_DATA` base inside the text range is not a
link error — it is a program whose library state is its own instructions. The
same holds for every `apps/*.lk`.

### Syscall ABI

Kernel syscalls (internal, message-passing style):

| Number | Name        | Description                  |
|--------|-------------|------------------------------|
| 0      | SYS_NULL    | No-op                        |
| 1      | SYS_COPY    | Copy between processes       |
| 10     | SYS_YIELD   | Re-queue current process     |
| 11     | SYS_EXIT    | Exit process                 |

User syscalls (userspace-facing, custom numbering):

| Number | Name            | Number | Name        | Number | Name            |
|--------|-----------------|--------|-------------|--------|-----------------|
| 1      | SYS_FORK        | 10     | SYS_OPEN    | 30     | SYS_SYNC        |
| 2      | SYS_EXEC        | 11     | SYS_CLOSE   | 31     | SYS_SIGNAL      |
| 3      | SYS_EXIT        | 12     | SYS_READ    | 32     | SYS_CHMOD       |
| 4      | SYS_WAIT        | 13     | SYS_WRITE   | 33     | SYS_ACCESS      |
| 5      | SYS_GETPID      | 14     | SYS_SEEK    | 34     | SYS_MKNOD       |
| 6      | SYS_GETPPID     | 20     | SYS_MKDIR   | 40     | SYS_READDIR     |
| 7      | SYS_TIME        | 21     | SYS_RMDIR   | 41     | SYS_DUP         |
| 8      | SYS_TIMES       | 22     | SYS_CHDIR   | 42     | SYS_GETUID      |
| 9      | SYS_GETRTC      | 23     | SYS_GETCWD  | 43     | SYS_GETGID      |
|        |                 | 24     | SYS_UNLINK  | 44     | SYS_SETUID      |
|        |                 | 25     | SYS_RENAME  | 45     | SYS_SETGID      |
|        |                 | 26     | SYS_STAT    | 46     | SYS_PIPE        |
|        |                 | 27     | SYS_FSTAT   | 47     | SYS_IOCTL       |
|        |                 | 28     | SYS_CREAT   | 60     | SYS_UMASK       |
|        |                 | 29     | SYS_LINK    | 48     | SYS_LOAD        |
|        |                 | 15     | SYS_SETRTC  | 49     | SYS_PARK        |
|        |                 | 16     | SYS_PROCTAB | 50     | SYS_IPC_SENDREC |
|        |                 |        |             | 51     | SYS_IPC_COPY_TO_USER |
|        |                 |        |             | 52     | SYS_IPC_COPY_FROM_USER |
|        |                 |        |             | 53     | SYS_IPC_SRV_COPY_TO_CLIENT |
|        |                 |        |             | 54     | SYS_IPC_SRV_COPY_FROM_CLIENT |
|        |                 |        |             | 55     | SYS_IPC_SEND    |

**Note**: Muzix defines a custom syscall numbering scheme that is NOT Minix
compatible (Minix has 69 call slots with different numbering, and Muzix's own
numbering has gaps: 48 numbers spread over 1–60 — the 42 above plus the six IPC
calls 50–55 defined in `lib/syscall.h` and dispatched in
`kernel/syscalls_ipc.c`). See
`docs/minix-muzix-audit.md` at the repository root for the full audit.

**Two numbering schemes, one router.** The kernel-internal `MUZIX_SYS_*`
numbers (`kernel/syscalls.h`: 0/1/10/11) are a separate namespace from the user
numbers above, and the low end collides numerically — `MUZIX_SYS_COPY`=1 is
user `SYS_FORK`, `MUZIX_SYS_YIELD`=10 is user `SYS_OPEN`, `MUZIX_SYS_EXIT`=11
is user `SYS_CLOSE`. They are disambiguated by the order
`muzix_syscall_to_server()` (`kernel/syscalls_ipc.c`) and the local dispatch
test them in — the internal numbers are matched before the server ranges — and
not by renumbering, so do not assume one number means the same call on both
paths.

### Process Model

- `muzix_proc_slot_t`: pid, parent_pid, flags, exit_status, pending_signals,
  stack_ptr, entry_point, uid/gid/euid/egid/umask, user/sys time, child time,
  ready-link, mproc, bank map.
- `muzix_kernel_proc_table_t`: fixed-size table (MUZIX_PROC_TABLE_MAX=8,
  `kernel/proc_table.h`), 3 priority queues (TASK_Q=0, SERVER_Q=1, USER_Q=2),
  kernel_map.
- Scheduler: round-robin within each priority queue; priority order TASK > SERVER > USER.
- Cooperative (non-preemptive): a process runs until it parks, blocks, or exits.
  There is a 120 Hz CTC tick interrupt and it **still does not switch processes
  — there is no preemption, and nothing about the interrupt path implies
  there will be.** The tick counts, it bounds every deadline, and it makes
  `UTIME`/`STIME` real; preempting from it is a separate change, and the
  argument in `platform/zeta-v2/tick.s` about which windows the handler may
  touch — its data cells only in window 0, its pushes on window 3 under the
  kernel map or window 1 on the interrupted process's own stack — is exactly
  what such a switch would invalidate.
- The tick is no longer the only interrupt. The same CTC runs a second,
  vectored one for the UART (`im 2`, vector 0x84), so a keystroke interrupts
  the machine instead of waiting out whatever deadline a reader is parked on.
  That is a wake, not a preemption: the handler sets a `_DATA` flag and the
  halt loop in `kernel_loop.c` turns it into a running process on its next
  pass, from ordinary kernel context with the map known.
- A blocked process **parks**. `muzix_proc_table_park()` (kernel/proc_table.c)
  sets `MUZIX_PROC_WAIT_TICK`, puts a 120 Hz deadline in `tick_stamp`, and
  **unlinks the slot from the ready queues** — so `pick_next()` needs no idea
  parking exists, and the one function that decides what runs stays the one
  that already did. The loop halts the processor when nothing is runnable;
  `muzix_proc_table_wake_expired()` requeues on the deadline and
  `muzix_proc_table_wake_input()` requeues on the UART flag.
- Because the C stack *is* the span above `_DATA`, a parked process has no
  stack to return to, so `park()` records a resume point — the
  `muzix_resume_pc` / `muzix_resume_sp` pair the syscall trap already captured,
  not a read back off the user's stack, which by then is the kernel's page
  0x21 and yields kernel code bytes. `tools/check_park_site.rb` fails the
  build if a `park()` call site attributes that capture to any slot other than
  `muzix_current_slot(ctx)`.

### Process Identity (one number, one owner)

**A process has ONE number, and it is the same integer the scheduler calls a
slot.** A user process and the IPC entry that describes it share it.

This was not always so, and the details matter because the failure mode is
invisible at every individual call site:

- `muzix_ipc_proc_t` lives in `procs[]`, and `procs[]` does **not** index from
  zero. The first `NR_TASKS` entries hold the negative system-task numbers
  (MINIX put them there), so a process number `nr` sits at index
  `NR_TASKS + nr`. **That `NR_TASKS +` is an array offset, not a property of
  the process.**
- Five sites added that offset to a number and then used the result as a
  process number. Each was plausible alone, each was wrong, and each was
  invisible until the machine ran:
  - `kernel/syscalls_ipc.c`: `ipc_slot = 10 + current_slot`, used as `caller`.
    `get_proc()` then computed `procs[10 + (10 + slot)]` — nobody registered
    there, so every send and receive was addressed to a dead entry.
  - `kernel/syscalls_ipc.c`: `muzix_ipc_server_slot()` returned `10 + slot`,
    fed straight in as a routed syscall's destination.
  - `kernel/syscalls_ipc.c`: `ipc_client = msg.m_source`, unconverted. The only
    readers (`SRV_COPY` handlers) pass it to `muzix_copy_*_proc()`, which
    rejects `target_slot >= MUZIX_PROC_TABLE_MAX`. So every server-to-client
    copy returned `EFAULT` — the same code a genuinely bad address returns.
  - `shell/shell.c`: `#define FS_SERVER_IPC_SLOT 11`, a hand-written guess at
    `10 + server_id`.
  - `lib/server.c`: `server->ipc_slot = 10 + id`. Dead code — nothing read it —
    which is precisely why nothing caught it.

**The rule, enforced by `tools/check_proc_identity.rb` (target
`test-proc-identity`, part of `make check`):**

1. The process number IS the kernel proc-table slot. Do not convert between
   them; there is nothing to convert.
2. `muzix_ipc_proc()` in `kernel/ipc.c` is the **only** function that indexes
   `procs[]` or performs the `NR_TASKS +` arithmetic. Its inverse,
   `muzix_ipc_proc_nr()`, reads a number stamped into the entry at init rather
   than deriving it — because `rp - procs` widened to `__divsint` once
   `p_msg` made `sizeof` a non-power-of-two, and `KERNEL_LINK_ORDER` links no
   `.lib`, so that is a link failure.
3. Server numbers are `MUZIX_PROC_FS` / `MUZIX_PROC_MM` / `MUZIX_PROC_PM` in
   `include/muzix_ipc.h`. Never compute one from an id or a slot.
4. `muzix_ipc_get_proc()` is deprecated and must not gain new callers.

The gate is deliberately narrow — `value * 10 + digit` in `libc` is decimal
arithmetic and is *not* an offence — because a gate that fires on unrelated
code gets switched off, and then it protects nothing. A regression is verified
by injecting `10 + current_slot` and confirming the gate fails.

### Deferred Copy-Out (resolved — delivered at resume)

Servers fork, park, are readied and answered, and the reply reaches the
caller's userspace buffer. The gap used to be the last step of a reply, and the
mechanism that closed it is in `kernel/syscalls_ipc.c`, in the syscall layer
that already holds `ctx`:

When a server's reply arrives, `mini_send()` delivers it into
`caller_ptr->p_msg` — the IPC entry's own buffer, which survives the switch
because it lives in the table and not on any stack. It used never to reach the
caller's userspace buffer, because the syscall handler frame that would do the
`copy_to_user()` is already gone: `muzix_kernel_switchout()` either publishes
another slot's resume point and returns, or calls
`muzix_zeta_enter_userspace()` and never returns at all. Doing the copy in
`kernel/ipc.c` was not the answer either — `tools/check_memory_manager.rb`
admits a bank-crossing caller only from a file that **defines** an
`muzix_mm_*` function, and `ipc.c` defines none.

The implemented fix copies at delivery time:

- `muzix_syscalls_note_recv_buffer()` stamps `p_user_addr` on the parked
  slot's IPC entry while the caller is still running — the only moment its own
  buffer address exists, because after the park the frame that held it is
  abandoned.
- `muzix_syscalls_deliver_parked()` runs in the delivering syscall, on a
  kernel stack that is still live, before the caller is readied: it copies
  `p_msg` into the recorded `p_user_addr` through `muzix_copy_to_proc()`
  (which forwards to `muzix_mm_copy_kernel_to_user()`, so the memory-manager
  gate is satisfied), stamps the slot's `resume_ret` with the reply's `m1_i1`,
  and clears `DELIVERED`. It is called from the send paths, immediately before
  `muzix_proc_table_ready()`, so the copy happens on the way into the resume
  rather than after it. A copy that is refused leaves `DELIVERED` set, and the
  caller's next receive still collects the message by the old path.

**For archaeology, the symptom while it was missing:** the caller parked,
resumed with `resume_ret == 0`, and its `write()` returned 0; the shell read
that as failure and exited — the trace went `SYS: num=0D` (write) then
`SYS: num=03` (exit), which looked like a filesystem fault and was not one.


---

## Build System

### Toolchain Requirements

- **SDCC** 4.5.0 (`-mz80` target), installed as system commands on `PATH`:
  `sdcc`, `sdasz80`, and `sdldz80`. On macOS, install with `brew install sdcc`;
  on other platforms install/build SDCC 4.5.0 for that OS. Other versions are
  not the project's validated toolchain.
- **sdasz80** (SDCC assembler, for `.s` files).
- **sdldz80** (SDCC linker).
- **Ruby** (for `ihx_to_bin.rb`, `make_minixfs.rb`, `add_exec_header.rb`, and
  every `tools/check_*.rb` gate; `make_zeta_rom.sh` and `make_una_image.sh`
  are `sh`).
- **GCC-compatible compiler** (for dependency scanning, the C emulator, and
  host-runnable `test_*.c`; macOS Command Line Tools provide clang through the
  `gcc` command).
- **GNU Make**, POSIX shell tools (`sed`, `grep`, `dd`, `wc`), and POSIX
  threads for the emulator.

### Build Commands

All builds run from `Muzix/`.

| Command                  | Description                              |
|--------------------------|------------------------------------------|
| `make help`              | Show all targets                         |
| `make rom`               | Everything: kernel link, userspace (libc, `apps/*.bin` for echo/cat/ls/date/top/uptime/loadavg/hwdiag, `shell/shell.bin`), minifs, 512 KB Zeta V2 ROM, validators — the default target |
| `make check`             | Every source-level gate; no image needed |
| `make test-kernel-layout` | `_CODE`/`_DATA` extents, headroom, `_HOME`, overlap |
| `make test-rom-image`    | ROM size, kernel entry, MINIX-v1 filesystem magic |
| `make test-rom-validator`| Full ROM layout validator                |
| `make test-host`         | Compile and run the host-runnable `test_*.c` |
| `make rebuild`           | Clean + `rom`                            |
| `make clean`             | Remove all generated artifacts           |

The intermediate targets all exist and are what `make rom` depends on:
`build-kernel` → `build/kernel.map`, `build-kernel-bin`,
`build-userspace-bin`, `build-minifs`, `build-rom-boot`, `build-rom-image`.

Targets not provided by the current Makefile include `make build`,
`make kernel`, `make kernel-bin`, `make userspace`, `make rom-image`,
`make romfs`, `make run-tests`, `make bootable-image`,
and `make test-bootable-image`. `make kernel` is especially misleading because
`kernel/` is a directory: make finds it, decides it is up to date, prints
"Nothing to be done for 'kernel'" and exits 0 — so it looks like a build that
did nothing rather than an error. Use the Makefile targets above. The UNA image
tooling (`tools/make_una_image.sh`, `tools/test_una_image.sh`) is likewise present but
not wired to any target, so the UNA layout below describes scripts you run by
hand, not a build product.

### Build Artifacts

- `.rel` — SDCC/SDAS relocatable object files (written beside the sources, at
  the root: `load.rel`, `tick.rel`, `apps/top.rel`, …)
- `.ihx` — Intel HEX executables (linked)
- `.lst` — Assembly listings
- `.map` — Linker maps
- `.sym` — Symbol tables
- `.noi` — NoICE debug files
- `build/` — Final binaries: `muzix-zeta-v2.rom`, `muzix-kernel.bin`,
  `muzix-minifs.bin`, `muzix-rom-boot.bin`, `kernel.ihx`, `kernel.map`,
  `kernel.noi`, one `<app>.bin` per userspace program plus `shell.bin`, and
  `hosttests/` + `tick_z80` for the host-runnable tests

There is no `.build/` temp directory; intermediates land next to their sources.

All generated files are excluded from VCS (`.gitignore`).

### ROM Image Layout (`make rom`)

```
Offset 0x00000:  Boot stub (rom_boot.s) — LDIR ROM pages 0-3 → RAM 32-35
Offset 0x00098:  Kernel image (build/muzix-kernel.bin, in the 128 KiB region)
Offset 0x20000:  MINIX-v1-format filesystem (build/muzix-minifs.bin, 384 KB)
Total:           524 288 bytes (512 KB)
```

### UNA BIOS Image Layout (scripts only, no make target)

`tools/make_una_image.sh` and `tools/test_una_image.sh` exist and no target
invokes either, so nothing below is produced by `make rom`.

```
Sector 0 (LBA 0):  UNA BIOS stage-1 (512 bytes, boot sector)
Sector 2 (LBA 2):  MUZ2 stage-2 (flat binary with 16-byte ABI header)
LBA 2048:          Optional filesystem
```

The MUZ2 header at LBA 2 (what `tools/make_una_image.sh` writes):
```
Offset 0:  Magic "MUZ2" (4 bytes)
Offset 4:  ABI version (1 byte)
Offset 5:  Header size (1 byte, 16)
Offset 6:  Payload length (2 bytes, LE)
Offset 8:  Total sectors (1 byte)
Offset 10: Entry offset (1 byte, 16: the payload begins this many bytes into
           the sector; bytes 9 and 11-15 are written zero, not reserved)
Offset 16: Payload (flat binary)
```

---

## Emulator Setup & Testing

The emulator lives in `emulator/` and is a z80pack-style Z80 emulator supporting
RomWBW, FUZIX, and Muzix.

### Building the Emulator

```bash
cd emulator
make              # Build z80pack-emu
make asan         # Build with AddressSanitizer for debugging
make clean        # Remove build artifacts
make rebuild      # Clean + rebuild
```

**Dependencies**: GCC, pthread. On macOS, uses `-D_OS_DARWIN` and `-lpthread`.

### Emulator CLI

```
usage: z80pack-emu [options] [rom_file]

  -s, --system SYS       romwbw | fuzix | muzix | cpmsim
  -d, --disk FILE        Disk image for FDC
  -r, --run              Run continuously (until HALT or Ctrl+C)
  -c, --cycles N         Max cycles (0 = unlimited)
  -i, --stdin            Enable stdin-to-UART forwarding
      --strict-zeta        Enforce strict ZETA SBC V2 behavior (default)
      --compat-hacks       Enable legacy compatibility shortcuts
  -v, --verbose          Verbose debug output
  -h, --help             Show help
  --version              Show version
```

### I/O Port Map (Emulator)

| Port(s)    | Function                        |
|------------|---------------------------------|
| $00–$01    | z80pack TTY1 (stat/data/ctl)    |
| $28–$2B    | z80pack TTY2/TTY3               |
| $32–$33    | z80pack TTY4                    |
| $0A–$11    | z80pack FDC (drive/track/sector/cmd/stat/dma) |
| $20–$23    | CTC channels 0–3                |
| $60–$63    | PPI (8255)                      |
| $68–$6D    | NS16550-compatible UART (Zeta V2) |
| $6E–$6F    | UART MSR/SPR                    |
| $70        | RomWBW UART / DS1302 RTC        |
| $78–$7B    | Bank select registers (Bank 0–3)|
| $7C        | MPGENA (paging enable)          |
| $B4–$B7    | SIO A/B (RomWBW)                |

### Running Muzix in the Emulator

```bash
# Build the ROM image first (from Muzix)
cd Muzix
make rom

# Run in emulator
cd emulator
./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -r

# Run with cycle limit and verbose
./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -c 50000000 -v

# Interactive mode (stdin forwarded to UART at $68).  Without -i the emulator
# prints "No demo input injected" and the machine just sits at the prompt.
./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -r -i
```

`build/muzix-una.img` is not produced by any target — see the UNA layout note
above; it only exists if you ran `tools/make_una_image.sh` by hand.

### Emulator Smoke Test

Some scripts under `emulator/` are historical helpers and may reference old
paths or command names. The following invocation was verified from this tree:

```sh
cd emulator
printf 'uptime\n' | ./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -i -c 100000000
```

Expect the userspace shell banner and an `uptime` result, with no `MNT FAIL`
marker. This exercises ROM-backed filesystem loading and one external
userspace command; it is not a hardware test.

### Emulator Memory Model

- **RAM**: 512 KB of banked RAM in the Zeta V2 emulator model
- **Bank size**: 16 KB
- **Physical page IDs**: `0x20`–`0x3F` are RAM pages; `0x40+` is unmapped/ROM
  in the platform model. Do not describe `0x40+` as extended RAM.
- **Kernel pages** (Zeta V2/FUZIX reference layout): 32, 33, 34, 35
- **Common memory**: $FC00–$FFFF (z80pack mode only; normally banked)
- **Emulated CPU clock**: 7.3728 MHz in this emulator profile. The physical
  board in use has a 4 MHz U17 oscillator; do not treat the emulator clock as a
  hardware measurement.
- **Muzix timer tick**: the CTC chain uses channel 0, clocked from
  `UART_CLK/2` (921.6 kHz), with time constant 256 to produce 3.6 kHz; channel
  1 counts those pulses with time constant 30 for 120 Hz. The emulator's DS1302
  model advances against emulated CPU cycles in `z80pack.c`; this is separate
  from the board's RTC crystal.

### Debugging Tips

- Use `-v` for verbose debug output (shows bank mappings, port I/O).
- Use `--strict-zeta` (default) to enforce real Zeta V2 behavior. Bank selectors
  are **write-only** — code must not read them back.
- The boot timeout (`ZETA_BOOT_TIMEOUT_DEFAULT` = 50M cycles) allows hardware-
  dependent port reads to return timeout values, skipping hardware probing.
- Breakpoints: use `zeta_set_breakpoint()` API (address + one-shot support).
- A `HALT` at the kernel-entry `_halt` label indicates `_kernel_main` returned.
  The address is build-dependent; inspect the current listing/map rather than
  relying on a recorded address. Check the actual return path and bank-map
  restoration before attributing the halt to ROM reads.

---

## Build Verification

### Standard Verification Flow

```bash
# 1. Build everything.  `make rom` is the whole build.
cd Muzix
make clean
make rom           # kernel link, userspace, minifs, ROM, and the validators

# 2. Source-level gates on their own (no image required)
make check
# Also available individually: test-kernel-layout, test-window0,
# test-syscall-context, test-user-pointers, test-link-scripts,
# test-emulator-sources, test-userspace-layout, test-userspace-runtime,
# test-memory-manager, test-park-site, test-proc-identity, test-host
# test-z80-call-convention is run by test-rom-image, not by make check.

# 3. Verify the ROM image (make rom already ran these)
make test-rom-image
# Checks: ROM size == 512*1024, kernel entry at 0x98 is DI+LD SP,
#         MINIX-v1 filesystem magic 0x137f at offset 128KB+512+13
make test-rom-validator

# 4. Run in emulator
cd emulator
make
printf 'uptime\n' | ./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -i -c 100000000
# Expect the shell banner and an uptime result; reject MNT FAIL.
```

### Test Suite

```bash
cd Muzix

# The host-runnable tests, which are the ones that execute
make test-host
# The runner prints current counts and a reason for tests it cannot build/run;
# these totals change as tests are added or become portable.

# An emulator run, with loadavg measured at known elapsed times
cd emulator && make
( (sleep 5; printf 'uptime\n'; sleep 1; printf 'loadavg\n') | \
  ./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -i )
```

The most recent recorded run executed these eight tests:

```
fs/           test_allocator, test_directory, test_path, test_tty_device
kernel/       test_load
platform/     test_rtc_ds1302, test_syscall_vector, test_tick
```

`tools/host_tests.rb` discovers tests, builds the host-portable subset, and
prints current counts plus reasons for tests it cannot build or run. Do not use
old test inventories or warning line numbers as current build results.

### Known Limitations

- Multitasking is **cooperative**, not preemptive. The 120 Hz tick
  (`platform/zeta-v2/tick.s`) counts and bounds waits; it does not switch
  processes, so a process runs until it parks or exits. Preempting from it is
  not wired and is not implied by its existing.
- **Rescheduling at syscall boundaries could not work here before parking
  existed. The measurements are history; the design that replaced them is
  current.** For the record, because the reasoning is worth having: the flag
  and the safe point both fired — 465 handler entries and 129 yield attempts
  over 30M cycles, with a store to a `_DATA` byte and a test of that byte at
  the top of `muzix_handle_userspace_syscall()` — and only **one** of those 129
  attempts actually switched, because `muzix_proc_table_yield()` handed back the
  running slot every other time: there was never a second candidate. The cause
  was that a process waiting for I/O was **not in a ready queue**. It was
  inside a system call, and `muzix_tty_wait_for_input()` held the CPU there for
  up to `MUZIX_TTY_READ_SECONDS` while it polled. Measured while `top` ran, the
  shell's UTIME/STIME were frozen at 214/86 across every frame, so it was never
  dispatched once, and the load figure sat at 0.98 / 1.00 for the whole session.

  None of that describes the machine now, and the prediction this file used to
  make — that blocking has to park, "a change to the blocking path in
  `fs/tty_device.c` plus a wake-on-completion in `kernel/proc_table.c`, which is
  a redesign rather than a patch" — is what got built. All four pieces:

  - **`park()`**, `kernel/proc_table.c`: sets `MUZIX_PROC_WAIT_TICK`, stores a
    120 Hz deadline in `tick_stamp`, **unlinks the slot from the ready queues**,
    and records the resume point from the pair the syscall trap published. It
    does *not* leave the slot queued for `pick_next()` to skip: unlinking is
    what keeps the runnable decision in one function, and it is why
    `muzix_proc_table_ready()` can be reused verbatim by both wakes.
  - **`sys_park()`** (syscall 49, `lib/syscall.h`), the userspace half. It has
    to be called from the process and not from inside a kernel read, because
    the instruction after it is the process's own — a park taken inside, say, a
    read would resume inside the kernel with the process's map installed under
    it.
  - **`MUZIX_READ_AGAIN` = -11** (`lib/tty_ioctl.h`), what a character device
    returns when its single port read finds no byte. It was `-1`, and `-1` is
    what a bad descriptor is, so `lib/libc.c`'s `read()` could not tell "wait
    for me" from "never going to happen" and returned it straight to the caller
    instead of parking on it. Every console reader then became a
    `while (result < 0) retry` loop around a read that would never wait — a
    spin with the processor held for all of it, and *that* loop, not the
    kernel, was the saturation. `read()` now parks for 240 ticks (two seconds)
    on exactly this value and returns every other answer, errors included;
    `getchar()` and `shell/shell.c`'s `readline()` go through `read()` for
    that reason rather than calling `sys_read()`.
  - **`muzix_proc_table_wake_input()`**, which turns the UART flag into a
    running process. Without it the two-second park deadline is the only thing
    that ends a wait, so a keystroke arriving half a second into a park waited
    out the remaining one and a half. It wakes *every* parked slot, not just
    the console's: there is no "parked on the tty" flag in the slot to select
    with, so a process parked on a pipe wakes, finds nothing and parks again —
    a spurious wakeup, correct, and bounded by how often input arrives.
    `muzix_proc_table_any_parked()` covers the other idle state, last process
    exited, where nothing is runnable *and* nothing is parked and
    `should_idle()` answers "there is work".

  Measured on the emulator at an idle prompt, `build/muzix-zeta-v2.rom` run as
  `z80pack-emu -s muzix …rom -i` with `uptime` and `loadavg` fed on stdin so
  each reading has a known elapsed time:

  | `uptime` | `loadavg` (1/5/15 min) | `since boot` |
  |---|---|---|
  | first read after boot | 0.98 0.98 0.98 | 1.00 |
  | 6 s  | 0.51 0.51 0.51 | 0.59 |
  | 23 s | 0.39 0.48 0.50 | 0.19 |
  | 52 s | 0.25 0.44 0.49 | 0.09 |
  | 80 s | 0.15 0.40 0.47 | 0.06 |

  A second run of the same shape read 0.30 0.46 0.49 / 0.11 at 39 s,
  0.18 0.41 0.48 / 0.06 at 71 s and 0.10 0.37 0.46 / 0.04 at 103 s. The
  reading taken immediately after boot is still 0.98 / 1.00 and always will
  be — boot genuinely is 100% busy, and `muzix_load_sample()` seeds its first
  reading from since-boot rather than from zero. **The difference is what
  happens next**: the 1-minute window falls to 0.15 by 80 s and 0.10 by 103 s
  and `since boot` to 0.04, where before it never moved at all. The three
  windows also separate in the right order and by the right amount for a machine
  that is idle — 0.10 / 0.37 / 0.46 at 103 s, short window lowest — which is
  what the 1, 5 and 15 minute constants are for.

  CTC control-word bits are D6 = counter/timer selection (1 = counter) and
  D5 = timer prescaler (/16 or /256). A mistaken change from channel 3's
  `0x27` to `0x67` set counter mode on the PPI-wired channel, stopping the
  free-running cycle count on real hardware while an earlier emulator model
  misread those bits and continued to count. The counter must be configured as
  `0x27` (timer, /256); the emulator now uses the hardware bit definitions.
  The physical board in use has a 4 MHz U17 oscillator. CTC3 therefore runs at
  4 MHz / 256 = 15.625 kHz, or about 130.2 counts per 120 Hz tick. Startup
  calibrates this ratio over 32 ticks, explicitly counting CTC3 wraps. The boot
  diagnostic sums the modulo-256 CTC3 down-counter
  deltas across eight tick-driven HALTs. The total should be close to 8 * 130
  counts, minus the awake work between interrupts; a single
  HALT can land just before a tick and produce a misleadingly small delta. A
  short busy-loop probe is not a valid test for this counter because it may run
  for fewer than 256 CPU cycles.
  `MUZIX_HALT_PROBE` is off by default to avoid adding its multi-second hardware
  diagnostics to every boot; define it as 1 when explicitly investigating IRQ
  or HALT behavior.

  `apps/top.c` parks in 30-tick slices while checking the RTC. It checks for
  keyboard input at the start of each refresh; the sliced RTC wait does not
  cancel early on input, so the quit delay can approach the two-second refresh
  interval.

  `SYS_WAIT` is a one-shot reap: it returns the pid of an exited child or `-1`
  if none is available; it does not park or wait on a child-exit queue.
  `shell/shell.c` calls it once and ignores the result, so callers must not
  assume it blocks until the child exits.
- `time()` converts DS1302 calendar fields to seconds since 2000-01-01.
  `uptime()` subtracts the RTC reading saved at boot. `UTIME` and `STIME` in
  `struct proc_info` are scheduler accounting counters in 120 Hz ticks, not
  seconds; one second corresponds to 120 ticks.
- Pipes are bounded and non-blocking; Minix suspend/revive queues not implemented.
- `mknod` accepts only TTY character devices.
- Minix syscall ABI incompatibility: Muzix uses custom numbering (48 numbers:
  42 non-IPC + 6 IPC) vs Minix's 69 slots. See `docs/minix-muzix-audit.md` at the
  repository root.
- Disk drivers (wini, floppy, printer) and 50+ MINIX user commands are not
  implemented.
- Block size difference: Minix uses 1024-byte blocks; Muzix uses 512-byte.

### Known gaps after the 2026-10-14 audit round

- **Undocumented Z80 instructions — surveyed; the two wins from it applied
  2026-10-15, the undocumented-specific verdicts unchanged.** The survey (IXH/IXL,
  SLL, the DD CB group, the MEMPTR flag trap, Z180 portability, hardware tests
  T1-T7) was in `docs/z80-undocumented-instructions.md`, now deleted. What landed:
  (1) `EXX` + `EX AF,AF'` instead of pushing AF/BC/DE/HL in both interrupt
  handlers — `platform/zeta-v2/tick.s`, measured on 2026-10-15 with
  deterministic idle runs: the idle halt fraction went 64.25% → 65.01%, the
  highest recorded, and the per-tick stack traffic 132T → 16T. **This makes
  the alternate register set an ABI resource: EXX/EX AF,AF' belong to the
  interrupt handlers and to nothing else** — the tree uses them nowhere else
  today (verified by grep), and any future user outside an interrupt handler
  silently corrupts one. (2) The page allocator's bitmap walk in
  `platform/zeta-v2/bitmap_ops.s`: `zeta_memory_alloc_page()` scanned a
  uint32_t mask with `(mask & (1u << index))` per step, which SDCC expands
  into a fresh 32-bit variable shift every iteration — ~3.5k T-states over 32
  bits against ~470T for the assembly byte-then-RRL walk, called through
   `muzix_bitmap_alloc()`/`muzix_bitmap_release()`, with C reference
   implementations in `test_host/bitmap_host.c` for the host gate. What did
   NOT land and why: SLL has no `(x<<1)|1` pattern in the tree; the ED-prefix
   SLL (ED 90-97) in the emulator had a carry-flag bug (`z->cf = val & 1`
   instead of `z->cf = val >> 7`) that was caught and fixed by the new GROUP
   19 in `z80test.c`; IXH/IXL cost +4T and +1 byte against H/L and win only
   when registers are exhausted; the DD CB indexed forms (`BIT/RES/SET
   b,(IX+d)`) win when the base is already in IX and the bit number is a
   compile-time constant — in the bitmap walk the bit index is a run-time
   value, so an indexed form costs an 8-way dispatch that measures worse than
   the walk that is there; and the undocumented `DD CB rot→reg` forms and
   `rtc_ds1302.c`'s `(src[i] >> j)` variable shift are a real but low-frequency
   win (the RTC is read once per 30720 ticks) against a bit-exact protocol
   with three documented traps, so it was left for a change with hardware in
   hand. `z80test.c` now has test groups covering these: GROUP 19 (SLL
   ED 90-97, 384 tests), GROUP 20 (DDCB/FDCB rotate+load, 17664 tests), GROUP
   21 (DDCB/FDCB RES/SET+load, 17664 tests), GROUP 22 (IXH/IXL/IYH/IYL
   operands, 1608 tests — LD nn, INC/DEC, LD r, ALU) — 37020 new tests, all
   passing. sdasz80 accepts no DD/FD mnemonic at all — any future use is `.db`
  with a decoding comment, and the hardware tests T1-T7 in the survey are the
  prerequisite.

- **FIXED 2026-10-14 (late) — `ls` printed nothing, and the cause was two
  microkernel-transition defects plus a namespace collision.** (1) Children of
  SYS_FORK were never registered in the IPC table: `muzix_ipc_register_server()`
  had callers only for the four slots kernel_main() starts itself, so a forked
  slot kept `P_SLOT_FREE` and every reply the FS server sent to it was refused
  with E_BAD_DEST — measured: `MSEND bad: caller=12 dest=15 dflags=05`, 53
  times in one run, the child parking and returning the park placeholder 0.
  `muzix_handle_fork()` now registers the child (unregister+register, so slot
  reuse is idempotent) and `muzix_handle_exit()` unregisters it. (2) The FS
  server's file handlers are stubs — `servers/fs/fs_server.lk` links
  fs_server + fs_clock + tty_device + the bank primitives and nothing else, so
  handle_open/handle_readdir/handle_read/handle_stat answer -1. The kernel now
  serves open/readdir/close for the ROM filesystem itself, from
  `kernel/rom_read.c`'s `muzix_rom_open_path()` / `muzix_rom_readdir()` (the
  same block device, the same MINIX-v1 walk the exec ROM path uses), intercepted
  ahead of the routing table like TIMES/PROCTAB/RTC. A synthetic descriptor is
  inode + 4, a range disjoint from the console's 0/1/2. (3) The userspace trap
  path also answered `MUZIX_SYS_YIELD` — which is 10, which is also user
  SYS_OPEN — before anything else, so every open() returned 0, a descriptor
  that looks valid; that early intercept is deleted from the userspace path
  (the internal numbers belong to the record-driven dispatcher), and the
  kernel-side handlers take `(uint16_t)arg` because the trap sign-extends the
  argument word (0x888A arrives as int32 0xFFFF888A). `ls` lists all 17
  entries; the remaining FS stubs (`read`, `stat`, `seek`) mean `cat file` is
  still not served, and the shell's one-shot `SYS_WAIT` still prints a prompt
  mid-command-output — both pre-existing, neither part of that fix.


Status section, not a wish list: each item below was found by the audit, was
deliberately closed or deliberately accepted, and is recorded here so the next
reader does not rediscover it as a bug. `AUDIT_FIX_PLAN.md` is the tracking
document for the same round. Everything in this section is the state of a
build that passes `make check`, `make rom` and the emulator smoke run.

- **Boot-time performance — FIXED on 2026-10-14 (was the accepted
  ~6 s against ~4 s deviation).** The exec path copies the program image
  through the memory-manager primitives (`muzix_mm_copy_kernel_to_user`,
  i.e. `zeta_copy_kernel_to_user`), and the cost was inside the primitive
  itself: every 256-byte exec chunk was staged byte-by-byte through
  `g_copy_staging` in C before the LDIR — about thirty instructions per byte
  for a copy whose ldir is a twentieth of that. The temp map replaces exactly
  one window and that window is always window 1 (a region-2 destination is
  remapped into window 1, a region-1 destination is window 1, regions 0 and 3
  are refused), so a source that lies entirely outside window 1 reads the
  same bytes either side of the switch and needs no staging at all:
  `zeta_copy_kernel_to_user` now ldirs straight out of the caller's buffer
  when the source range does not intersect window 1, and stages only when it
  does. Measured on 2026-10-14, same harness on 25bd81c, on the fixes before
  this one, and on this change: `uptime` reports `up 0 days, 0:00:06`,
  `0:00:06`, `0:00:04`; the shell banner arrives at 3.41M, 3.41M and 2.42M
  instructions; and the idle halt fraction (no input, 120M cycles) is 54.29%,
  54.13% and 64.31% — the last one is back to the pre-audit ~63% the audit
  had cost. The copies still go through the one mechanism allowed to program
  the banks; the gate is untouched, and the staging fallback is unchanged for
  the window-1 source case that needs it.
- **HIGH-15 — the kernel does not link the message-routed MM dispatcher.**
  Three files are named `mm_service.c`. vpath order decides which one the
  kernel gets (`kernel/` ahead of `mm/` and `servers/mm/`), so the kernel
  image contains `kernel/mm_service.c`, whose page allocator and three copy
  entry points are real and whose message-routed entry points (`newmap`,
  `copy`, `load_map`, `load_kernel_map`, `cross_bank_call`,
  `copy_page_to_kernel`, `refresh_visible`) are deliberate refusals. The real
  dispatcher `mm/mm_service.c` — every method a
  `muzix_system_task_handle_message()` call — is linked only into the
  userspace MM server, through its own copy under `servers/mm/`. The
  consequence the audit flagged: the kernel's own `MUZIX_SVC_*` handlers in
  `kernel/system_service.c` have **no caller in the image** — the only callers
  of `muzix_system_task_handle_message()` are the two dispatchers the kernel
  does not link — so they are dead code there, kept because the host tests
  drive them directly and because deleting the dispatcher would delete the
  shape the servers are modelled on. Switching the kernel link was considered
  and rejected: it would put an unexercised dispatcher between `exec_loader`,
  `rom_read` and every syscall-layer copy on one side and the platform
  primitives on the other, on a kernel stack with about 1.2 KiB of headroom,
  on the path that boots the machine. If a kernel caller ever needs one of
  the refused methods, implement it in `kernel/mm_service.c` as a direct call
  into `platform/zeta-v2/context_switch.c`.
- **P-2 — the tick handler is reachable through two vector paths, and only one
  of them is exercised.** `platform/zeta-v2/tick.s` plants a three-byte `JP`
  to `_muzix_tick_isr` at `0x0038` (IM1) *and* an IM2 table at `0x0080` whose
  `0x82` entry is the tick and whose `0x84` entry is the UART's receive
  interrupt; the machine runs IM2, so the IM1 vector is a floor under the mode
  the ROM boot stub leaves behind — a tick that arrives before `im 2` is armed
  lands on the same handler rather than on whatever byte happens to sit at
  `0x38`. Its safety argument (window-0 code and cells, no tail call, no bank
  write, pushes only on the kernel stack or on the interrupted process's own)
  has to hold for both paths, and the emulator's CTC model exercises only the
  IM2 one: nothing in the test harness fires a tick in IM1 mode. The table
  itself is pure data at `0x0080–0x008F`, clear of `_CODE` (which starts at
  `0x0098`) by argument, not by a gate.
- **`syscall_vector.c` is dead in the image.** The module is linked
  (`KERNEL_C_MODULES`) and its host test (`test_syscall_vector.c`) passes, but
  `muzix_z80_install_syscall_vector()` has no production caller:
  `platform/zeta-v2/kernel_entry.s` plants the `JP` at `0x0030` itself, in
  three stores, and that is the installed path. Kept as the documented API and
  for the test; nothing in the ROM calls it.
- **The UNA dispatch path is unverified — there is no UNA hardware.**
  `platform/zeta-v2/una_boot.s` / `una_stage2.s` behind
  `tools/make_una_image.sh` / `tools/test_una_image.sh` are hand-run scripts
  with no make target. Stage-2 is a deliberately small freestanding payload
  that proves the reset-to-stage-2 handoff (it prints a banner and stops)
  while the C kernel is not freestanding for UNA: no UNA block device, no
  filesystem through it. The scripts' output has never been checked against a
  board.
- **The FS server's UART transmit bound is self-referential — a design
  limitation.** In the FS server's link, `_muzix_zeta_uart_send()`'s four-tick
  wait for a free transmitter is measured on `muzix_tick_count`, which
  resolves to `servers/fs/fs_clock.c`'s counter — advanced once per dispatched
  request at the top of `fs_server_handle()`. A transmit wait taken *inside* a
  handler therefore cannot expire on its own: the counter can only move on the
  next request, which cannot arrive while the server is blocked in the send.
  The real transmitter drains by itself, so in practice the bound holds; on a
  line that never reports THR-empty the FS server would deadlock inside its
  own send, where the kernel's identical wait is bounded by the real 120 Hz
  tick. Accepted, per `fs_clock.h`: that counter measures request progress,
  not wall time, and the waits that must be bounded in real time are bounded
  by libc's `read()` parking on the kernel's deadline.

### Boot Failure Investigation Notes

A known issue involved `rom_read_block` reading port `0x79` (write-only bank
selector) and restoring `0xFF`, corrupting the kernel bank. **Resolution**:
`rom_read_block` no longer touches a bank register at all. It receives a
`muzix_mm_service_t` via `g_block_dev.context` and calls
`muzix_mm_copy_rom_page_to_kernel()`, which delegates to
`zeta_copy_rom_page_to_kernel()` in `platform/zeta-v2/context_switch.c`. That is
the platform layer's temp-map machinery: `copy_temp_map()` copies all four of
`state->bank_reg[]` and swaps exactly one window, and `map_call_impl()` puts the
caller's complete map back afterwards. Strict emulator validation now reaches
the Muzix shell with banks `20 21 22 23`; repeated `kernel_main` re-entry is
gone. `tools/check_memory_manager.rb` makes a second caller of the platform
copy primitives a build failure.

**`zeta_save_kernel_map()` / `zeta_restore_map()` are not what does this, and an
earlier version of this file said they were.** Those two names exist only under
`platform/zeta-v2/test_kernel/` — `bank_model.c`, `bank_model.h` and
`context_switch.c` — which is the standalone model the kernel host tests and the
emulator's own test binaries link. Neither is in `KERNEL_LINK_ORDER`, so they
are in no production image; `grep -rn zeta_save_kernel_map` finds nothing
outside that directory. If something cites them as the production mechanism, it
is citing the test double.

---

## Key File References

### Kernel Core
- `kernel/kernel_main.c` — Entry point: init UART, init IPC, init MM service, init services, exec servers + shell
- `kernel/kernel_loop.c` / `kernel_loop.h` — Scheduler dispatch loop
- `kernel/proc_table.c` / `proc_table.h` — Process table (max 8 procs), the
  ready queues and the round-robin pick, park and the two wakes
- `kernel/load.c` / `load.h` — the three Q14 busy-fraction windows behind
  `SYS_LOAD`
- `kernel/syscalls.c` / `syscalls.h` — Syscall dispatch (kernel + user)
- `kernel/syscalls_ipc.c` — IPC-based syscall dispatch (FS/MM/PM → userspace servers)
- `kernel/ipc.c` (header `include/muzix_ipc.h` — there is no `kernel/ipc.h`) — Minix-style IPC core (send/receive/both, priority queues)
- `kernel/exec_loader.c` / `exec_loader.h` — Muzix executable-header validation, linker-derived entry handling, and flat-image loading (dual path: direct ROM reads before FS server, IPC after)
- `kernel/bootstrap.c` / `startup.c` — Boot initialization
- `kernel/shell.c` — In-kernel shell (TTY-based, fallback if exec fails)
- `kernel/rom_read.c` — ROM block device read (write-only bank-safe)
- `kernel/libc_str.c` — Kernel string utilities
- `kernel/msg_pool.c` / `kernel/proc_server.c` — source-only orphans: neither
  is in `KERNEL_C_MODULES`, so nothing links them. They call
  `muzix_proc_table_park()` on arbitrary slots, which `tools/check_park_site.rb`
  exists to refuse (`muzix_current_slot(ctx)` is the only sanctioned capture),
  so do not mistake them for kernel code

### Memory Manager (Userspace Server at slot 3)
- `servers/mm/mm_server.c` — Userspace MM server main loop and IPC handlers
- `kernel/mm_service.c` — the **kernel-linked** MM service module. Three files
  in the tree share the stem `mm_service`; the Makefile's `KERNEL_C_MODULES`
  lists the bare stem and vpath puts `kernel/` ahead of `mm/` and `servers/mm/`,
  so this file is the kernel's implementation. It implements the page
  allocator and the three copy entry points (`muzix_mm_copy_rom_page_to_kernel`
  for `rom_read_block`, `muzix_mm_copy_kernel_to_user` /
  `muzix_mm_copy_user_to_kernel` for `muzix_copy_*_current` and the syscall
  layer's copies) as direct calls into the platform's `zeta_copy_*` primitives,
  and refuses the message-routed entry points (`newmap`, `copy`, `load_map`,
  `load_kernel_map`, `cross_bank_call`, `copy_page_to_kernel`,
  `refresh_visible`) that no kernel caller needs. Its header records why the
  link is not switched to the dispatcher
- `mm/mm_service.c` / `mm_service.h` — the message-routed dispatcher (every
  method builds a `muzix_system_message_t` and calls
  `muzix_system_task_handle_message()`). **Not in the kernel image** — the
  userspace MM server links its own copy under `servers/mm/` per
  `MM_SERVER_C_MODULES`. Editing it does not change the kernel
- `mm/mm_copy.c` / `mm_copy.h` — Copy service: map-to-map transfer, the kernel→user/user→kernel entry points
- `mm/mproc_model.c` / `mproc_model.h` — Process memory model with `umap()`

### Filesystem (Userspace Server at slot 2)
- `servers/fs/fs_server.c` — Userspace FS server main loop and IPC handlers
- `fs/fs_service.c` — **Userspace** FS service logic (open/read/write/seek/close, etc.)
- `fs/volume.c` / `volume.h` — Volume mount/format/flush lifecycle
- `fs/block_device.c` / `block_device.h` — Block device abstraction
- `fs/block_cache.c` / `block_cache.h` — LRU block cache
- `fs/inode_table.c` / `inode_store.c` — Inode management + persistence
- `fs/directory.c` / `directory_store.c` — Directory entries + persistence
- `fs/allocator.c` / `allocator_store.c` — Zone allocation
- `fs/file_io.c` — File I/O operations
- `fs/path.c` — Path resolution
- `fs/superblock.c` — Superblock load/validate
- `fs/tty_device.c` — TTY abstraction (canonical mode, echo, XON/XOFF)
- `servers/fs/fs_clock.c` / `fs_clock.h` — the clock the FS server's TTY console
  uses for its progress counter (in `FS_SERVER_MODULES`; there is no
  `fs/rom_block_device.c` — ROM block reads are `kernel/rom_read.c`)

### Process Manager (Userspace Server at slot 4)
- `servers/pm/pm_server.c` — Userspace PM server main loop and IPC handlers
- `pm/pm_service.c` — **Userspace** PM service logic (fork/exec/exit/wait, signals, RTC, uid/gid)
- `kernel/proc_table.c` — **Shared** process table (max 8 procs), ready queues, round-robin pick, park and wakes

### Platform (Zeta V2)
- `platform/zeta-v2/bank_io.h` / `bank_io.s` — Bank switching I/O (ports $78–$7C)
- `platform/zeta-v2/kernel_bank_model.c` — Kernel bank mapping model
- `platform/zeta-v2/kernel_entry.s` — Kernel entry at 0x0098; `_halt` is reached
  only if `_kernel_main` returns. `_muzix_zeta_enter_userspace` refuses a map
  whose `pages[3]` is not the kernel's `0x23` before it writes a single bank
  register (the same window-3 rule `context_switch.c` enforces on the copy
  paths), and `_muzix_zeta_resume_from` hands a resumed process its pending
  syscall's return value in DE, exactly as the trap's ordinary exit does
- `platform/zeta-v2/rom_boot.s` — ROM boot stub (LDIR + JP 0x0098)
- `platform/zeta-v2/syscall_entry.c` / `syscall_entry.s` — Userspace syscall trap
- `platform/zeta-v2/syscall_entry_stub.c` — Syscall stub
- `platform/zeta-v2/syscall_runtime.c` / `syscall_runtime.h` — Syscall runtime binding
- `platform/zeta-v2/syscall_vector.c` / `syscall_vector.h` — Syscall vector table
- `platform/zeta-v2/uart_io.s` / `uart_io.h` — NS16550 UART driver ($68–$6D)
- `platform/zeta-v2/z80_io.h` / `z80_io.s` — Z80 I/O primitives (inb/outb)
- `platform/zeta-v2/context_switch.c` / `context_switch.h` — map install and
  restore, `write_all_banks()`, and the four `zeta_copy_*` primitives. **This
  is the module the memory-access rules above are about**: it is the only
  production code that programs the bank registers, it refuses a map whose
  window 3 is not the kernel's, and `zeta_copy_rom_page_to_kernel()` is what
  `rom_read_block` actually goes through
- `platform/zeta-v2/rtc_ds1302.c` / `rtc_ds1302.h` — the DS1302 on port `$70`,
  which is where `time()`, `uptime` and the load windows' sense of wall time
  come from
- `platform/zeta-v2/trace.c` / `trace.h` — the ring buffer the fallback
  shell's `trace` command dumps
- `platform/zeta-v2/sdz80_runtime.s` — `___sdcc_call_hl`, `___sdcc_call_iy` and
  (the `--stack-auto` frame entry) `___sdcc_enter_ix`, and no multiply helper,
  which is why `__mullong` does not resolve from it
- `platform/zeta-v2/una_boot.s`, `una_stage2.s` — the UNA BIOS stage-1/stage-2
  sources behind `tools/make_una_image.sh`; no make target builds them
- `platform/zeta-v2/tick.s` / `tick.h` — the 120 Hz CTC tick **and the UART's
  interrupt, both under IM2**: `xor a / ld i,a / im 2`, a vector table at
  0x0080, the counter, the deadline test and the process accounting. CTC
  channel 0 carries the vector base (`0x80`), channel 1 is the tick (arrives as
  vector `0x82`), and channel 2 is the UART's interrupt controller in counter
  mode with time constant 1, so one received byte is one edge and one vector
  (`0x84`). The board wires the CTC as a vectored interrupt controller, which
  is why IM2 and not IM1, and FUZIX's port for this board's family does the
  same. An IM1 `JP` to the tick handler is **still planted at 0x0038**, three
  bytes written once: IM1 is the mode the ROM boot stub leaves behind, so if
  anything re-arms interrupts before `muzix_tick_init` has run `im 2`, a tick
  lands on the tick handler rather than on whatever byte happens to be at
  0x38. It is a floor under the old mode, not a second code path. The table at
  0x0080 is safe under every map because **window 0 is always kernel** —
  `mm/mproc_model.c` builds a process map as `{0x20, stack, text, 0x23}` and
  only windows 1 and 2 are ever replaced — which is the same fact
  `tools/check_window0.rb` enforces and the reason the vector table and the
  handler itself have always been safe (the handler is `_muzix_tick_isr`,
  0x01C2 as of this writing — read `build/kernel.map`, not this file, for the
  current address). The divisor arithmetic, the "why is this handler safe under
  any map" argument and the "why does the UART handler set a flag instead of
  waking anybody" argument are all in the source, and they are the reason the
  next person should not make it bigger.
- `platform/zeta-v2/test_kernel/` — Test kernel: bank_model, context_switch, minix_adapter

### Library
- `lib/libc.c` / `libc.h` — C runtime (strings, memory, conversions) and the
  user-facing `read`, which parks on `MUZIX_READ_AGAIN`
- `lib/tty_ioctl.h` — the TTY line-discipline flags, `MUZIX_TTY_IOCTL_PENDING`,
  and `MUZIX_READ_AGAIN` (-11). The kernel, libc and every program include it,
  so it is the one place the read interface's three answers are distinguished
- `lib/proc_info.h` — the `struct proc_info` layout `top` reads out of
  `SYS_PROCTAB`, and the `MUZIX_PROC_TABLE_MAX` static assertion
- `lib/syscall.c` / `syscall.h` — Syscall wrappers (48 user syscall numbers,
  42 non-IPC + the 6 IPC calls, over 1–60 with gaps)
- `lib/user_syscall.s` — Assembly syscall entry (CALL 0x0030)
- `lib/crt0.s` — startup; loads `argc`/`argv` into HL/DE before `call _main`
- `lib/math.s`, `lib/sdcc_runtime.s` — userspace-side helpers, linked per app
- `lib/romfs_data.c` — ROMFS data blob; legacy and not linked into anything
  (no link line references it, and the tools exclude it as generated data) —
  kept for reference, not a build component

### Apps & Shell
- `shell/shell.c` — Userspace shell, and PID 1: the kernel execs it into slot 1
- `apps/echo.c`, `apps/cat.c`, `apps/ls.c`, `apps/date.c`, `apps/top.c`,
  `apps/uptime.c`, `apps/loadavg.c`, `apps/hwdiag.c` — User commands. `loadavg` is the one-shot
  measurement of the kernel's three busy-fraction windows and prints `since
  boot` beside them; `top` is the display and parks in slices between redraws
- `oldOrphanedCodeBackup/init.c` — a legacy `/init` patterned after MINIX that
  nothing runs; the
  shell is PID 1. Moved out of the build, not deleted — see the README there

### Tools
- `tools/ihx_to_bin.rb` — Intel HEX → flat binary (handles SDCC `00:` prefix)
- `tools/add_exec_header.rb` — Prepends entry point to flat binary
- `oldOrphanedCodeBackup/make_romfs.rb` — wrote MZRFS1. Moved out of `tools/`;
  nothing in the build references it and the image uses the MINIX-v1 filesystem
  from `make_minixfs.rb` instead
- `tools/make_minixfs.rb` — Generates MINIX v1-compatible filesystem image
- `tools/make_zeta_rom.sh` — Assembles 512 KB Zeta V2 Flash ROM
- `tools/make_una_image.sh` — Creates UNA BIOS disk image with MUZ2 ABI
- `kernel/kernel.lk` — **dead.**  Nothing invokes it; the kernel link is the
  `build/kernel.map` rule in the `Makefile`, which derives its `-b _CODE` and
  `-b _DATA` flags and takes its module list from `KERNEL_LINK_ORDER`.  The file
  still sits in the tree and still reads as though it were the linker script for
  this kernel, which is how a change gets made to the wrong file.  It also lists
  `bank_call.s`, which no longer exists, and pins `-b _DATA = 0xDC00`, a third
  wrong number in a file nothing reads.
- `apps/*.lk`, `shell/shell.lk` — app and shell linker scripts (`_CODE` =
  0x8000, `_DATA` left to the linker default — **not** 0x9000, see the userspace
  layout section above), and unlike `kernel/kernel.lk` these **are** live: the
  Makefile links each one with `sdldz80 -f <name>.lk`

### Build Gates

These are the scripts behind `make check`. Each fails the build rather than
printing a note, and several derive what they check from the sources so a new
caller cannot slip past review.

| Gate | What it refuses |
|---|---|
| `tools/check_kernel_layout.rb` | `_CODE`/`_DATA` overlap, `_DATA` below the end of `_CODE`, headroom under 1024 bytes, a populated `_HOME`, a `.lib` in `KERNEL_LINK_ORDER` |
| `tools/kernel_data_base.rb` | not a gate — prints the `-b _DATA` the link must use |
| `tools/check_window0.rb` | the tick module or its symbols moving out of window 0 — and, since the 2026-10-14 audit round, the pinned set is `kernel_entry`, `syscall_entry`, `tick`, `zeta_bank_io` and `zeta_uart`, plus the window-0 handoff symbols and every module the source scan finds writing a bank port |
| `tools/check_memory_manager.rb` | a new caller of the platform copy primitives outside `mm/` |
| `tools/check_park_site.rb` | a `park()` call site that does not park `muzix_current_slot(ctx)` |
| `tools/check_proc_identity.rb` | the `10 +`/`NR_TASKS +` array offset added to a process number, a direct variable index into `procs[]`, a local re-definition of a server number, or a new caller of the deprecated `muzix_ipc_get_proc()` |
| `tools/check_z80_call_convention.rb` | a compiled probe disagreeing with the measured return register |
| `tools/check_syscall_context.rb` | a syscall-context field read but never assigned, and the second failure mode it names |
| `tools/check_user_pointers.rb` | a user address used without crossing a copy-in boundary — Muzix's answer to MINIX's `p_splimit` |
| `tools/check_link_scripts.rb` | an `apps/*.lk` or `shell/shell.lk` that is missing or untracked |
| `tools/check_emulator_sources.rb` | an emulator source the build compiles but git does not have |
| `tools/check_userspace_layout.rb` | a userspace image violating what `lib/crt0.s` assumes |
| `tools/check_userspace_runtime.rb` | any userspace link reporting an undefined global |
| `tools/host_tests.rb` | compiles and runs the host-runnable `test_*.c` |
| `tools/rom_validator.rb`, `tools/rom_raw_validator.rb` | ROM layout; the first runs from `test-rom-validator` |

### Documentation

- `docs/hardware-notes.md` — Zeta V2 components and code provenance
- `docs/muzix-architecture.md` — current Muzix and Zeta V2 architecture
- `docs/process-memory.md` — MINIX reference process model and Z80 bank abstraction
- `docs/minix-muzix-audit.md` — Audit of MINIX vs Muzix feature parity
- `PROJECT_STATUS.md` — Current implementation status and boot history
- `../MINIX_MUZIX_COMPARISON.md` — Workspace-level comparison with MINIX
- `Muzix/README.md` — Project overview and build instructions
- `Muzix/BUILD_REPORT.md` — Build status report

The workspace-level `docs/` path refers to the parent directory of
`Muzix/` and is distinct from `Muzix/docs/`.

---

## Source Conventions

- **Language**: Primarily C (SDCC) + Z80 assembly (sdasz80).
- **Naming**: `muzix_` prefix for kernel-internal functions, `sys_` for syscall
  wrappers in userspace, `MUZIX_` prefix for constants/macros.
- **Headers**: `#ifndef MUZIX_..._H` guard style.
- **Memory**: `process_map_t` uses `pages[4]` (one per 16 KB window). Physical
  page numbers `0x00`–`0x1F` map ROM, `0x20`–`0x3F` are RAM, and `0x40+` is
  unmapped/ROM in the platform model.
- **Syscalls**: Userspace entry via `CALL 0x0030` (see `lib/user_syscall.s`).
- **Bank safety**: Never read bank selector ports; always save/restore the
  complete kernel map for temporary bank switches.
- **No commits in repo** of the new code unless explicitly requested by the user.

## Quick Start

```bash
# Build everything
cd Muzix
make clean && make rom && make check

# Run in emulator
cd emulator
make clean && make
./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -v -c 10000000
```
