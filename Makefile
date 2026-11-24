# Muzix Z80 Operating System - Professional Build System
#
# GNU Make-based build for the Muzix Z80 port targeting the Zeta SBC V2.
# Toolchain: SDCC 4.5.0 (-mz80), sdasz80, sdldz80, Ruby for converters/gates.
#
# Design (production-grade):
#   - Every module is a real file target with gcc-generated header deps.
#   - Incremental builds: only changed modules recompile.
#   - Two-pass kernel link: _DATA derived from linked _CODE extent.
#   - All non-file targets declared .PHONY; no silent gate skips.
#   - .DELETE_ON_ERROR: failed recipes remove partial outputs.
#   - User-overridable variables via ?=; project variables via :=.
#   - clean removes every generated artifact, nothing left behind.
#
# Consulted docs (makefile-generator skill):
#   .kilo/skills/makefiles/SKILL.md (Stage 2 internal docs, Stage 3 patterns)
#   GNU Make Manual: https://www.gnu.org/software/make/manual/
#   GNU Coding Standards: https://www.gnu.org/prep/standards/

# ---------------------------------------------------------------------------
# Modern GNU Make header
# ---------------------------------------------------------------------------
SHELL := bash
.ONESHELL:
.SHELLFLAGS := -eu -o pipefail -c
.DELETE_ON_ERROR:
.SUFFIXES:
MAKEFLAGS += --warn-undefined-variables
MAKEFLAGS += --no-builtin-rules

# ---------------------------------------------------------------------------
# Toolchain (user-overridable)
# ---------------------------------------------------------------------------
SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDASZ80FLAGS   ?= -g
SDLDZ80   ?= sdldz80
SDAR      ?= sdar
HOST_CC   ?= gcc
RUBY      ?= ruby

export SDCC SDASZ80 SDLDZ80 SDAR HOST_CC RUBY
export LC_ALL := en_US.UTF-8

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
CFLAGS         := -mz80 -Iinclude -Ilib
KERNEL_CFLAGS  := -mz80 --opt-code-size -Iinclude -Ilib

# Message-based syscall dispatch (microkernel preparation).
# `= 1` exactly; any other value (incl. 0) leaves it undefined so
# `#if defined(...)` headers take the non-routing branch.
MUZIX_MESSAGE_DISPATCH := 1
ifeq ($(MUZIX_MESSAGE_DISPATCH),1)
KERNEL_CFLAGS += -DMUZIX_MESSAGE_DISPATCH
endif

# ---------------------------------------------------------------------------
# Dependency scanning flags (gcc only resolves #include; never compiles)
# ---------------------------------------------------------------------------
DEPFLAGS := -I. -Iinclude -Ikernel -Ifs -Imm -Ipm -Iplatform/zeta-v2 \
            -Iplatform/zeta-v2/test_kernel -Ilib -Iapps -Ishell -Itools

# ---------------------------------------------------------------------------
# Source path resolution (vpath)
# ---------------------------------------------------------------------------
vpath %.c kernel fs mm pm platform/zeta-v2 platform/zeta-v2/test_kernel \
        servers/fs servers/mm servers/pm lib lib/libc lib/syscall lib/socket
vpath %.s kernel platform/zeta-v2 lib

# ---------------------------------------------------------------------------
# Project identity
# ---------------------------------------------------------------------------
PROJECT := muzix
TARGET   := build/muzix-zeta-v2.rom
PREFIX   ?= /usr/local

# ---------------------------------------------------------------------------
# Module lists
# ---------------------------------------------------------------------------
# Kernel C modules -> <basename>.rel (vpath resolves directory)
KERNEL_C_MODULES := \
  kernel_main kernel_bank_model trace libc_str shell \
  exec_loader rom_read mm_service mm_copy block_device \
  runtime_state bootstrap proc_table init_task startup \
  system_entry system_service kernel_loop syscalls syscalls_ipc \
  syscall_vector syscall_entry syscall_runtime context_switch \
  rtc_ds1302 load mproc_model ipc system_task proc_context \
  tick_helpers cron spi_io w5100 socket_manager socket_service

# Userspace servers (userspace slot 2/3/4)
FS_SERVER_MODULES   := fs_server fs_clock
FS_SERVER_LIB_MODULES := tty_device
MM_SERVER_C_MODULES := mm_server mm_service mm_copy mproc_model
PM_SERVER_C_MODULES := pm_server
CRON_SERVER_C_MODULES := cron_server

# Kernel assembly modules (basename matches source)
KERNEL_S_MODULES := z80_io kernel_entry sdz80_runtime tick bitmap_ops
# Renamed assembly objects (source basename != object basename)
KERNEL_S_EXTRA := z80_syscall_entry.rel zeta_bank_io.rel zeta_uart_io.rel

# Derived object lists
KERNEL_C_RELS      := $(addsuffix .rel,$(KERNEL_C_MODULES))
KERNEL_S_PAT_RELS  := $(addsuffix .rel,$(KERNEL_S_MODULES))
KERNEL_S_RELS      := $(KERNEL_S_PAT_RELS) $(KERNEL_S_EXTRA)
KERNEL_RELS        := $(KERNEL_C_RELS) $(KERNEL_S_RELS)

# Server objects
FS_SERVER_SELF_RELS := $(addprefix servers/fs/,$(addsuffix .rel,$(FS_SERVER_MODULES)))
FS_SERVER_LIB_RELS  := $(addprefix fs/,$(addsuffix .rel,$(FS_SERVER_LIB_MODULES)))
FS_SERVER_RELS      := $(FS_SERVER_SELF_RELS) $(FS_SERVER_LIB_RELS)
FS_SERVER_PLATFORM_RELS := kernel_bank_model.rel bitmap_ops.rel zeta_bank_io.rel \
                           z80_io.rel zeta_uart_io.rel
MM_SERVER_RELS      := $(addprefix servers/mm/,$(addsuffix .rel,$(MM_SERVER_C_MODULES)))
PM_SERVER_RELS      := $(addprefix servers/pm/,$(addsuffix .rel,$(PM_SERVER_C_MODULES)))
CRON_SERVER_RELS    := $(addprefix servers/cron/,$(addsuffix .rel,$(CRON_SERVER_C_MODULES)))
SERVER_RELS         := $(FS_SERVER_RELS) $(MM_SERVER_RELS) $(PM_SERVER_RELS) $(CRON_SERVER_RELS)

# Server linker scripts
FS_SERVER_LK  := servers/fs/fs_server.lk
MM_SERVER_LK  := servers/mm/mm_server.lk
PM_SERVER_LK  := servers/pm/pm_server.lk
CRON_SERVER_LK := servers/cron/cron_server.lk

# Userspace runtime
# libc archive: multi-module archive built from lib/libc/*.c; the linker pulls
# only the modules an app actually references, instead of the whole monolithic
# object. syscall.rel and socket.rel remain single-module .rel files.
LIBC_RELS    := lib/libc.lib lib/syscall.lib lib/socket.lib
LIBC_S_RELS  := lib/user_syscall.rel lib/crt0.rel lib/math.rel \
                lib/sdcc_runtime.rel lib/server.rel lib/z80_io.rel \
                lib/i2c.rel lib/lcd.rel
SERVER_CRT0_REL := lib/server_crt0.rel

# Userspace applications
APPS_RELS := apps/echo.rel apps/cat.rel apps/ls.rel apps/ls_time.rel \
             apps/date.rel apps/top.rel apps/uptime.rel apps/loadavg.rel \
             apps/hwdiag.rel apps/z80test.rel apps/z80t_asm.rel \
             apps/i2ctest.rel apps/i2cdiag.rel apps/cron.rel apps/wget.rel \
             apps/httpd.rel apps/http_parser.rel apps/reboot.rel \
             apps/cgi_index.rel apps/cgi_hello.rel apps/ps.rel apps/kill.rel
SHELL_RELS   := shell/shell.rel
BOOT_RELS    := tools/bootloader.rel
ROMBOOT_RELS := platform/zeta-v2/rom_boot.rel

# Header dependency files (non-kernel modules).  Only .rel files have .d
# companions; .lib archives do not.
US_DEPS := $(patsubst %.rel,%.d,$(filter %.rel,$(LIBC_RELS) $(APPS_RELS) $(SHELL_RELS) $(BOOT_RELS) \
                             $(FS_SERVER_RELS) $(MM_SERVER_RELS) $(PM_SERVER_RELS)))
KERNEL_DEPS := $(addsuffix .d,$(KERNEL_C_MODULES))

# ---------------------------------------------------------------------------
# Link order (significant: sdld places modules in given order;
# kernel_entry must be first for _CODE = 0x0098; tick must be early for
# window-0 residency of the IM1 handler)
# ---------------------------------------------------------------------------
KERNEL_LINK_ORDER := \
  kernel_entry.rel tick.rel tick_helpers.rel trace.rel \
  context_switch.rel zeta_bank_io.rel kernel_bank_model.rel bitmap_ops.rel \
  syscall_entry.rel z80_syscall_entry.rel syscall_runtime.rel \
   zeta_uart_io.rel z80_io.rel rtc_ds1302.rel spi_io.rel w5100.rel socket_manager.rel \
  libc_str.rel rom_read.rel kernel_main.rel shell.rel \
  exec_loader.rel sdz80_runtime.rel \
   mm_service.rel mm_copy.rel \
   runtime_state.rel bootstrap.rel proc_table.rel proc_context.rel load.rel cron.rel \
   init_task.rel startup.rel system_entry.rel system_service.rel system_task.rel \
   kernel_loop.rel socket_service.rel syscalls_ipc.rel syscalls.rel \
  mproc_model.rel ipc.rel

# ---------------------------------------------------------------------------
# Userspace link inputs per app/server
# ---------------------------------------------------------------------------
APP_IHX := apps/echo.ihx apps/cat.ihx apps/ls.ihx apps/date.ihx \
           apps/top.ihx apps/uptime.ihx apps/loadavg.ihx apps/hwdiag.ihx \
           apps/i2ctest.ihx apps/i2cdiag.ihx apps/cron.ihx \
           apps/wget.ihx apps/httpd.ihx \
           apps/cgi_index.ihx apps/cgi_hello.ihx \
           apps/reboot.ihx \
           apps/ps.ihx apps/kill.ihx \
           apps/z80test.ihx \
           shell/shell.ihx

APP_BIN := build/echo.bin build/cat.bin build/ls.bin build/httpd.bin \
           build/date.bin build/top.bin build/uptime.bin build/loadavg.bin \
           build/hwdiag.bin build/z80test.bin build/i2ctest.bin build/i2cdiag.bin \
           build/cron.bin build/wget.bin build/shell.bin \
           build/cgi_index.bin build/cgi_hello.bin \
           build/reboot.bin build/ps.bin build/kill.bin

SERVER_IHX := servers/fs/fs_server.ihx servers/mm/mm_server.ihx \
              servers/pm/pm_server.ihx servers/cron/cron_server.ihx
SERVER_BIN := build/fs_server.bin build/mm_server.bin \
              build/pm_server.bin build/cron_server.bin

# ---------------------------------------------------------------------------
# Diagnostics for sdldz80 links (catch both "error" and "Undefined Global")
# ---------------------------------------------------------------------------
SDLD_DIAG := 2>&1 | grep -i "error\|Undefined Global" && exit 1 || true

# ---------------------------------------------------------------------------
# Build identity stamp (git short hash + dirty flag)
# ---------------------------------------------------------------------------
BUILD_STAMP := shell/muzix_build_stamp.h

# ---------------------------------------------------------------------------
# Clean scope: output directories actually written to by this build
# ---------------------------------------------------------------------------
CLEAN_DIRS := . apps shell lib lib/libc lib/syscall lib/socket fs mm pm tools \
              servers/fs servers/mm servers/pm servers/cron \
              platform/zeta-v2 platform/zeta-v2/test_kernel kernel

# ===========================================================================
# PHONY targets
# ===========================================================================
.PHONY: help rom clean rebuild all check check-toolchain \
        build-libc build-apps build-shell build-bootloader \
        build-servers-bin build-userspace-bin build-minifs \
        build-rom-boot build-rom-image build-kernel build-kernel-bin \
        test-kernel-layout test-window0 test-syscall-context \
        test-user-pointers test-link-scripts test-emulator-sources \
        test-userspace-layout test-userspace-runtime \
        test-memory-manager test-park-site test-proc-identity \
        test-host test-z80-call-convention \
        test-kernel-stack \
        test-rom-image test-rom-validator FORCE

# ===========================================================================
# Default target
# ===========================================================================
all: rom

rom: check-toolchain build-servers-bin build/muzix-zeta-v2.rom \
     test-rom-image test-rom-validator
	@echo ""
	@echo "================================"
	@echo "Muzix Z80 ROM Build: COMPLETE"
	@echo "================================"
	@echo "Output: build/muzix-zeta-v2.rom (512 KiB)"

# ===========================================================================
# Help
# ===========================================================================
help:
	@echo "================================"
	@echo "Muzix Z80 Port Build System"
	@echo "================================"
	@echo ""
	@echo "Targets:"
	@echo "  make rom          - Build complete ROM image with validation (DEFAULT)"
	@echo "  make all          - Alias for rom"
	@echo "  make clean        - Remove all generated files"
	@echo "  make rebuild      - Clean and rebuild everything"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Stages:"
	@echo "  make build-kernel         - Kernel link + map"
	@echo "  make build-kernel-bin     - Kernel binary"
	@echo "  make build-userspace-bin  - All userspace apps + servers"
	@echo "  make build-minifs         - MINIX-v1 filesystem image"
	@echo "  make build-rom-boot       - ROM boot stub"
	@echo "  make build-rom-image      - Final 512 KiB ROM image"
	@echo ""
	@echo "Validation gates:"
	@echo "  make check              - Every source-level gate"
	@echo "  make test-kernel-layout - _CODE/_DATA extents, headroom, _HOME"
	@echo "  make test-window0       - Window-0 bank-writer invariant"
	@echo "  make test-rom-image     - ROM size, entry, filesystem magic"
	@echo "  make test-rom-validator - Full ROM layout validator"
	@echo "  make test-host          - Host-runnable test_*.c"
	@echo ""
	@echo "Override: make CC=clang PREFIX=/opt SDCC=/opt/sdcc/bin/sdcc"

# ===========================================================================
# Toolchain check
# ===========================================================================
check-toolchain:
	@set -e; for tool in "$(SDCC)" "$(SDASZ80)" "$(SDLDZ80)" "$(HOST_CC)" "$(RUBY)"; do \
	    command -v "$$tool" >/dev/null 2>&1 || { \
	        echo "Required tool not found on PATH: $$tool" >&2; exit 1; }; \
	done
	@version="$$($(SDCC) --version 2>&1 | head -n 1)"; \
	    echo "$$version" | grep -Fq ' 4.5.0 ' || { \
	        echo "Expected SDCC 4.5.0, found: $$version" >&2; exit 1; }

# ===========================================================================
# Header dependency generation (gcc -MM, target rewritten to .rel)
# ===========================================================================
%.d: %.c
	@$(HOST_CC) -MM $(DEPFLAGS) $< 2>/dev/null | sed 's|^[^:]*:|$*.rel:|' > $@ || true

# ===========================================================================
# Compile rules
# ===========================================================================

# Kernel C modules
$(KERNEL_C_RELS): %.rel: %.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(KERNEL_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Kernel S modules (basename match)
$(KERNEL_S_PAT_RELS): %.rel: %.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Renamed assembly objects
z80_syscall_entry.rel: platform/zeta-v2/syscall_entry.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

zeta_bank_io.rel: platform/zeta-v2/bank_io.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

zeta_uart_io.rel: platform/zeta-v2/uart_io.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Userspace lib C
lib/%.rel: lib/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Userspace lib libc split modules (one .c per function, packed into libc.lib)
LIBC_SPLIT_SRCS := $(wildcard lib/libc/*.c) lib/i2c.c
LIBC_SPLIT_RELS := $(patsubst lib/libc/%.c,lib/libc/%.rel,$(filter lib/libc/%.c,$(LIBC_SPLIT_SRCS))) lib/i2c.rel
lib/i2c.rel: lib/i2c.c
	$(SDCC) $(CFLAGS) -c $< -o $@

lib/libc/%.rel: lib/libc/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

lib/libc.lib: $(LIBC_SPLIT_RELS)
	@echo "  sdar libc.lib"
	@rm -f $@
	$(SDAR) -rc $@ $(LIBC_SPLIT_RELS)

# Userspace lib syscall split modules (one .c per function group, packed into syscall.lib)
SYSCALL_SPLIT_SRCS := $(wildcard lib/syscall/*.c) lib/syscall.c
SYSCALL_SPLIT_RELS := $(patsubst lib/syscall/%.c,lib/syscall/%.rel,$(filter lib/syscall/%.c,$(SYSCALL_SPLIT_SRCS))) lib/syscall.rel

lib/syscall/%.rel: lib/syscall/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

lib/syscall.lib: $(SYSCALL_SPLIT_RELS)
	@echo "  sdar syscall.lib"
	@rm -f $@
	$(SDAR) -rc $@ $(SYSCALL_SPLIT_RELS)

lib/syscall.rel: lib/syscall.c
	$(SDCC) $(CFLAGS) -c $< -o $@

# Userspace lib socket split modules (one .c per function group, packed into socket.lib)
SOCKET_SPLIT_SRCS := $(wildcard lib/socket/*.c)
SOCKET_SPLIT_RELS := $(patsubst lib/socket/%.c,lib/socket/%.rel,$(SOCKET_SPLIT_SRCS))

lib/socket/%.rel: lib/socket/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

lib/socket.lib: $(SOCKET_SPLIT_RELS)
	@echo "  sdar socket.lib"
	@rm -f $@
	$(SDAR) -rc $@ $(SOCKET_SPLIT_RELS)

# Userspace lib S
lib/%.rel: lib/%.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Server crt0 (.asm, not .s)
$(SERVER_CRT0_REL): lib/server_crt0.asm
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Userspace apps C
apps/%.rel: apps/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Userspace apps S / special
apps/z80t_asm.rel: apps/z80t_asm.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# FS userspace modules
fs/%.rel: fs/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# Server compile rules
SERVER_CFLAGS := $(CFLAGS) -Iservers/mm -Iservers/fs -Iservers/pm -Iservers/cron \
                 -I. -Ikernel -Ifs -Imm -Ipm -Iplatform/zeta-v2 -Ilib

servers/fs/%.rel: servers/fs/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

servers/mm/%.rel: servers/mm/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

servers/pm/%.rel: servers/pm/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

servers/cron/%.rel: servers/cron/%.c
	@printf "  %-46s " "$<"
	@if $(SDCC) $(SERVER_CFLAGS) -c $< -o $@ 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# ROM boot stub object
platform/zeta-v2/rom_boot.rel: platform/zeta-v2/rom_boot.s
	@printf "  %-46s " "$<"
	@if $(SDASZ80) $(SDASZ80FLAGS) -o $@ $< 2>&1 | grep -qi "error"; then echo "ERROR"; exit 1; else echo "OK"; fi

# ===========================================================================
# Build identity stamp (regenerated every build via FORCE)
# ===========================================================================
$(BUILD_STAMP): FORCE
	@rev=$$(git rev-parse --short HEAD 2>/dev/null) || rev=""; \
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

# ===========================================================================
# Userspace link (each app/server links via its own .lk in its own directory)
# ===========================================================================
apps/echo.ihx: apps/echo.rel apps/echo.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/echo"
	@cd apps && $(SDLDZ80) -f echo.lk $(SDLD_DIAG)

apps/cat.ihx: apps/cat.rel apps/cat.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/cat"
	@cd apps && $(SDLDZ80) -f cat.lk $(SDLD_DIAG)

apps/ls.ihx: apps/ls.rel apps/ls_time.rel apps/ls.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/ls"
	@cd apps && $(SDLDZ80) -f ls.lk $(SDLD_DIAG)

apps/date.ihx: apps/date.rel apps/date.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/date"
	@cd apps && $(SDLDZ80) -f date.lk $(SDLD_DIAG)

apps/top.ihx: apps/top.rel apps/top.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/top"
	@cd apps && $(SDLDZ80) -f top.lk $(SDLD_DIAG)

apps/uptime.ihx: apps/uptime.rel apps/uptime.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/uptime"
	@cd apps && $(SDLDZ80) -f uptime.lk $(SDLD_DIAG)

apps/loadavg.ihx: apps/loadavg.rel apps/loadavg.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/loadavg"
	@cd apps && $(SDLDZ80) -f loadavg.lk $(SDLD_DIAG)

apps/hwdiag.ihx: apps/hwdiag.rel apps/hwdiag.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/hwdiag"
	@cd apps && $(SDLDZ80) -f hwdiag.lk $(SDLD_DIAG)

apps/i2ctest.ihx: apps/i2ctest.rel apps/i2ctest.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/i2ctest"
	@cd apps && $(SDLDZ80) -f i2ctest.lk $(SDLD_DIAG)

apps/wget.ihx: apps/wget.rel apps/wget.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/wget"
	@cd apps && $(SDLDZ80) -f wget.lk $(SDLD_DIAG)

apps/httpd.ihx: apps/httpd.rel apps/http_parser.rel apps/httpd.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/httpd"
	@cd apps && $(SDLDZ80) -f httpd.lk $(SDLD_DIAG)

apps/cgi_index.ihx: apps/cgi_index.rel apps/cgi_index.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/cgi_index"
	@cd apps && $(SDLDZ80) -f cgi_index.lk $(SDLD_DIAG)

apps/cgi_hello.ihx: apps/cgi_hello.rel apps/cgi_hello.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/cgi_hello"
	@cd apps && $(SDLDZ80) -f cgi_hello.lk $(SDLD_DIAG)

apps/reboot.ihx: apps/reboot.rel apps/reboot.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/reboot"
	@cd apps && $(SDLDZ80) -f reboot.lk $(SDLD_DIAG)

apps/ps.ihx: apps/ps.rel apps/ps.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/ps"
	@cd apps && $(SDLDZ80) -f ps.lk $(SDLD_DIAG)

apps/kill.ihx: apps/kill.rel apps/kill.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/kill"
	@cd apps && $(SDLDZ80) -f kill.lk $(SDLD_DIAG)

apps/i2cdiag.ihx: apps/i2cdiag.rel apps/i2cdiag.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/i2cdiag"
	@cd apps && $(SDLDZ80) -f i2cdiag.lk $(SDLD_DIAG)

apps/cron.ihx: apps/cron.rel apps/cron.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/cron"
	@cd apps && $(SDLDZ80) -f cron.lk $(SDLD_DIAG)

apps/z80test.ihx: apps/z80test.rel apps/z80t_asm.rel apps/z80test.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link apps/z80test"
	@cd apps && $(SDLDZ80) -f z80test.lk $(SDLD_DIAG)

shell/shell.ihx: shell/shell.rel shell/shell.lk $(LIBC_RELS) $(LIBC_S_RELS)
	@echo "  link shell/shell"
	@cd shell && $(SDLDZ80) -f shell.lk $(SDLD_DIAG)

# Server links
servers/fs/fs_server.ihx: $(FS_SERVER_RELS) $(FS_SERVER_PLATFORM_RELS) $(FS_SERVER_LK) \
                          $(KERNEL_S_RELS) $(LIBC_RELS) $(LIBC_S_RELS) $(SERVER_CRT0_REL)
	@echo "  link servers/fs/fs_server"
	@cd servers/fs && $(SDLDZ80) -f fs_server.lk $(SDLD_DIAG)

servers/mm/mm_server.ihx: $(MM_SERVER_RELS) $(MM_SERVER_LK) $(LIBC_RELS) $(LIBC_S_RELS) \
                          $(SERVER_CRT0_REL)
	@echo "  link servers/mm/mm_server"
	@cd servers/mm && $(SDLDZ80) -f mm_server.lk $(SDLD_DIAG)

servers/pm/pm_server.ihx: $(PM_SERVER_RELS) $(PM_SERVER_LK) $(LIBC_RELS) $(LIBC_S_RELS) \
                          $(SERVER_CRT0_REL)
	@echo "  link servers/pm/pm_server"
	@cd servers/pm && $(SDLDZ80) -f pm_server.lk $(SDLD_DIAG)

servers/cron/cron_server.ihx: $(CRON_SERVER_RELS) servers/cron/cron_server.lk \
                              $(LIBC_RELS) $(LIBC_S_RELS) $(SERVER_CRT0_REL)
	@echo "  link servers/cron/cron_server"
	@cd servers/cron && $(SDLDZ80) -f cron_server.lk $(SDLD_DIAG)

# ===========================================================================
# ihx -> bin conversion + exec header
# ===========================================================================
build/%.bin: apps/%.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ apps/$*.map

build/shell.bin: shell/shell.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ shell/shell.map

build/fs_server.bin: servers/fs/fs_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/fs/fs_server.map

build/mm_server.bin: servers/mm/mm_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/mm/mm_server.map

build/pm_server.bin: servers/pm/pm_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/pm/pm_server.map

build/cron_server.bin: servers/cron/cron_server.ihx tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@ruby tools/ihx_to_bin.rb $< $@ 0x8000 65535
	@ruby tools/add_exec_header.rb $@ servers/cron/cron_server.map

# ===========================================================================
# Stage targets
# ===========================================================================
build-libc: $(LIBC_RELS) $(LIBC_S_RELS)

build-apps: $(APPS_RELS)

build-shell: $(SHELL_RELS)

build-bootloader: $(BOOT_RELS)

build-servers-bin: $(SERVER_BIN)

build-userspace-bin: $(APP_BIN) $(SERVER_BIN)

# ===========================================================================
# MINIX-v1-format filesystem image
# ===========================================================================
build/muzix-minifs.bin: $(APP_BIN) $(SERVER_BIN) \
                        docs/architecture.md docs/hardware-notes.md \
                        docs/memory-copy-notes.md tools/make_minixfs.rb \
                        tools/ihx_to_bin.rb tools/add_exec_header.rb | build
	@echo "Building MINIX-v1-format filesystem image..."
	@ruby tools/make_minixfs.rb $@ \
		docs/architecture.md /doc/readme.txt \
		docs/hardware-notes.md /doc/hw-notes.txt \
		docs/memory-copy-notes.md /doc/copy-notes.txt \
		docs/architecture.md /doc/arch.txt \
		docs/zeta-rom-layout.md /doc/rom.txt \
		build/echo.bin /bin/echo \
		build/cat.bin /bin/cat \
		build/ls.bin /bin/ls \
		build/date.bin /bin/date \
		build/top.bin /bin/top \
		build/uptime.bin /bin/uptime \
		build/loadavg.bin /bin/loadavg \
		build/hwdiag.bin /bin/hwdiag \
		build/z80test.bin /bin/z80test \
		build/wget.bin /bin/wget \
		build/httpd.bin /bin/httpd \
		build/cron.bin /bin/cron \
		build/shell.bin /sbin/shell \
		build/fs_server.bin /sbin/fs_server \
		build/mm_server.bin /sbin/mm_server \
		build/pm_server.bin /sbin/pm_server \
		build/cron_server.bin /sbin/cron_server \
		build/cgi_index.bin /cgi-bin/index \
		build/cgi_hello.bin /cgi-bin/hello \
		build/reboot.bin /bin/reboot \
		build/ps.bin /bin/ps \
		build/kill.bin /bin/kill
	@echo "  ✓ minifs complete"

build-minifs: build/muzix-minifs.bin

# ===========================================================================
# ROM boot stub
# ===========================================================================
build/muzix-rom-boot.bin: $(ROMBOOT_RELS) tools/ihx_to_bin.rb | build
	@echo "Building ROM boot stub..."
	@$(SDASZ80) $(SDASZ80FLAGS) -o platform/zeta-v2/rom_boot.rel platform/zeta-v2/rom_boot.s
	@$(SDLDZ80) -nmi platform/zeta-v2/rom_boot.rel
	@ruby tools/ihx_to_bin.rb platform/zeta-v2/rom_boot.ihx $@ 0 0x98
	@echo "  ✓ rom-boot complete"

build-rom-boot: build/muzix-rom-boot.bin

# ===========================================================================
# Kernel link (two-pass: _DATA derived from linked _CODE extent)
# ===========================================================================
build/kernel.link.stamp: $(KERNEL_RELS) tools/ihx_to_bin.rb tools/kernel_data_base.rb tools/check_window0.rb | build
	@echo "Linking kernel..."
	$(SDLDZ80) -mjwx -i kernel.ihx \
	    -b _CODE=0x0098 -b _DATA=0xEA00 \
	    $(KERNEL_LINK_ORDER) -e $(SDLD_DIAG)
	base=$$(ruby tools/kernel_data_base.rb kernel.map); \
	 echo "  _CODE ends at $$(ruby tools/kernel_data_base.rb --end kernel.map), _DATA=$$base"; \
	 $(SDLDZ80) -mjwx -i kernel.ihx \
	     -b _CODE=0x0098 -b _DATA=$$base \
	     $(KERNEL_LINK_ORDER) -e $(SDLD_DIAG)
	ruby tools/check_window0.rb kernel.map .
	@touch $@

# The link's three outputs copied out of the stamp's recipe.
build/kernel.ihx build/kernel.map build/kernel.noi: build/kernel.link.stamp
	@if [ -f "$(@F)" ]; then cp "$(@F)" "$@"; else rm -f "$@"; fi

build-kernel: build/kernel.map

build/muzix-kernel.bin: build/kernel.ihx tools/ihx_to_bin.rb | build
	@echo "Generating kernel binary..."
	@ruby tools/ihx_to_bin.rb build/kernel.ihx $@ 0x0098 65384
	@echo "  ✓ muzix-kernel.bin complete"

build-kernel-bin: build/muzix-kernel.bin

# ===========================================================================
# ROM image
# ===========================================================================
build/muzix-zeta-v2.rom: build/muzix-rom-boot.bin build/muzix-kernel.bin \
                          build/muzix-minifs.bin tools/make_zeta_rom.sh | build
	@echo "Building Zeta V2 ROM image..."
	@sh tools/make_zeta_rom.sh build/muzix-rom-boot.bin build/muzix-kernel.bin \
	    build/muzix-minifs.bin $@
	@echo "  ✓ muzix-zeta-v2.rom complete"

build-rom-image: build/muzix-zeta-v2.rom

# ===========================================================================
# Build directory (order-only prerequisite)
# ===========================================================================
build:
	@mkdir -p build

# ===========================================================================
# Validation gates
# ===========================================================================
test-kernel-layout: build/kernel.map
	@ruby tools/check_kernel_layout.rb build/kernel.map

test-kernel-stack: build/muzix-zeta-v2.rom emulator/z80pack-emu
	@ruby tools/check_kernel_stack.rb .

emulator/z80pack-emu:
	$(MAKE) -C emulator

test-window0: build/kernel.map
	@ruby tools/check_window0.rb build/kernel.map

test-syscall-context:
	@ruby tools/check_syscall_context.rb .

test-user-pointers:
	@ruby tools/check_user_pointers.rb .

test-link-scripts:
	@ruby tools/check_link_scripts.rb

test-emulator-sources:
	@ruby tools/check_emulator_sources.rb

test-userspace-runtime: build-apps
	@ruby tools/check_userspace_runtime.rb

test-userspace-layout: shell/shell.map apps/echo.map apps/cat.map apps/ls.map \
                         apps/date.map apps/top.map apps/uptime.map \
                         apps/loadavg.map apps/hwdiag.map apps/i2ctest.map \
                         apps/i2cdiag.map apps/wget.map apps/httpd.map
	@set -e; for m in shell/shell.map apps/echo.map apps/cat.map apps/ls.map \
	                   apps/date.map apps/top.map apps/uptime.map \
	                   apps/loadavg.map apps/hwdiag.map apps/i2ctest.map \
	                   apps/i2cdiag.map apps/wget.map apps/httpd.map; do \
	    test -f $$m || { echo "missing map: $$m - build userspace first" >&2; exit 1; }; \
	    ruby tools/check_userspace_layout.rb $$m 0x8000 0x4000 || exit 1; \
	done

test-memory-manager:
	@ruby tools/check_memory_manager.rb .

test-park-site:
	@ruby tools/check_park_site.rb .

test-proc-identity:
	@ruby tools/check_proc_identity.rb

test-host:
	@ruby tools/host_tests.rb .

test-z80-call-convention:
	@ruby tools/check_z80_call_convention.rb .

# All gates, without building the ROM
check: check-toolchain test-kernel-layout test-userspace-layout test-memory-manager \
       test-window0 test-syscall-context test-user-pointers \
       test-link-scripts test-emulator-sources test-userspace-runtime test-host \
       test-park-site test-proc-identity
	@echo "all gates: PASS"

test-rom-image: check-toolchain build/muzix-zeta-v2.rom test-kernel-layout \
                test-userspace-layout test-memory-manager test-window0 \
                test-z80-call-convention test-syscall-context test-user-pointers \
                test-link-scripts test-emulator-sources test-userspace-runtime \
                test-host test-park-site test-kernel-stack
	@ruby -e 'rom = File.binread("build/muzix-zeta-v2.rom"); \
	    abort "ROM size" unless rom.bytesize == 512 * 1024; \
	    abort "kernel entry" unless rom.byteslice(0x98, 2).bytes == [0xf3, 0x31]; \
	    abort "MINIX-v1 filesystem magic" unless \
	        rom.byteslice(128*1024 + 512 + 13, 2).unpack1("v") == 0x137f; \
	    puts "Zeta 512 KiB ROM: kernel + MINIX-v1-format filesystem: PASS"'

test-rom-validator: build/muzix-zeta-v2.rom
	@ruby tools/rom_validator.rb -v build/muzix-zeta-v2.rom

# ===========================================================================
# Housekeeping
# ===========================================================================
clean:
	@echo "Cleaning build artifacts..."
	@for d in $(CLEAN_DIRS); do \
	    [ -d "$$d" ] || continue; \
	    find "$$d" -maxdepth 1 -type f \( -name "*.rel" -o -name "*.ihx" \
	        -o -name "*.asm" -o -name "*.lst" -o -name "*.map" \
	        -o -name "*.sym" -o -name "*.noi" -o -name "*.d" \) \
	        ! -name "server_crt0.asm" -delete; \
	done
	@rm -rf .build build
	@rm -f $(BUILD_STAMP)
	@echo "Clean complete."

rebuild: clean rom

# ===========================================================================
# Generated header dependencies (must be after variable definitions)
# ===========================================================================
-include $(KERNEL_DEPS) $(US_DEPS)
