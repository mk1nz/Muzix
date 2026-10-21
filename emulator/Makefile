# Z80pack Emulator - Makefile
#
# Build system for the z80pack-based Z80 emulator
#
# Targets:
#   all       - Build the emulator (default)
#   clean     - Remove built files
#   run       - Build and run the Muzix ROM (../build/muzix-zeta-v2.rom)
#   run-muzix - Alias for run
#   run-fuzix - Build and run FUZIX (needs a fuzix.img you supply)
#   asan      - Build with AddressSanitizer for memory debugging
#
# Usage:
#   make              # Build emulator
#   make run          # Build and run the Muzix ROM
#   make run-fuzix    # Build and run with FUZIX (needs fuzix.img)
#   make clean        # Clean build artifacts
#   make asan         # Build with AddressSanitizer

# Compiler and flags
CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -g
CFLAGS  += -std=c99 -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=

# ASAN flags (can be enabled with make asan)
ASAN_CFLAGS = -fsanitize=address -fno-omit-frame-pointer -g
ASAN_LDFLAGS = -fsanitize=address

# Emulator name (changed to avoid conflict with z80pack directory)
EMULATOR = z80pack-emu
EMULATOR_ASAN = z80pack-emu-asan

# Source files
DS1302_SRCS = zeta_sbc_v2/emu_ds1302.c
Z80_SRCS    = z80/z80.c
Z80PACK_SRCS = z80pack/z80pack.c
MAIN_SRCS   = main.c
SRCS        = $(DS1302_SRCS) $(Z80_SRCS) $(Z80PACK_SRCS) $(MAIN_SRCS)

# Object files
DS1302_OBJS  = zeta_sbc_v2/emu_ds1302.o
Z80_OBJS     = z80/z80.o
Z80PACK_OBJS = z80pack/z80pack.o
MAIN_OBJS    = main.o
OBJS         = $(DS1302_OBJS) $(Z80_OBJS) $(Z80PACK_OBJS) $(MAIN_OBJS)

# Directories
Z80_DIR      = z80
Z80PACK_DIR  = z80pack
DS1302_DIR   = zeta_sbc_v2

# Header files.  zeta_sbc_v2/ is here because emu_ds1302.c includes
# zeta_sbc_v2/emulator.h and used to carry no header prerequisite at all:
# editing that header left emu_ds1302.o stale, and the host tests that link
# emu_ds1302.c would keep testing the old chip model.
HDRS        = $(wildcard $(Z80PACK_DIR)/*.h) $(wildcard $(Z80_DIR)/*.h) \
              $(wildcard $(DS1302_DIR)/*.h)

# Detect OS
OS := $(shell uname -s)
ifeq ($(OS),Darwin)
    CFLAGS += -DOS_DARWIN
    LDFLAGS += -lpthread
else
    LDFLAGS += -lpthread
endif

# Default target
.PHONY: all
all: $(EMULATOR)

# Build emulator
$(EMULATOR): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# Z80 core objects
$(DS1302_OBJS): $(DS1302_DIR)/%.o : $(DS1302_DIR)/%.c $(HDRS)
	@mkdir -p $(@D)
	@echo Compiling $<
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(Z80_OBJS): $(Z80_DIR)/%.o : $(Z80_DIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -MMD -MP -I$(Z80_DIR) -c -o $@ $<

# Z80pack objects
$(Z80PACK_OBJS): $(Z80PACK_DIR)/%.o : $(Z80PACK_DIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -MMD -MP -I$(Z80PACK_DIR) -I$(Z80_DIR) -c -o $@ $<

# Main objects
$(MAIN_OBJS): %.o : %.c $(HDRS)
	$(CC) $(CFLAGS) -MMD -MP -I$(Z80PACK_DIR) -I$(Z80_DIR) -c -o $@ $<

# Clean target - fixed to handle directory named 'z80pack'
.PHONY: clean
clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(EMULATOR) $(EMULATOR_ASAN) ./z80pack/z80pack
	rm -rf *.dSYM
	@echo "Cleaned build artifacts"

# Build with AddressSanitizer for memory debugging
.PHONY: asan
asan: CFLAGS += $(ASAN_CFLAGS)
asan: LDFLAGS += $(ASAN_LDFLAGS)
asan: $(EMULATOR_ASAN)

$(EMULATOR_ASAN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built with ASAN: $@"
	@echo "Run with: ./$@ [options]"

# Run with the Muzix ROM built by the parent project.  This target used to
# name ZETA2_std.rom, which does not exist in this tree; the real image is
# the one `make rom` produces in ../build/.
.PHONY: run
run: $(EMULATOR)
	@echo "Running $(EMULATOR) with ../build/muzix-zeta-v2.rom..."
	@if [ -f ../build/muzix-zeta-v2.rom ]; then \
		./$(EMULATOR) -s muzix ../build/muzix-zeta-v2.rom -r -i; \
	else \
		echo "../build/muzix-zeta-v2.rom not found - build it with 'make rom' in ../"; \
	fi

# Run with FUZIX (needs a FUZIX disk image at fuzix.img, not in this tree)
.PHONY: run-fuzix
run-fuzix: $(EMULATOR)
	@echo "Running $(EMULATOR) with FUZIX..."
	@if [ -f fuzix.img ]; then \
		./$(EMULATOR) -s fuzix -d fuzix.img -r; \
	else \
		echo "fuzix.img not found - supply a FUZIX disk image at emulator/fuzix.img"; \
	fi

# Run with Muzix (alias for run)
.PHONY: run-muzix
run-muzix: run

# Run with verbose output
.PHONY: debug
debug: $(EMULATOR)
	@echo "Running $(EMULATOR) with verbose output..."
	@if [ -f ../build/muzix-zeta-v2.rom ]; then \
		./$(EMULATOR) -s muzix ../build/muzix-zeta-v2.rom -v -c 100000; \
	else \
		echo "../build/muzix-zeta-v2.rom not found - build it with 'make rom' in ../"; \
	fi

# Rebuild
.PHONY: rebuild
rebuild: clean all

# Debug build
.PHONY: debug-build
debug-build: CFLAGS += -DDEBUG -O0
debug-build: LDFLAGS += -g
debug-build: clean $(EMULATOR)

# Show configuration
.PHONY: info
info:
	@echo "Emulator: $(EMULATOR)"
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "Source files: $(SRCS)"
	@echo "Object files: $(OBJS)"

# Help
.PHONY: help
help:
	@echo "Z80pack Emulator Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build the emulator (default)"
	@echo "  clean       - Remove built files"
	@echo "  run         - Build and run the Muzix ROM (../build/muzix-zeta-v2.rom)"
	@echo "  run-fuzix   - Build and run FUZIX (needs a fuzix.img you supply)"
	@echo "  run-muzix   - Alias for run"
	@echo "  debug       - Build and run the Muzix ROM with verbose output"
	@echo "  rebuild     - Clean and rebuild"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  CC=$(CC)"
	@echo "  CFLAGS=$(CFLAGS)"
	@echo "  LDFLAGS=$(LDFLAGS)"

# Dependencies
-include $(OBJS:.o=.d)

