# Muzix project build system
# Z80 port with SDCC compiler
#
# Incremental: every module is a real file target with gcc-generated header
# dependencies, so `make rom` only recompiles what actually changed. The
# previous version had one target per stage containing ~50 unconditional
# sdcc -c lines, which rebuilt libc, every app, the bootloader and all 50
# kernel modules on every invocation.

SDCC ?= sdcc
SDASZ80 ?= sdasz80
SDLDZ80 ?= sdldz80
HOST_CC ?= gcc
export SDCC SDASZ80 SDLDZ80 HOST_CC
CFLAGS := -mz80
# --peep-file REPLACES SDCC's whole default peephole rule set, so this file has
# to carry every rule the kernel needs.  It carries two, and the kernel links
# with no other optimisation: dropping this file and relying on the defaults
# measured 0xEA2B against 0xEA16 for _CODE, i.e. slightly worse, and left
# ___sdcc_call_iy/_hl unresolved.  (SDCC 4.5 has no --peep-010, so the frame
# pointer cannot be omitted from the command line either - which matters, because
# the kernel stack is only about 1.2 KB.)
# No --peep-file here, deliberately.  It does not add rules, it REPLACES SDCC's
# whole default peephole set with the contents of the file, so the kernel was
# built with the two call rewrites in muzix_sdccall.peep and none of SDCC's own
# optimisations.  That is the root of a whole family of faults found while
# bringing the shell up - a syscall result left in HL across the restore hook
# came back as a _DATA address, an indirect call jumped to the buffer instead of
# the callback, and a frame's SP-relative window test read its own saved
# registers - and every one of them moved when an unrelated module changed size.
# platform/zeta-v2/sdz80_runtime.s now provides ___sdcc_call_hl/_iy directly,
# so the indirection resolves without the rewrite and the default rules apply.
KERNEL_CFLAGS := -mz80 --opt-code-size

# Optional message-based syscall dispatch (microkernel preparation).
# When defined, syscall_dispatch() routes via muzix_system_message_t when
# ctx->vm_base is non-zero; vm_base stays 0 by default so the inline path
# is used and behaviour is unchanged.  Enable with:
#   make rom MUZIX_MESSAGE_DISPATCH=1
# For microkernel build, we enable it by default.
#
# The test is `= 1`, not "non-empty".  `ifneq ($(MUZIX_MESSAGE_DISPATCH),)`
# treats ANY value - including the documented disable, `make rom
# MUZIX_MESSAGE_DISPATCH=0` - as "was set", and defines the macro to 0, which
# is not the same thing as not defining it: the tree's headers test the macro
# with `#if defined(...)` in places, so a define-to-zero takes the routing
# branch it was asked to skip.  The enable path therefore has to be spelled
# "exactly 1", so that disabling actually disables.
MUZIX_MESSAGE_DISPATCH := 1
ifeq ($(MUZIX_MESSAGE_DISPATCH),1)
KERNEL_CFLAGS += -DMUZIX_MESSAGE_DISPATCH
endif

# Used only for -MM dependency scanning. gcc never compiles these sources, so
# SDCC-specific syntax is irrelevant; it only needs to resolve #include.
DEPFLAGS := -I. -Ikernel -Ifs -Imm -Ipm -Iplatform/zeta-v2 \
            -Iplatform/zeta-v2/test_kernel -Ilib -Iapps -Ishell -Itools

# Kernel object files all land in the project root, so vpath resolves the
# module name to its source directory.
vpath %.c kernel fs mm pm platform/zeta-v2 platform/zeta-v2/test_kernel servers/fs servers/mm servers/pm lib
vpath %.s kernel platform/zeta-v2 lib

# Every target in this list has no file of that name on disk; it is a label for
# a recipe, and a label is not a target of anything.  The list was six long,
# which meant `make clean` and `make check` were file-less targets and `make
# test-memory-manager` was not - so a file named `test-memory-manager` in the
# tree (an editor backup, a stray redirect) would make make say "'target' is up
# to date" and run nothing, silently, while every other gate ran.  That is the
# failure mode of a missing .PHONY entry: not an error, a gate that skips.
# Keep it complete.  The aggregate targets are the ones easiest to forget,
# because they look like files (`rom`, `check`, `all`) or like products
# (`build-*`, `test-*`).
.PHONY: help rom clean rebuild all check check-toolchain \
        build-libc build-apps build-shell build-bootloader build-servers-bin \
        build-userspace-bin build-minifs build-rom-boot build-rom-image \
        build-kernel build-kernel-bin \
        test-kernel-layout test-window0 test-syscall-context test-user-pointers \
        test-link-scripts test-emulator-sources test-userspace-layout \
        test-userspace-runtime test-memory-manager test-park-site \
        test-proc-identity test-host test-z80-call-convention \
        test-rom-image test-rom-validator


# ---- Module lists -------------------------------------------------------

# Kernel C modules: <basename>.c -> <basename>.rel
#
# `mm_service` resolves through the vpath line above, and kernel/ is ahead of
# mm/ and servers/mm/ there - so the kernel links kernel/mm_service.c, NOT
# mm/mm_service.c.  Three files share that stem (the third is the copy the
# userspace MM server links per MM_SERVER_C_MODULES); the kernel's is the
# deliberate one, and kernel/mm_service.c's header says why the link is not
# switched over to the message-routed dispatcher.  Editing mm/mm_service.c
# does not change the kernel.
KERNEL_C_MODULES := \
  kernel_main kernel_bank_model trace libc_str shell \
  exec_loader rom_read mm_service mm_copy block_device \
  runtime_state bootstrap proc_table init_task startup \
  system_entry system_service kernel_loop syscalls syscalls_ipc syscall_vector syscall_entry syscall_runtime context_switch \
  rtc_ds1302 load mproc_model ipc system_task proc_context

# Server C modules (userspace servers).
#
# FS_SERVER_MODULES are the ones that live under fs/ and are built by the
# `fs/%.rel: fs/%.c` rule above.  They used to be listed together with fs_server
# and prefixed with servers/fs/, which produced paths such as
# servers/fs/fs_service.rel - a file that cannot exist, since the source is
# fs/fs_service.c - and nothing consumed the resulting list, so the FS service
# layer was never built into anything and the server linked as a thin
# pass-through.  Keeping the halves separate is what makes the paths true.
#
# STEP 2c (2026-10-12): the FS server now links a TTY console, and the modules
# below are exactly what that needs - no more.  fs/tty_device.c and
# zeta_uart_io.rel are the console, fs_clock.c supplies the progress counter
# they need, and the bank primitives are the minimum the UART paths reach.  The
# rest of the service library (fs_service, block_device, volume, ...) is still
# out: it needs the MM server's copy primitives and a ROM block device that does
# not exist yet - see the note in servers/fs/fs_server.lk.
FS_SERVER_MODULES := fs_server fs_clock
FS_SERVER_LIB_MODULES := tty_device
FS_SERVER_SELF_RELS := servers/fs/fs_server.rel
MM_SERVER_C_MODULES := mm_server mm_service mm_copy mproc_model
PM_SERVER_C_MODULES := pm_server

# Kernel assembly modules whose object basename matches the source basename,
# so the generic %.rel: %.s rule can build them.
KERNEL_S_MODULES := z80_io kernel_entry sdz80_runtime tick bitmap_ops

# Assembly modules whose object name differs from the source basename; these
# need explicit rules.
KERNEL_S_EXTRA := z80_syscall_entry.rel zeta_bank_io.rel zeta_uart_io.rel

KERNEL_C_RELS := $(addsuffix .rel,$(KERNEL_C_MODULES))
KERNEL_S_PAT  := $(addsuffix .rel,$(KERNEL_S_MODULES))
KERNEL_S_RELS := $(KERNEL_S_PAT) $(KERNEL_S_EXTRA)

# Server objects.  Everything the FS server link actually consumes, and nothing
# else: its own object and the local clock under servers/fs/, the TTY under fs/,
# and the bank primitives.  The list is what servers/fs/fs_server.lk lists, so
# the two cannot drift.
FS_SERVER_SELF_RELS := $(addprefix servers/fs/,$(addsuffix .rel,$(FS_SERVER_MODULES)))
FS_SERVER_LIB_RELS  := $(addprefix fs/,$(addsuffix .rel,$(FS_SERVER_LIB_MODULES)))
FS_SERVER_RELS := $(FS_SERVER_SELF_RELS) $(FS_SERVER_LIB_RELS)
# The bank primitives the TTY and the UART paths reach.  These already exist as
# build outputs (see the rules above); they are listed here so that changing the
# .lk without adding the object here, or the reverse, stops being a thing that
# links anyway.
FS_SERVER_PLATFORM_RELS := kernel_bank_model.rel bitmap_ops.rel zeta_bank_io.rel \
                           z80_io.rel zeta_uart_io.rel
MM_SERVER_RELS := $(addprefix servers/mm/,$(addsuffix .rel,$(MM_SERVER_C_MODULES)))
PM_SERVER_RELS := $(addprefix servers/pm/,$(addsuffix .rel,$(PM_SERVER_C_MODULES)))
SERVER_RELS := $(FS_SERVER_RELS) $(MM_SERVER_RELS) $(PM_SERVER_RELS)

# Server linker scripts
FS_SERVER_LK := servers/fs/fs_server.lk
MM_SERVER_LK := servers/mm/mm_server.lk
PM_SERVER_LK := servers/pm/pm_server.lk

# Link order is significant and is NOT the build order: sdld places modules in
# the order given, and kernel_entry must be first so the boot stub lands at
# _CODE = 0x0098.  tick.rel must be early for the same kind of reason:
# muzix_tick_isr is the IM1 interrupt handler and has to be in window 0, the
# only window a map change does not take away, and uart_io.s reads the tick
# counter it defines.
#
# 2026-10-14: the window-0-pinned modules are now a GROUPED PREFIX, because
# kernel growth silently pushed the trap's cells (_g_zeta_state_ptr,
# _g_syscall_result, _muzix_fork_stack_len) and the syscall trap itself past
# 0x4000 into window 1 - check_window0.rb failed and the build stopped, which
# is the gate doing its job.  Everything in the first two lines below must stay
# below 0x4000: kernel_entry and tick (entry + ISRs), trace (the ISRs and the
# trap write the ring), context_switch and zeta_bank_io (the only production
# code that programs the bank registers), and the syscall group (the trap
# parks the resume point and installs the map).  The rest is free to float;
# the map-changing callers of the primitives that sit above 0x4000 are the
# documented boot/mid-kernel exceptions.  check_window0.rb derives the
# bank-writer set from the sources, so a new file that programs a port is
# caught here rather than at runtime.
KERNEL_LINK_ORDER := \
  kernel_entry.rel tick.rel trace.rel \
  context_switch.rel zeta_bank_io.rel kernel_bank_model.rel bitmap_ops.rel \
  syscall_vector.rel syscall_entry.rel z80_syscall_entry.rel syscall_runtime.rel \
  zeta_uart_io.rel z80_io.rel rtc_ds1302.rel \
  libc_str.rel rom_read.rel kernel_main.rel shell.rel \
  exec_loader.rel sdz80_runtime.rel \
  mm_service.rel mm_copy.rel block_device.rel \
  runtime_state.rel bootstrap.rel proc_table.rel proc_context.rel load.rel \
  init_task.rel startup.rel system_entry.rel system_service.rel system_task.rel kernel_loop.rel syscalls_ipc.rel syscalls.rel \
  mproc_model.rel ipc.rel

KERNEL_RELS := $(KERNEL_C_RELS) $(KERNEL_S_RELS)

KERNEL_DEPS := $(addsuffix .d,$(KERNEL_C_MODULES))

# Userspace objects keep their directory prefix
LIBC_RELS    := lib/libc.rel lib/syscall.rel
LIBC_S_RELS  := lib/user_syscall.rel lib/crt0.rel lib/math.rel lib/sdcc_runtime.rel lib/server.rel lib/z80_io.rel
APPS_RELS    := apps/echo.rel apps/cat.rel apps/ls.rel apps/date.rel \
               apps/top.rel apps/uptime.rel apps/loadavg.rel apps/hwdiag.rel \
               apps/z80test.rel apps/z80t_asm.rel
SHELL_RELS   := shell/shell.rel
BOOT_RELS    := tools/bootloader.rel
ROMBOOT_RELS := platform/zeta-v2/rom_boot.rel

# Header-dependency files for the non-kernel modules (picked up by -include).
#
# This list is what makes an edit to a header rebuild the objects that consume
# it.  It covered libc, the apps, the shell and the bootloader and NOT the three
# servers, so editing include/muzix_ipc.h left servers/fs/fs_server.rel and its
# siblings untouched: the link then ran against the previous build's objects,
# which is a different failure from a clean tree every time (it built, and it
# broke only on the next person to `make clean`).  The server object lists are
# the variables above, so adding a server module here widens this by itself -
# the same derivation the module lists use.
US_DEPS := $(patsubst %.rel,%.d,$(LIBC_RELS) $(APPS_RELS) $(SHELL_RELS) $(BOOT_RELS) \
                             $(FS_SERVER_RELS) $(MM_SERVER_RELS) $(PM_SERVER_RELS))

# ---- Header dependency generation ---------------------------------------
# $<module>.d lists the module's transitive headers with the target rewritten
# from the .o gcc emits to the .rel we actually build.
%.d: %.c
	@$(HOST_CC) -MM $(DEPFLAGS) $< 2>/dev/null | sed 's|^[^:]*:|$*.rel:|' > $@ || true

# ---- Compile rules ------------------------------------------------------

$(KERNEL_C_RELS): %.rel: %.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(KERNEL_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

$(KERNEL_S_PAT): %.rel: %.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Renamed assembly objects
z80_syscall_entry.rel: platform/zeta-v2/syscall_entry.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

zeta_bank_io.rel: platform/zeta-v2/bank_io.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

zeta_uart_io.rel: platform/zeta-v2/uart_io.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

lib/%.rel: lib/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

lib/%.rel: lib/%.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# server_crt0 is the userspace server startup (FS/MM/PM link it, and it stands up
# the _INITIALIZED/_DATA segments before _main).  It is a .asm, not a .s, so the
# lib/%.rel: lib/%.s rule above never matched it and the checked-in server_crt0.rel
# went stale - which is how the server data-initialisation bug stayed invisible: the
# fix in server_crt0.asm kept failing to appear in the image.  Build it explicitly.
SERVER_CRT0_REL := lib/server_crt0.rel
$(SERVER_CRT0_REL): lib/server_crt0.asm
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

apps/%.rel: apps/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

apps/z80t_asm.rel: apps/z80t_asm.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# FS userspace modules (for fs_server)
fs/%.rel: fs/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Server compile rules
SERVER_CFLAGS := $(CFLAGS) -Iservers/mm -Iservers/fs -Iservers/pm -I. -Ikernel -Ifs -Imm -Ipm -Iplatform/zeta-v2 -Ilib

servers/fs/%.rel: servers/fs/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

servers/mm/%.rel: servers/mm/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

servers/pm/%.rel: servers/pm/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# The build identity the shell prints in its banner, so a running machine can
# be told which tree it came from.  This is not a nicety: the ROM carries no
# version anywhere else, and without it "is the fix I just made on the board?"
# cannot be answered from the board at all - it can only be argued about.
#
# A short hash plus a dirty flag, because a hash alone is worse than nothing
# when the tree was modified after the commit: the number would name a commit
# that is not what ran.  `git diff --quiet HEAD` rather than `git status`,
# because this file is itself generated and untracked, and asking about status
# would make every build report itself dirty.
BUILD_STAMP := shell/muzix_build_stamp.h

$(BUILD_STAMP): FORCE
	@rev=`git rev-parse --short HEAD 2>/dev/null` || rev=""; \
	 if [ -z "$$rev" ]; then \
	   id="no-git"; \
	 elif git diff --quiet HEAD 2>/dev/null; then \
	   id="$$rev"; \
	 else \
	   id="$$rev-dirty"; \
	 fi; \
	 { echo "/* Generated by the Makefile. Do not edit. */"; \
	   echo "#ifndef MUZIX_BUILD_STAMP_H"; \
	   echo "#define MUZIX_BUILD_STAMP_H"; \
	   echo "#define MUZIX_BUILD_ID \"$$id\""; \
	   echo "#endif"; } > $@; \
	 printf "  %-46s %s\n" "$@" "$$id"

FORCE:

shell/shell.rel: shell/shell.c $(BUILD_STAMP)
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

tools/%.rel: tools/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

platform/zeta-v2/rom_boot.rel: platform/zeta-v2/rom_boot.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# ---- Public build stages ------------------------------------------------

# Default target - builds complete ROM image with validation
rom: check-toolchain build-servers-bin build/muzix-zeta-v2.rom test-rom-image test-rom-validator
	@echo ""
	@echo "================================"
	@echo "Muzix Z80 ROM Build: COMPLETE"
	@echo "================================"
	@echo "Output: build/muzix-zeta-v2.rom (512 KiB)"

help:
	@echo "================================"
	@echo "Muzix Z80 Port Build System"
	@echo "================================"
	@echo ""
	@echo "Targets:"
	@echo "  make rom         - Build complete ROM image with validation (DEFAULT)"
	@echo "  make clean       - Remove all generated files"
	@echo "  make rebuild     - Clean and rebuild everything"
	@echo "  make help        - Show this help"
	@echo ""

check-toolchain:
	@set -e; for tool in "$(SDCC)" "$(SDASZ80)" "$(SDLDZ80)" "$(HOST_CC)" ruby; do \
	    command -v "$$tool" >/dev/null 2>&1 || { echo "Required tool not found on PATH: $$tool" >&2; exit 1; }; \
	done
	@version="$$($(SDCC) --version 2>&1 | head -n 1)"; \
	    echo "$$version" | grep -Fq ' 4.5.0 ' || { \
	        echo "Expected SDCC 4.5.0, found: $$version" >&2; exit 1; }

build-libc: $(LIBC_RELS) $(LIBC_S_RELS)

build-apps: $(APPS_RELS)

build-shell: $(SHELL_RELS)

build-bootloader: $(BOOT_RELS)

# ---- Userspace link -----------------------------------------------------
# Each app links in its own directory (the .lk scripts use relative paths)
apps/echo.ihx: apps/echo.rel apps/echo.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/echo"
	@cd apps && $(SDLDZ80) -f echo.lk 2>&1 | grep -qi error && exit 1 || true

apps/cat.ihx: apps/cat.rel apps/cat.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/cat"
	@cd apps && $(SDLDZ80) -f cat.lk 2>&1 | grep -qi error && exit 1 || true

apps/ls.ihx: apps/ls.rel apps/ls.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/ls"
	@cd apps && $(SDLDZ80) -f ls.lk 2>&1 | grep -qi error && exit 1 || true

apps/date.ihx: apps/date.rel apps/date.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/date"
	@cd apps && $(SDLDZ80) -f date.lk 2>&1 | grep -qi error && exit 1 || true

apps/top.ihx: apps/top.rel apps/top.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/top"
	@cd apps && $(SDLDZ80) -f top.lk 2>&1 | grep -qi error && exit 1 || true

apps/uptime.ihx: apps/uptime.rel apps/uptime.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/uptime"
	@cd apps && $(SDLDZ80) -f uptime.lk 2>&1 | grep -qi error && exit 1 || true

apps/loadavg.ihx: apps/loadavg.rel apps/loadavg.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/loadavg"
	@cd apps && $(SDLDZ80) -f loadavg.lk 2>&1 | grep -qi error && exit 1 || true

apps/hwdiag.ihx: apps/hwdiag.rel apps/hwdiag.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/hwdiag"
	@cd apps && $(SDLDZ80) -f hwdiag.lk 2>&1 | grep -qi error && exit 1 || true

apps/z80test.ihx: apps/z80test.rel apps/z80t_asm.rel apps/z80test.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/z80test"
	@cd apps && $(SDLDZ80) -f z80test.lk 2>&1 | grep -qi error && exit 1 || true

shell/shell.ihx: shell/shell.rel shell/shell.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link shell/shell"
	@cd shell && $(SDLDZ80) -f shell.lk 2>&1 | grep -qi error && exit 1 || true

APP_IHX := apps/echo.ihx apps/cat.ihx apps/ls.ihx apps/date.ihx \
           apps/top.ihx apps/uptime.ihx apps/loadavg.ihx apps/hwdiag.ihx \
           apps/z80test.ihx \
           shell/shell.ihx
APP_BIN := build/echo.bin build/cat.bin build/ls.bin \
           build/date.bin build/top.bin build/uptime.bin build/loadavg.bin \
           build/hwdiag.bin build/z80test.bin build/shell.bin

# The converters are prerequisites, not just commands.  A rule that runs
# tools/ihx_to_bin.rb without naming it rebuilds nothing when the converter
# changes, and add_exec_header.rb was missing from the app and server rules
# altogether - so editing the header format changed no image until the next
# person ran `make clean`.
build/%.bin: apps/%.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ $(@:build/%.bin=apps/%.map)

build/shell.bin: shell/shell.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ shell/shell.map

# ---- Server link rules -----------------------------------------------------
# The prerequisites now include $(FS_SERVER_RELS), the FS service modules, not
# just fs_server.rel.  The old rule depended on fs_server.rel alone, so a module
# missing from the .lk was never built and never missed.
#
# The diagnostic filter is one line shared by all three rules, not three
# spellings of it.  sdldz80 reports a module missing from the .lk as
# `Undefined Global '_muzix_mm_copy' referenced by module 'mm_server'`, and that
# string does not contain the word "error" - so a rule that grep'd for "error"
# alone let a dropped module LINK: every call to it resolved to 0x0000, which is
# the boot stub, so the server restarted the machine instead of answering.  The
# FS rule was fixed to match both strings; MM and PM kept the single-"error"
# spelling behind it, which is the same rot one level down.  Spelling the
# filter once is what stops the next spelling falling behind.
#
# The pipeline is `... && exit 1 || true` rather than a plain `exit 1` so that a
# grep finding nothing (a clean link) leaves the recipe going, the way the FS
# rule has always worked.
SERVER_LINK_DIAG := 2>&1 | grep -i "error\|Undefined Global" && exit 1 || true

servers/fs/fs_server.ihx: $(FS_SERVER_RELS) $(FS_SERVER_PLATFORM_RELS) $(FS_SERVER_LK) $(KERNEL_S_RELS) $(LIBC_RELS) $(LIBC_S_RELS) $(SERVER_CRT0_REL)
	@echo "  link servers/fs/fs_server"
	@cd servers/fs && $(SDLDZ80) -f fs_server.lk $(SERVER_LINK_DIAG)

servers/mm/mm_server.ihx: $(MM_SERVER_RELS) $(MM_SERVER_LK) $(LIBC_RELS) $(LIBC_S_RELS) $(SERVER_CRT0_REL)
	@echo "  link servers/mm/mm_server"
	@cd servers/mm && $(SDLDZ80) -f mm_server.lk $(SERVER_LINK_DIAG)

servers/pm/pm_server.ihx: $(PM_SERVER_RELS) $(PM_SERVER_LK) $(LIBC_RELS) $(LIBC_S_RELS) $(SERVER_CRT0_REL)
	@echo "  link servers/pm/pm_server"
	@cd servers/pm && $(SDLDZ80) -f pm_server.lk $(SERVER_LINK_DIAG)

SERVER_IHX := servers/fs/fs_server.ihx servers/mm/mm_server.ihx servers/pm/pm_server.ihx
SERVER_BIN := build/fs_server.bin build/mm_server.bin build/pm_server.bin

build/fs_server.bin: servers/fs/fs_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/fs/fs_server.map

build/mm_server.bin: servers/mm/mm_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/mm/mm_server.map

build/pm_server.bin: servers/pm/pm_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/pm/pm_server.map

build-servers-bin: $(SERVER_BIN)

build-userspace-bin: $(APP_BIN) $(SERVER_BIN)

# ---- MINIX-v1-format filesystem image -----------------------------------
# 'readme' is passed through to make_minixfs.rb even though no such file
# exists in the tree; the script tolerates it, so it is deliberately not a
# prerequisite here.
#
# The two converters are prerequisites as well as make_minixfs.rb: the rule
# consumes what they write (every $(APP_BIN) and every $(SERVER_BIN) is their
# output), so an edit to ihx_to_bin.rb or add_exec_header.rb has to be able to
# rebuild the image, and a rule that only names the last script in the chain
# cannot.
build/muzix-minifs.bin: $(APP_BIN) $(SERVER_BIN) docs/architecture.md docs/hardware-notes.md \
                        docs/memory-copy-notes.md tools/make_minixfs.rb \
                        tools/ihx_to_bin.rb tools/add_exec_header.rb
	@mkdir -p build
	@echo "Building MINIX-v1-format filesystem image..."
	@ruby tools/make_minixfs.rb $@ \
		docs/architecture.md /readme.txt \
		docs/hardware-notes.md /hw-notes.txt \
		docs/memory-copy-notes.md /copy-notes.txt \
		build/echo.bin /bin/echo \
		build/cat.bin /bin/cat \
		build/ls.bin /bin/ls \
		build/date.bin /bin/date \
		build/top.bin /bin/top \
		build/uptime.bin /bin/uptime \
		build/loadavg.bin /bin/loadavg \
		build/hwdiag.bin /bin/hwdiag \
		build/z80test.bin /bin/z80test \
		build/shell.bin /sbin/shell \
		build/fs_server.bin /sbin/fs_server \
		build/mm_server.bin /sbin/mm_server \
		build/pm_server.bin /sbin/pm_server
	@echo "  ✓ minifs complete"

build-minifs: build/muzix-minifs.bin

# ---- ROM boot stub ------------------------------------------------------
build/muzix-rom-boot.bin: $(ROMBOOT_RELS) tools/ihx_to_bin.rb
	@mkdir -p build
	@echo "Building ROM boot stub..."
	@$(SDASZ80) -o platform/zeta-v2/rom_boot.rel platform/zeta-v2/rom_boot.s
	@$(SDLDZ80) -nmi platform/zeta-v2/rom_boot.rel
	@ruby tools/ihx_to_bin.rb platform/zeta-v2/rom_boot.ihx $@ 0 0x98
	@echo "  ✓ rom-boot complete"

build-rom-boot: build/muzix-rom-boot.bin

# ---- Kernel -------------------------------------------------------------
# kernel.ihx -> build/kernel.ihx + build/kernel.map + build/kernel.noi
#
# Two link passes.  The first uses a provisional _DATA purely to learn where
# _CODE really ends; the second places _DATA above that.  sdldz80 does not
# enforce _CODE/_DATA separation - given a base below the end of _CODE it emits
# both and lets them overlap, which is how _muzix_set_syscall_entry ended up
# living inside _DATA and calling it executed data.  A hand-written constant
# cannot stay correct because _CODE moves whenever a module changes size, so
# tools/kernel_data_base.rb derives it instead.
# The window-0 check runs as part of producing the map, not only in
# test-rom-image.  It is the whole enforcement of "only window-0 code may change
# a map" now that the runtime guard is gone: muzix_zeta_caller_in_window0() read
# SP+2 assuming its caller had built no stack frame, and SDCC emits
# `push hl` / `push de` at the top of every forwarder, so it was reading saved
# registers and refusing the child of a fork while passing the parent.  Running
# it here means `make build-kernel` cannot produce a kernel that breaks the
# invariant, whatever else is or is not built afterwards.
#
# PORTABLE GROUPING - why there is a stamp in the middle.
# This rule used to be spelled `build/kernel.map build/kernel.ihx
# build/kernel.noi &:`.  GNU Make 4.3 and later read `&:` as a grouped target:
# one recipe run that produces all three.  Make 3.81 - the make on this box -
# reads it as an ordinary multi-target rule with an extra target literally named
# `&`, so the recipe ran for whichever of the three was wanted and the `&` rule
# produced nothing.  The same spellings meant different builds on different
# versions, and on 3.81 only by accident.  The link is now ONE real target,
# build/kernel.link.stamp, and the three outputs are declared below it.  A
# multi-target rule that has a recipe runs that recipe once per target under
# every GNU make, and this recipe is a file copy, so running it more than once
# changes nothing: the behaviour is now the same under 3.81 and 4.3+ rather than
# similar.  It also restores a single deleted output, which the grouped rule
# did not.
build/kernel.link.stamp: $(KERNEL_RELS) tools/ihx_to_bin.rb tools/kernel_data_base.rb tools/check_window0.rb
	mkdir -p build
	echo "Linking kernel..."
	$(SDLDZ80) -mjwx -i kernel.ihx \
	    -b _CODE=0x0098 -b _DATA=0xEA00 \
	    $(KERNEL_LINK_ORDER) -e 2>&1 || true
	base=`ruby tools/kernel_data_base.rb kernel.map`; \
	 echo "  _CODE ends at `ruby tools/kernel_data_base.rb --end kernel.map`, _DATA=$$base"; \
	 $(SDLDZ80) -mjwx -i kernel.ihx \
	     -b _CODE=0x0098 -b _DATA=$$base \
	     $(KERNEL_LINK_ORDER) -e 2>&1
	ruby tools/check_window0.rb kernel.map .
	touch $@

# The link's three outputs, copied out of the stamp's recipe.  kernel.noi is a
# NoICE debug file and is not produced by every link, so a missing source leaves
# no stale output behind instead of leaving yesterday's one in place.
# The window-0 gate above reads kernel.map - the file the link just wrote, which
# is byte for byte what build/kernel.map is copied from, so it sees the same map
# and every output of the link is checked whichever one is asked for.
build/kernel.ihx build/kernel.map build/kernel.noi: build/kernel.link.stamp
	@if [ -f "$(@F)" ]; then cp "$(@F)" "$@"; else rm -f "$@"; fi

build-kernel: build/kernel.map

build/muzix-kernel.bin: build/kernel.ihx tools/ihx_to_bin.rb
	@mkdir -p build
	@echo "Generating kernel binary..."
	@ruby tools/ihx_to_bin.rb build/kernel.ihx $@ 0x0098 65384
	@echo "  ✓ muzix-kernel.bin complete"

build-kernel-bin: build/muzix-kernel.bin

# ---- ROM image ----------------------------------------------------------
build/muzix-zeta-v2.rom: build/muzix-rom-boot.bin build/muzix-kernel.bin \
                          build/muzix-minifs.bin tools/make_zeta_rom.sh
	@echo "Building Zeta V2 ROM image..."
	@sh tools/make_zeta_rom.sh build/muzix-rom-boot.bin build/muzix-kernel.bin \
	    build/muzix-minifs.bin $@
	@echo "  ✓ muzix-zeta-v2.rom complete"

build-rom-image: build/muzix-zeta-v2.rom

# ---- Validation ---------------------------------------------------------
test-kernel-layout: build/kernel.map
	@ruby tools/check_kernel_layout.rb build/kernel.map

# Only window 0 survives a map change, so every instruction that can write a
# bank register - or that installs a map and then returns - has to be linked
# below 0x4000.  This is what makes the invariant structural rather than a
# comment: muzix_zeta_caller_in_window0() is meant to catch the mistake at run
# time, but it reads SP+2 on the assumption that its caller built no stack
# frame, and SDCC emits `push hl` / `push de` at the top of every forwarder in
# context_switch.c.  The check passed while the system rebooted into the boot
# stub, which is why this exists.  It reads the link map, so growing a module
# past the boundary fails the build instead of the machine.
test-window0: build/kernel.map
	@ruby tools/check_window0.rb build/kernel.map

# The userspace syscall context must be fully initialised, and the running
# process's identity derived exactly once.  Both rules exist because of bugs
# that compiled, linked, and did nothing: ctx->exec_loader was never assigned,
# so exec reported success without loading a program; and ctx->pid was assigned
# twice, the second time from a pid cached at boot, so every syscall was
# attributed to the boot process and a child's exec loaded into its parent's
# slot.  Neither is visible at the call site, and a struct field left NULL is
# indistinguishable from one that is set until something reads it.
test-syscall-context:
	@ruby tools/check_syscall_context.rb .

# A user address must not reach the kernel without crossing a copy-in
# boundary.  MINIX keeps p_splimit - a field in struct proc naming the lowest
# legal stack address - so a process passing anything else is refused by a
# value in the data rather than by a review.  Muzix has no such field: a
# process address is a 16-bit number in a syscall argument, and the only thing
# between it and a kernel dereference is that the author remembers to copy it.
test-user-pointers:
	@ruby tools/check_user_pointers.rb .

# A linker script is hand-written source, not a toolchain output, and every
# app's link rule names one as a prerequisite.  The build produces no .lk at
# all, so a script that is not tracked is not something `make clean` or a
# missing rule would ever report: there is simply no way to link that app on a
# fresh checkout, and nothing fails at the point it goes wrong.
test-link-scripts:
	@ruby tools/check_link_scripts.rb

# The same argument for the emulator: emulator/.gitignore once listed a source
# directory as a build product, so a model the Makefile compiles was not in the
# repository, and a fix to it could sit uncommitted with nothing reporting it.
# This asks the Makefile what it compiles rather than re-deriving it here.
test-emulator-sources:
	@ruby tools/check_emulator_sources.rb

# Every gate, without building the ROM.  `make rom` runs these too, but they
# only need the kernel map and the sources, so a working on the kernel alone
# should not have to produce an image to find out it is wrong.
check: check-toolchain test-kernel-layout test-userspace-layout test-memory-manager \
       test-window0 test-syscall-context test-user-pointers \
       test-link-scripts test-emulator-sources test-userspace-runtime test-host \
       test-park-site test-proc-identity
	@echo "all gates: PASS"

# The test_*.c sources, compiled and RUN on the host.
#
# They were written, linked into .ihx, reported as "BUILT" by a runner that
# started nothing, and never executed once - so they could not fail, which is
# the only property that makes a test worth having.  tools/host_tests.rb finds
# every test_*.c in the tree with a glob, links each one against the module(s)
# it actually calls (the closure is computed by the linker, not listed by
# hand, so a new test is picked up with no edit here), runs it, and fails the
# build on a non-zero exit.  It reports the tests it could not execute, with
# the reason and the count, rather than counting them as passes.
test-host:
	@ruby tools/host_tests.rb .

# Every userspace link resolves. An undefined global in an sdldz80 link is a
# definition at 0x0000, so `call __mullong` becomes `call 0x0000` - the boot
# stub - and the program restarts the machine instead of returning. The link
# rules filter for the string "error", so this warning was being discarded.
test-userspace-runtime:
	@ruby tools/check_userspace_runtime.rb

# Same idea for userspace. Guards the layout lib/crt0.s depends on: _DATA clear
# of _CODE, _DATA below _INITIALIZED/_INITIALIZER (crt0 has no _BSS to clear),
# and the whole image inside the text window exec_loader copies into.
#
# hwdiag is in the list because it is linked: apps/hwdiag.lk produces
# apps/hwdiag.map, and the loop below did not name it, so the ninth userspace
# program was never layout-checked at all.
#
# A missing map is now fatal.  The old loop carried `test -f $$m || continue`,
# which made every entry in the list optional: a map that was never produced -
# because the app did not build, or the link rule moved - skipped the gate
# silently, and a target that can skip any of its work is not a gate.  The maps
# are prerequisites of this target now, so make builds them first; the `test`
# in the loop is the belt to that braces, and it says why.
test-userspace-layout: shell/shell.map apps/echo.map apps/cat.map apps/ls.map \
                       apps/date.map apps/top.map apps/uptime.map \
                       apps/loadavg.map apps/hwdiag.map apps/z80test.map
	@set -e; for m in shell/shell.map apps/echo.map apps/cat.map apps/ls.map \
	                   apps/date.map apps/top.map apps/uptime.map \
	                   apps/loadavg.map apps/hwdiag.map apps/z80test.map; do \
	    test -f $$m || { echo "missing map: $$m - build userspace first" >&2; exit 1; }; \
	    ruby tools/check_userspace_layout.rb $$m 0x8000 0x4000 || exit 1; \
	done

# AGENTS.md: all memory access goes through the memory manager, and there is no
# manual bank switching outside it. Ratcheted against
# tools/mm_violations.txt, so the known set can shrink but never grow.
test-memory-manager:
	@ruby tools/check_memory_manager.rb .

# Which process a park() call is attributing the syscall trap's resume point to.
# The capture lives in two global cells with no owner field, so parking a slot
# that is not the one which just made the system call resumes it at another
# process's stack and the machine reboots.  Ratcheted structurally, so a second
# call site is a build failure rather than a review comment.
test-park-site:
	@ruby tools/check_park_site.rb .

# Process identity: one number, one owner of the procs[] array offset.
#
# The array does not index from zero - the first NR_TASKS entries hold the
# negative system tasks - so `procs[NR_TASKS + nr]` is an ARRAY OFFSET.  Five
# places added that offset to a number they then used as a process number,
# each independently plausible and independently wrong: see the note above
# MUZIX_PROC_FS in include/muzix_ipc.h for the full list and what each broke.
# Between them they produced symptoms that read as filesystem and IPC failures,
# which is why they were each chased separately for days.
#
# The gate refuses the arithmetic outside the one function allowed to have it,
# refuses a local re-definition of a server number, and refuses new callers of
# the deprecated accessor.  It is deliberately narrow - `value * 10 + digit` in
# libc is not an offence - because a gate that cries wolf gets switched off,
# and then it protects nothing.
test-proc-identity:
	@ruby tools/check_proc_identity.rb

# Which register a Z80 return value comes back in, measured off a compiled
# probe rather than read out of the sdcccall(1) documentation.  The modules here
# are plain -mz80 and the linked library is sdcccall(1), so both conventions
# exist in one image; muzix_tick_now() returning its 16-bit value in HL made
# every deadline in the system wrong by an amount nothing else could see.
test-z80-call-convention:
	@ruby tools/check_z80_call_convention.rb .

test-rom-image: check-toolchain build/muzix-zeta-v2.rom test-kernel-layout test-userspace-layout test-memory-manager test-window0 test-z80-call-convention test-syscall-context test-user-pointers test-link-scripts test-emulator-sources test-userspace-runtime test-host test-park-site
	@ruby -e 'rom = File.binread("build/muzix-zeta-v2.rom"); abort "ROM size" unless rom.bytesize == 512 * 1024; abort "kernel entry" unless rom.byteslice(0x98, 2).bytes == [0xf3, 0x31]; abort "MINIX-v1 filesystem magic" unless rom.byteslice(128*1024 + 512 + 13, 2).unpack1("v") == 0x137f; puts "Zeta 512 KiB ROM: kernel + MINIX-v1-format filesystem: PASS"'

test-rom-validator: build/muzix-zeta-v2.rom
	@ruby tools/rom_validator.rb -v build/muzix-zeta-v2.rom

# ---- Housekeeping -------------------------------------------------------
# The directories this build writes objects into.  Written out rather than
# `find .`, because `find .` is not scoped to this project: it follows into
# anything nested in the current directory, and this tree is not the only thing
# there - the agent worktrees under .kilo/ hold .rel files of their own, and a
# clean run from here deleted them out from under live work.  The list is the
# set of output directories the rules above actually write to (kernel objects
# land in the project root, the servers' beside their sources, and so on), so it
# is complete as written; directories that do not exist yet are skipped rather
# than making the build look like it failed.
CLEAN_DIRS := . apps shell lib fs mm pm tools \
              servers/fs servers/mm servers/pm \
              platform/zeta-v2 platform/zeta-v2/test_kernel

clean:
	@echo "Cleaning build artifacts..."
	# *.asm is deleted because SDCC emits one next to every .rel it compiles -
	# with the single exception of lib/server_crt0.asm, the hand-written server
	# startup that FS/MM/PM link.  This is the .lk trap below, one directory
	# over, and it is not theoretical: 2026-10-13, a clean took out the one
	# .asm the build never writes, the link stopped with "No rule to make
	# target lib/server_crt0.asm", and the file had to be reconstructed from
	# lib/crt0.s.  The exclusion is by name, the way the .lk note protects the
	# linker scripts.
	@for d in $(CLEAN_DIRS); do \
	    [ -d "$$d" ] || continue; \
	    find "$$d" -maxdepth 1 -type f \( -name "*.rel" -o -name "*.ihx" \
	        -o -name "*.asm" -o -name "*.lst" -o -name "*.map" \
	        -o -name "*.sym" -o -name "*.noi" -o -name "*.d" \) \
	        ! -name "server_crt0.asm" -delete; \
	done
	# *.lk is deliberately NOT deleted.  This line used to skip the three
	# directories that hold hand-written scripts, which is a trap: a linker
	# script added in a fourth directory is source, and `make clean` would take
	# it out from under the tree.  Measured - the build emits no .lk at all, so
	# there is nothing here to clean and nothing to protect.
	@rm -rf .build
	@rm -rf build
	@rm -f $(BUILD_STAMP)
	@echo "Clean complete."

rebuild: clean rom

all: rom

# Pull in the generated header dependencies. This must stay *after* the
# variable definitions above, because -include is expanded when the line is
# read, not when the rule runs.
-include $(KERNEL_DEPS) $(US_DEPS)
