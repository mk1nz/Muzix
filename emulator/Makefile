# Z80pack Emulator - Makefile
#
# Build system for the z80pack-based Z80 emulator
#
# Targets:
#   all       - Build the emulator (default)
#   clean     - Remove built files
#   run       - Build and run with ZETA2_std.rom
#   asan      - Build with AddressSanitizer for memory debugging
#
# Usage:
#   make              # Build emulator
#   make run          # Build and run with RomWBW
#   make run-fuzix    # Build and run with FUZIX
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

# Emulator name (changed to avoid conflict with z80pack directory)
EMULATOR = z80pack-emu

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

# Header files
HDRS        = $(wildcard $(Z80PACK_DIR)/*.h) $(wildcard $(Z80_DIR)/*.h)

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
$(DS1302_OBJS): zeta_sbc_v2/%.o : zeta_sbc_v2/%.c
	@mkdir -p $(@D)
	@echo Compiling $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(Z80_OBJS): $(Z80_DIR)/%.o : $(Z80_DIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -I$(Z80_DIR) -c -o $@ $<

# Z80pack objects
$(Z80PACK_OBJS): $(Z80PACK_DIR)/%.o : $(Z80PACK_DIR)/%.c $(HDRS)
	$(CC) $(CFLAGS) -I$(Z80PACK_DIR) -I$(Z80_DIR) -c -o $@ $<

# Main objects
$(MAIN_OBJS): %.o : %.c $(HDRS)
	$(CC) $(CFLAGS) -I$(Z80PACK_DIR) -I$(Z80_DIR) -c -o $@ $<

# Clean target - fixed to handle directory named 'z80pack'
.PHONY: clean
clean:
	rm -f $(OBJS) $(EMULATOR) $(EMULATOR_ASAN) ./z80pack/z80pack
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

# Run with RomWBW
.PHONY: run
run: $(EMULATOR)
	@echo "Running $(EMULATOR) with ZETA2_std.rom..."
	@if [ -f ZETA2_std.rom ]; then \
		./$(EMULATOR) ZETA2_std.rom -r -c 1000000; \
	else \
		echo "ZETA2_std.rom not found"; \
	fi

# Run with FUZIX
.PHONY: run-fuzix
run-fuzix: $(EMULATOR)
	@echo "Running $(EMULATOR) with FUZIX..."
	@if [ -f fuzix.img ]; then \
		./$(EMULATOR) -s fuzix -d fuzix.img -r; \
	else \
		echo "fuzix.img not found"; \
	fi

# Run with Muzix
.PHONY: run-muzix
run-muzix: $(EMULATOR)
	@echo "Running $(EMULATOR) with Muzix..."
	@if [ -f muzix.rom ]; then \
		./$(EMULATOR) -s muzix muzix.rom -r; \
	else \
		echo "muzix.rom not found"; \
	fi

# Run with verbose output
.PHONY: debug
debug: $(EMULATOR)
	@echo "Running $(EMULATOR) with verbose output..."
	./$(EMULATOR) ZETA2_std.rom -v -c 100000

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
	@echo "  run         - Build and run with RomWBW"
	@echo "  run-fuzix   - Build and run with FUZIX"
	@echo "  run-muzix   - Build and run with Muzix"
	@echo "  debug       - Build and run with verbose output"
	@echo "  rebuild     - Clean and rebuild"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  CC=$(CC)"
	@echo "  CFLAGS=$(CFLAGS)"
	@echo "  LDFLAGS=$(LDFLAGS)"

# Dependencies
-include $(OBJS:.o=.d)

