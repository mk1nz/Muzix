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

# Used only for -MM dependency scanning. gcc never compiles these sources, so
# SDCC-specific syntax is irrelevant; it only needs to resolve #include.
DEPFLAGS := -I. -Ikernel -Ifs -Imm -Ipm -Iplatform/zeta-v2 \
            -Iplatform/zeta-v2/test_kernel -Ilib -Iapps -Ishell -Itools

# Kernel object files all land in the project root, so vpath resolves the
# module name to its source directory.
vpath %.c kernel fs mm pm platform/zeta-v2 platform/zeta-v2/test_kernel
vpath %.s kernel platform/zeta-v2 lib

.PHONY: help rom clean rebuild check-toolchain test-kernel-layout test-userspace-runtime test-host


# ---- Module lists -------------------------------------------------------

# Kernel C modules: <basename>.c -> <basename>.rel
KERNEL_C_MODULES := \
  kernel_main kernel_bank_model trace libc_str syscall_entry_stub shell \
  exec_loader rom_read mm_service mm_copy system_task proc_context \
  runtime_state bootstrap proc_table init_task startup \
  system_entry kernel_loop syscalls system_service pm_service block_device \
  block_cache allocator allocator_store tty_device inode_table inode_store \
  superblock directory directory_store path file_io volume fs_service \
  mproc_model syscall_vector syscall_entry syscall_runtime context_switch \
  rtc_ds1302 load

# Kernel assembly modules whose object basename matches the source basename,
# so the generic %.rel: %.s rule can build them.
KERNEL_S_MODULES := z80_io kernel_entry sdz80_runtime tick

# Assembly modules whose object name differs from the source basename; these
# need explicit rules.
KERNEL_S_EXTRA := z80_syscall_entry.rel zeta_bank_io.rel zeta_uart_io.rel

KERNEL_C_RELS := $(addsuffix .rel,$(KERNEL_C_MODULES))
KERNEL_S_PAT  := $(addsuffix .rel,$(KERNEL_S_MODULES))
KERNEL_S_RELS := $(KERNEL_S_PAT) $(KERNEL_S_EXTRA)

# Link order is significant and is NOT the build order: sdld places modules in
# the order given, and kernel_entry must be first so the boot stub lands at
# _CODE = 0x0098. tick.rel is third-from-last-but-one for the same kind of
# reason: muzix_tick_isr is the IM1 interrupt handler and has to be in window 0,
# which is the only window a map change does not take away.  It is placed
# immediately after kernel_entry for two reasons: everything placed before the
# link reaches 0x4000 shares that property, so it goes in while there is still
# room rather than at the end; and uart_io.s reads the tick counter it defines,
# and sdldz80 resolves a cross-module reference only in link order, so tick has
# to come first or the link reports an undefined symbol.  Keep this list
# byte-identical to the historical one.
KERNEL_LINK_ORDER := \
  kernel_entry.rel tick.rel libc_str.rel rom_read.rel kernel_main.rel shell.rel \
  exec_loader.rel kernel_bank_model.rel trace.rel z80_io.rel context_switch.rel \
  rtc_ds1302.rel \
  syscall_vector.rel syscall_entry.rel z80_syscall_entry.rel syscall_runtime.rel \
  zeta_bank_io.rel zeta_uart_io.rel \
  sdz80_runtime.rel mm_service.rel mm_copy.rel system_task.rel proc_context.rel \
  runtime_state.rel bootstrap.rel proc_table.rel load.rel \
  init_task.rel startup.rel system_entry.rel kernel_loop.rel syscalls.rel \
  system_service.rel pm_service.rel block_device.rel block_cache.rel allocator.rel \
  allocator_store.rel tty_device.rel inode_table.rel inode_store.rel superblock.rel \
  directory.rel directory_store.rel path.rel file_io.rel volume.rel fs_service.rel \
  mproc_model.rel syscall_entry_stub.rel

KERNEL_RELS := $(KERNEL_C_RELS) $(KERNEL_S_RELS)

KERNEL_DEPS := $(addsuffix .d,$(KERNEL_C_MODULES))

# Userspace objects keep their directory prefix
LIBC_RELS    := lib/libc.rel lib/syscall.rel
LIBC_S_RELS  := lib/user_syscall.rel lib/crt0.rel lib/math.rel lib/sdcc_runtime.rel
APPS_RELS    := apps/echo.rel apps/cat.rel apps/ls.rel apps/date.rel \
              apps/top.rel apps/uptime.rel apps/loadavg.rel apps/hwdiag.rel
SHELL_RELS   := shell/shell.rel
BOOT_RELS    := tools/bootloader.rel
ROMBOOT_RELS := platform/zeta-v2/rom_boot.rel

# Header-dependency files for the non-kernel modules (picked up by -include).
US_DEPS := $(patsubst %.rel,%.d,$(LIBC_RELS) $(APPS_RELS) $(SHELL_RELS) $(BOOT_RELS))

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

apps/%.rel: apps/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

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
rom: check-toolchain build/muzix-zeta-v2.rom test-rom-image test-rom-validator
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

shell/shell.ihx: shell/shell.rel shell/shell.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link shell/shell"
	@cd shell && $(SDLDZ80) -f shell.lk 2>&1 | grep -qi error && exit 1 || true

APP_IHX := apps/echo.ihx apps/cat.ihx apps/ls.ihx apps/date.ihx \
          apps/top.ihx apps/uptime.ihx apps/loadavg.ihx apps/hwdiag.ihx \
          shell/shell.ihx
APP_BIN := build/echo.bin build/cat.bin build/ls.bin \
           build/date.bin build/top.bin build/uptime.bin build/loadavg.bin \
           build/hwdiag.bin build/shell.bin

build/%.bin: apps/%.ihx
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ $(@:build/%.bin=apps/%.map)

build/shell.bin: shell/shell.ihx
	@mkdir -p build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ shell/shell.map

build-userspace-bin: $(APP_BIN)

# ---- MINIX-v1-format filesystem image -----------------------------------
# 'readme' is passed through to make_minixfs.rb even though no such file
# exists in the tree; the script tolerates it, so it is deliberately not a
# prerequisite here.
build/muzix-minifs.bin: $(APP_BIN) docs/architecture.md docs/hardware-notes.md \
                       docs/memory-copy-notes.md tools/make_minixfs.rb
	@mkdir -p build
	@echo "Building MINIX-v1-format filesystem image..."
	@ruby tools/make_minixfs.rb $@ \
		docs/architecture.md readme \
		docs/hardware-notes.md hw-notes \
		docs/memory-copy-notes.md copy-notes \
		build/echo.bin echo \
		build/cat.bin cat \
		build/ls.bin ls \
		build/date.bin date \
		build/top.bin top \
		build/uptime.bin uptime \
		build/loadavg.bin loadavg \
		build/hwdiag.bin hwdiag \
		build/shell.bin shell
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
# kernel.ihx -> build/kernel.ihx + build/kernel.map
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
build/kernel.map build/kernel.ihx build/kernel.noi &: $(KERNEL_RELS) tools/ihx_to_bin.rb tools/kernel_data_base.rb tools/check_window0.rb
	@mkdir -p build
	@echo "Linking kernel..."
	@$(SDLDZ80) -mjwx -i kernel.ihx \
	    -b _CODE=0x0098 -b _DATA=0xEA00 \
	    $(KERNEL_LINK_ORDER) -e 2>&1 || true
	@base=`ruby tools/kernel_data_base.rb kernel.map`; \
	 echo "  _CODE ends at `ruby tools/kernel_data_base.rb --end kernel.map`, _DATA=$$base"; \
	 $(SDLDZ80) -mjwx -i kernel.ihx \
	     -b _CODE=0x0098 -b _DATA=$$base \
	     $(KERNEL_LINK_ORDER) -e 2>&1
	@cp kernel.ihx build/kernel.ihx
	@cp kernel.map build/kernel.map
	@cp kernel.noi build/kernel.noi 2>/dev/null || true
	@echo "  ✓ kernel.ihx complete"
	@ruby tools/check_window0.rb build/kernel.map .

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
       test-park-site
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
test-userspace-layout:
	@set -e; for m in shell/shell.map apps/echo.map apps/cat.map apps/ls.map \
	                   apps/date.map apps/top.map apps/uptime.map \
	                   apps/loadavg.map; do \
	    test -f $$m || continue; \
	    ruby tools/check_userspace_layout.rb $$m 0x8000 0x4000; \
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
clean:
	@echo "Cleaning build artifacts..."
	@find . -type f \( -name "*.rel" -o -name "*.ihx" -o -name "*.asm" \
		-o -name "*.lst" -o -name "*.map" -o -name "*.sym" \
		-o -name "*.noi" -o -name "*.d" \) -delete
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
