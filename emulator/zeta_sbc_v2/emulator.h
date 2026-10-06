/**
 * Zeta SBC V2 Emulator - Public API
 * 
 * This emulator provides a Z80 CPU emulation environment for the Zeta SBC V2
 * hardware platform. It includes memory management, bank switching, and 
 * serial I/O capabilities.
 * 
 * License: MIT
 */

#ifndef ZETA_SBC_V2_EMULATOR_H
#define ZETA_SBC_V2_EMULATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

/* Z80 type from superzazu/z80 - include from parent directory */
#include "../z80/z80.h"

/* Define z80_t as alias for struct z80 */
typedef struct z80 z80_t;

/* ============================================================================
 * ZETA2 Hardware Port Definitions
 * ============================================================================ */

/* PPI Ports (8255) */
#define ZETA_PORT_PPI_A        0x60
#define ZETA_PORT_PPI_B        0x61
#define ZETA_PORT_PPI_C        0x62
#define ZETA_PORT_PPI_CTRL     0x63

/* FDC Ports (ZETA2 mode - 82077A) */
#define ZETA_PORT_FDC_MSR      0x30    /* Main Status Register */
#define ZETA_PORT_FDC_DATA     0x31    /* Data Register */
#define ZETA_PORT_FDC_CCR      0x28    /* Configuration Control Register */
#define ZETA_PORT_FDC_DOR      0x38    /* Digital Output Register */
#define ZETA_PORT_FDC_TC       0x38    /* Terminal Count (read) */

/* CTC Ports */
#define ZETA_PORT_CTC_0        0x20
#define ZETA_PORT_CTC_1        0x21
#define ZETA_PORT_CTC_2        0x22
#define ZETA_PORT_CTC_3        0x23

/* Memory Paging */
#define ZETA_PORT_BANK_0       0x78
#define ZETA_PORT_BANK_1       0x79
#define ZETA_PORT_BANK_2       0x7A
#define ZETA_PORT_BANK_3       0x7B
#define ZETA_PORT_MPGENA       0x7C    /* Paging enable */

/* Timer Ports (legacy) */
#define ZETA_PORT_TIMER_CTRL   0x84
#define ZETA_PORT_TIMER_DATA   0x85
#define ZETA_PORT_TIMER_PRESC  0x86
#define ZETA_PORT_TIMER_INT    0x87

/* FUZIX UART (16550-compatible) */
#define ZETA_PORT_UART_BASE    0x68
#define ZETA_PORT_UART_RHR     0x68    /* Receive Holding Register */
#define ZETA_PORT_UART_THR     0x68    /* Transmit Holding Register */
#define ZETA_PORT_UART_IER     0x69    /* Interrupt Enable */
#define ZETA_PORT_UART_IIR     0x6A    /* Interrupt ID / FIFO Control */
#define ZETA_PORT_UART_LCR     0x6B    /* Line Control */
#define ZETA_PORT_UART_MCR     0x6C    /* Modem Control */
#define ZETA_PORT_UART_LSR     0x6D    /* Line Status */
#define ZETA_PORT_UART_MSR     0x6E    /* Modem Status */
#define ZETA_PORT_UART_SPR     0x6F    /* Scratch Pad */

/* RomWBW SIO (Z80 SIO) */
#define ZETA_PORT_SIOA_DATA    0xB4
#define ZETA_PORT_SIOA_CTRL    0xB6
#define ZETA_PORT_SIOB_DATA    0xB5
#define ZETA_PORT_SIOB_CTRL    0xB7

/* RomWBW UART */
#define ZETA_PORT_UART_RWB     0x70    /* RomWBW UART */

/* RTC (DS1302) */
#define ZETA_PORT_RTC          0x70

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

/* Total RAM size: 512KB with banked memory */
#define ZETA_RAM_SIZE           (512 * 1024)  /* 512KB total RAM */
#define ZETA_BANK_SIZE          (16 * 1024)   /* 16KB per bank */
#define ZETA_NUM_BANKS          64             /* Number of 16KB banks (supports 1MB ROM) */
#define ZETA_VISIBLE_RAM        (64 * 1024)   /* 64KB visible address space */

/* ROM address space - 512KB for MUZIX image */
#define ZETA_ROM_SIZE           (512 * 1024)  /* 512KB ROM (MUZIX image) */
#define ZETA_ROM_START          0x0000        /* ROM starts at 0x0000 */

/* ============================================================================
 * I/O Port Configuration
 * ============================================================================ */

/* Bank control ports ($78-$7C) - 5 ports for 4 regions of 16KB each */
#define ZETA_PORT_BANK_BASE     0x78
#define ZETA_PORT_BANK_0        0x78  /* Bank for $0000-$3FFF */
#define ZETA_PORT_BANK_1        0x79  /* Bank for $4000-$7FFF */
#define ZETA_PORT_BANK_2        0x7A  /* Bank for $8000-$BFFF */
#define ZETA_PORT_BANK_3        0x7B  /* Bank for $C000-$FFFF */
#define ZETA_PORT_BANK_4        0x7C  /* MPGENA - paging control/bank for special regions */

/* UART ports (standard Z80 SIO) */
#define ZETA_PORT_SIO_A_DATA    0x80
#define ZETA_PORT_SIO_A_CTRL    0x81
#define ZETA_PORT_SIO_B_DATA    0x82
#define ZETA_PORT_SIO_B_CTRL    0x83

/* Timer ports */
#define ZETA_PORT_TIMER_CTRL    0x84
#define ZETA_PORT_TIMER_DATA    0x85

/* ============================================================================
 * Serial Configuration
 * ============================================================================ */

/* Default serial settings */
#define ZETA_DEFAULT_BAUD       38400
#define ZETA_DEFAULT_PORT       "/dev/ttyUSB0"

/* ============================================================================
 * HBIOS Function Numbers
 * ============================================================================ */

/* HBIOS RST 0x08 function calls (from RomWBW HBIOS) */
/* Function number is passed in register A, not C */
#define HBIOS_CONIN             0x00  /* Console input (blocking) - returns char in A */
#define HBIOS_CONOUT            0x01  /* Console output - character in E */
#define HBIOS_CONST             0x02  /* Console input status - returns char count in A */
#define HBIOS_DIRIOF            0x04  /* Direct console I/O (raw) */
#define HBIOS_DIRIST            0x05  /* Direct console input status */
#define HBIOS_OUTSUB            0x07  /* Output sub (special character) */
#define HBIOS_INSUB             0x08  /* Input sub (special character) */
#define HBIOS_CONST2            0x0B  /* Console input status (alternate) */
#define HBIOS_CONIN2            0x0C  /* Console input (alternate) */
#define HBIOS_CONOUT2           0x0D  /* Console output (alternate) */
#define HBIOS_STROUT            0x09  /* Output string from memory (DE=ptr) */
#define HBIOS_SETBNK            0x18  /* Set bank for DMA operations */
#define HBIOS_GETBNK            0x19  /* Get current bank */
#define HBIOS_VERSION           0x21  /* Get HBIOS version string */

/* ============================================================================
 * Timing Configuration
 * ============================================================================ */

/* Clock frequency for this emulator profile; the physical SBC's U17 is 4 MHz. */
#define ZETA_CPU_CLOCK          7372800UL

/* Timer tick: 50 Hz (standard for FUZIX/ROMWBW) */
#define ZETA_TICK_FREQ          50
#define ZETA_CYCLES_PER_TICK    (ZETA_CPU_CLOCK / ZETA_TICK_FREQ)

/* ============================================================================
 * Boot Timeout Configuration
 * ============================================================================ */

/* Default boot timeout in cycles (at 7.37MHz CPU clock)
 * 50M cycles = ~6.8 seconds - sufficient for RomWBW boot
 * After this many cycles, hardware-dependent port reads return timeout values
 * to allow boot to progress without actual hardware */
#define ZETA_BOOT_TIMEOUT_DEFAULT   (50000000UL)  /* ~6.8 seconds */
#define ZETA_BOOT_TIMEOUT_INFINITE  (UINT64_MAX) /* No timeout */

/* ============================================================================
 * Callback Types for Serial Bridge
 * ============================================================================ */

/**
 * Serial send byte callback
 * @param byte The byte to send
 * @param userdata User-defined data pointer
 */
typedef void (*zeta_serial_send_t)(uint8_t byte, void* userdata);

/**
 * Serial receive byte callback (blocking)
 * @param userdata User-defined data pointer
 * @return The received byte
 */
typedef uint8_t (*zeta_serial_recv_t)(void* userdata);

/**
 * Serial data available callback
 * @param userdata User-defined data pointer
 * @return true if data is available to read
 */
typedef bool (*zeta_serial_available_t)(void* userdata);

/**
 * Timer tick callback
 * @param userdata User-defined data pointer
 */
typedef void (*zeta_tick_callback_t)(void* userdata);

/**
 * HBIOS call callback
 * @param func The HBIOS function number
 * @param regs Pointer to register structure
 * @param userdata User-defined data pointer
 * @return Return value from HBIOS (in HL)
 */
typedef uint16_t (*zeta_hbios_call_t)(uint8_t func, void* regs, void* userdata);

/**
 * BDOS call callback
 * @param func The BDOS function number
 * @param regs Pointer to register structure
 * @param userdata User-defined data pointer
 * @return Return value from BDOS (in HL)
 */
typedef uint16_t (*zeta_bdos_call_t)(uint8_t func, void* regs, void* userdata);

/* ============================================================================
 * Emulator Context Structure
 * ============================================================================ */

/* Forward declaration */
struct zeta_sbc_v2_t;

/**
 * Zeta SBC V2 Emulator context
 * Contains all state for the emulated system
 */
typedef struct zeta_sbc_v2_t {
    /* Z80 CPU core */
    z80_t cpu;
    
    /* Memory */
    uint8_t* ram;              /* Full 512KB RAM allocation */
    uint8_t* rom;              /* ROM image (512KB for MUZIX) */
    uint8_t visible_ram[ZETA_VISIBLE_RAM];  /* Currently visible 64KB */
    uint16_t bank_reg[5];      /* Bank control registers $78-$7C (indices 0-4) */
    
    /* Memory mapping */
    size_t rom_size;          /* Size of loaded ROM */
    uint8_t rom_bank;         /* Current ROM bank (for 512KB ROM) */
    bool rom_writable;        /* Allow writing to ROM area (for development) */
    bool paging_enabled;      /* MPGENA paging enabled state */
    
    /* Serial I/O callbacks */
    zeta_serial_send_t send_byte;
    zeta_serial_recv_t recv_byte;
    zeta_serial_available_t data_available;
    void* serial_userdata;
    
    /* HBIOS callbacks */
    zeta_hbios_call_t hbios_call;
    void* hbios_userdata;
    
    /* BDOS callbacks */
    zeta_bdos_call_t bdos_call;
    void* bdos_userdata;
    
    /* Timing */
    uint64_t total_cycles;     /* Total cycles executed */
    uint64_t next_tick_cycle;  /* Cycle count for next timer tick */
    zeta_tick_callback_t tick_callback;
    void* tick_userdata;
    
    /* Callbacks */
    void (*debug_output)(struct zeta_sbc_v2_t*);
    void* debug_userdata;
    
    /* State flags */
    bool halted;
    bool running;          /* Execution running flag (controlled by signal handler) */
    bool debug_mode;
    bool trace_enabled;
    bool verbose_mode;
    
    /* Statistics */
    uint32_t instructions_executed;
    uint32_t memory_reads;
    uint32_t memory_writes;
    uint32_t port_reads;
    uint32_t port_writes;
    
    /* Boot timeout for skipping hardware-dependent operations */
    uint64_t boot_timeout_cycles;    /* Cycle count at which to enable timeout mode */
    bool boot_timeout_enabled;       /* Whether boot timeout is enabled */
    bool boot_timeout_mode;          /* True if currently in timeout mode (no hardware) */
    
} zeta_sbc_v2_t;

/**
 * Register structure for HBIOS calls
 * Used when passing register state to HBIOS callbacks
 */
typedef struct {
    uint16_t af;    /* Accumulator + Flags */
    uint16_t bc;    /* B + C */
    uint16_t de;    /* D + E */
    uint16_t hl;    /* H + L */
    uint16_t ix;    /* Index X */
    uint16_t iy;    /* Index Y */
    uint16_t sp;    /* Stack Pointer */
    uint16_t pc;    /* Program Counter */
} zeta_regs_t;

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================ */

/**
 * Create a new Zeta SBC V2 emulator instance
 * @return Pointer to emulator context, or NULL on error
 */
zeta_sbc_v2_t* zeta_create(void);

/**
 * Destroy a Zeta SBC V2 emulator instance
 * @param zeta Pointer to emulator context
 */
void zeta_destroy(zeta_sbc_v2_t* zeta);

/**
 * Reset the emulator to initial state
 * @param zeta Pointer to emulator context
 */
void zeta_reset(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

/**
 * Load ROM image into emulator
 * @param zeta Pointer to emulator context
 * @param data Pointer to ROM image data
 * @param size Size of ROM image in bytes
 * @param addr Load address (typically 0x0000)
 * @return 0 on success, non-zero on error
 */
int zeta_load_rom(zeta_sbc_v2_t* zeta, const uint8_t* data, size_t size, uint16_t addr);

/**
 * Load RAM from file
 * @param zeta Pointer to emulator context
 * @param filename Path to file
 * @param addr Load address
 * @return 0 on success, non-zero on error
 */
int zeta_load_ram(zeta_sbc_v2_t* zeta, const char* filename, uint16_t addr);

/**
 * Save RAM to file
 * @param zeta Pointer to emulator context
 * @param filename Path to file
 * @param addr Start address
 * @param size Number of bytes to save
 * @return 0 on success, non-zero on error
 */
int zeta_save_ram(zeta_sbc_v2_t* zeta, const char* filename, uint16_t addr, size_t size);

/**
 * Set ROM writable (for development/testing)
 * @param zeta Pointer to emulator context
 * @param writable true to allow writes to ROM area
 */
void zeta_set_rom_writable(zeta_sbc_v2_t* zeta, bool writable);

/* ============================================================================
 * Serial I/O Configuration
 * ============================================================================ */

/**
 * Configure serial I/O callbacks
 * @param zeta Pointer to emulator context
 * @param send Callback for sending bytes
 * @param recv Callback for receiving bytes (blocking)
 * @param available Callback to check if data is available
 * @param userdata User data passed to callbacks
 */
void zeta_configure_serial(zeta_sbc_v2_t* zeta,
    zeta_serial_send_t send,
    zeta_serial_recv_t recv,
    zeta_serial_available_t available,
    void* userdata);

/**
 * Configure HBIOS callback
 * @param zeta Pointer to emulator context
 * @param hbios_call Callback for HBIOS function calls
 * @param userdata User data passed to callback
 */
void zeta_configure_hbios(zeta_sbc_v2_t* zeta,
    zeta_hbios_call_t hbios_call,
    void* userdata);

/**
 * Configure timer tick callback
 * @param zeta Pointer to emulator context
 * @param callback Callback function
 * @param userdata User data passed to callback
 */
void zeta_set_tick_callback(zeta_sbc_v2_t* zeta,
    zeta_tick_callback_t callback,
    void* userdata);

/* ============================================================================
 * Execution Control
 * ============================================================================ */

/**
 * Execute a single instruction
 * @param zeta Pointer to emulator context
 */
void zeta_step(zeta_sbc_v2_t* zeta);

/**
 * Execute instructions for a maximum number of cycles
 * @param zeta Pointer to emulator context
 * @param max_cycles Maximum number of cycles to execute
 */
void zeta_run(zeta_sbc_v2_t* zeta, uint64_t max_cycles);

/**
 * Execute until HALT instruction or max cycles reached
 * @param zeta Pointer to emulator context
 * @param max_cycles Maximum number of cycles (0 = unlimited)
 */
void zeta_run_until_halt(zeta_sbc_v2_t* zeta, uint64_t max_cycles);

/**
 * Execute until a specific address is reached
 * @param zeta Pointer to emulator context
 * @param target_addr Target address
 * @param max_cycles Maximum number of cycles
 * @return true if target was reached
 */
bool zeta_run_until_addr(zeta_sbc_v2_t* zeta, uint16_t target_addr, uint64_t max_cycles);

/* ============================================================================
 * Debug and Trace
 * ============================================================================ */

/**
 * Enable or disable debug mode
 * @param zeta Pointer to emulator context
 * @param enable true to enable debug mode
 */
void zeta_set_debug(zeta_sbc_v2_t* zeta, bool enable);

/**
 * Enable or disable instruction tracing
 * @param zeta Pointer to emulator context
 * @param enable true to enable tracing
 */
void zeta_set_trace(zeta_sbc_v2_t* zeta, bool enable);

/**
 * Set debug count limit
 * @param limit Maximum number of debug messages to print (0 = unlimited)
 */
void zeta_set_debug_count_limit(int limit);

/**
 * Get current debug count limit
 * @return Current limit
 */
int zeta_get_debug_count_limit(void);

/**
 * Print debug information about current state
 * @param zeta Pointer to emulator context
 */
void zeta_debug_output(zeta_sbc_v2_t* zeta);

/**
 * Print instruction disassembly at current PC
 * @param zeta Pointer to emulator context
 */
void zeta_disassemble_current(zeta_sbc_v2_t* zeta);

/**
 * Set debug output callback
 * @param zeta Pointer to emulator context
 * @param callback Debug output callback function
 * @param userdata User data for callback
 */
void zeta_set_debug_callback(zeta_sbc_v2_t* zeta,
    void (*callback)(zeta_sbc_v2_t*),
    void* userdata);

/* ============================================================================
 * Register and Memory Access
 * ============================================================================ */

/**
 * Get current program counter
 * @param zeta Pointer to emulator context
 * @return Current PC value
 */
uint16_t zeta_get_pc(zeta_sbc_v2_t* zeta);

/**
 * Get current stack pointer
 * @param zeta Pointer to emulator context
 * @return Current SP value
 */
uint16_t zeta_get_sp(zeta_sbc_v2_t* zeta);

/**
 * Get total cycle count
 * @param zeta Pointer to emulator context
 * @return Total cycles executed
 */
uint64_t zeta_get_cycles(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * Execution Control - Running Flag
 * ============================================================================ */

/**
 * Set the running flag for execution control
 * Used by signal handlers to stop execution
 * @param zeta Pointer to emulator context
 * @param running true to continue running, false to stop
 */
void zeta_set_running(zeta_sbc_v2_t* zeta, bool running);

/**
 * Get the current running flag state
 * @param zeta Pointer to emulator context
 * @return true if execution should continue
 */
bool zeta_is_running(zeta_sbc_v2_t* zeta);

/**
 * Check if execution should stop (for use in execution loops)
 * @param zeta Pointer to emulator context
 * @return true if execution should stop
 */
bool zeta_should_stop(zeta_sbc_v2_t* zeta);

/**
 * Get instructions executed count
 * @param zeta Pointer to emulator context
 * @return Instructions executed
 */
uint32_t zeta_get_instructions(zeta_sbc_v2_t* zeta);

/**
 * Read a byte from emulated memory
 * @param zeta Pointer to emulator context
 * @param addr Memory address
 * @return Byte value at address
 */
uint8_t zeta_read_byte(zeta_sbc_v2_t* zeta, uint16_t addr);

/**
 * Write a byte to emulated memory
 * @param zeta Pointer to emulator context
 * @param addr Memory address
 * @param val Byte value to write
 */
void zeta_write_byte(zeta_sbc_v2_t* zeta, uint16_t addr, uint8_t val);

/**
 * Read a word (16-bit) from emulated memory
 * @param zeta Pointer to emulator context
 * @param addr Memory address
 * @return Word value at address (little-endian)
 */
uint16_t zeta_read_word(zeta_sbc_v2_t* zeta, uint16_t addr);

/**
 * Write a word (16-bit) to emulated memory
 * @param zeta Pointer to emulator context
 * @param addr Memory address
 * @param val Word value to write (little-endian)
 */
void zeta_write_word(zeta_sbc_v2_t* zeta, uint16_t addr, uint16_t val);

/**
 * Get register state
 * @param zeta Pointer to emulator context
 * @param regs Pointer to register structure to fill
 */
void zeta_get_registers(zeta_sbc_v2_t* zeta, zeta_regs_t* regs);

/**
 * Set register state
 * @param zeta Pointer to emulator context
 * @param regs Pointer to register structure with values
 */
void zeta_set_registers(zeta_sbc_v2_t* zeta, const zeta_regs_t* regs);

/**
 * Get current bank number for a region
 * @param zeta Pointer to emulator context
 * @param region 0-4 for bank region ($78-$7C)
 * @return Current bank number
 */
uint8_t zeta_get_bank(zeta_sbc_v2_t* zeta, int region);

/**
 * Set bank number for a region
 * @param zeta Pointer to emulator context
 * @param region 0-4 for bank region ($78-$7C)
 * @param bank Bank number to select
 */
void zeta_set_bank(zeta_sbc_v2_t* zeta, int region, uint8_t bank);

/* ============================================================================
 * Statistics and Information
 * ============================================================================ */

/**
 * Get emulator version string
 * @return Version string
 */
const char* zeta_get_version(void);

/**
 * Get emulator statistics
 * @param zeta Pointer to emulator context
 * @param instructions Pointer to store instruction count
 * @param memory_reads Pointer to store memory read count
 * @param memory_writes Pointer to store memory write count
 * @param port_reads Pointer to store port read count
 * @param port_writes Pointer to store port write count
 */
void zeta_get_stats(zeta_sbc_v2_t* zeta,
    uint32_t* instructions,
    uint32_t* memory_reads,
    uint32_t* memory_writes,
    uint32_t* port_reads,
    uint32_t* port_writes);

/**
 * Print emulator statistics
 * @param zeta Pointer to emulator context
 */
void zeta_print_stats(zeta_sbc_v2_t* zeta);

/**
 * Dump memory to console (for debugging)
 * @param zeta Pointer to emulator context
 * @param start Start address
 * @param size Number of bytes to dump
 */
void zeta_dump_memory(zeta_sbc_v2_t* zeta, uint16_t start, size_t size);

/**
 * Dump registers to console (for debugging)
 * @param zeta Pointer to emulator context
 */
void zeta_dump_registers(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * Breakpoint Management
 * ============================================================================ */

/**
 * Breakpoint structure
 */
typedef struct {
    uint16_t address;      /* Breakpoint address */
    bool enabled;          /* Breakpoint enabled */
    bool one_shot;         /* One-shot breakpoint (disable after hit) */
    const char* condition; /* Optional condition string */
} zeta_breakpoint_t;

/**
 * Set a breakpoint at address
 * @param zeta Pointer to emulator context
 * @param addr Address to break on
 * @return Breakpoint index, or -1 if no slots available
 */
int zeta_set_breakpoint(zeta_sbc_v2_t* zeta, uint16_t addr);

/**
 * Clear a breakpoint
 * @param zeta Pointer to emulator context
 * @param index Breakpoint index
 */
void zeta_clear_breakpoint(zeta_sbc_v2_t* zeta, int index);

/**
 * Clear all breakpoints
 * @param zeta Pointer to emulator context
 */
void zeta_clear_all_breakpoints(zeta_sbc_v2_t* zeta);

/**
 * Check if current PC is at a breakpoint
 * @param zeta Pointer to emulator context
 * @return true if at breakpoint
 */
bool zeta_check_breakpoint(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * Debug Output Control
 * ============================================================================ */

/**
 * Debug output macro - prints debug messages only when verbose mode is enabled
 * @param zeta Pointer to emulator context
 * @param format Printf-style format string (must be followed by arguments)
 *
 * Note: Uses local variable to avoid multiple evaluations of zeta parameter
 * which can cause segfaults if zeta is evaluated after a failed allocation.
 * Output uses flockfile to prevent interleaving with serial output.
 *
 * This macro uses fprintf internally which is already buffered, but the
 * flockfile/funlockfile calls ensure atomicity with other stdout operations.
 */
#define ZETA_DEBUG(zeta, ...) \
    do { \
        zeta_sbc_v2_t* _zeta_debug = (zeta); \
        if (_zeta_debug && _zeta_debug->verbose_mode) { \
            flockfile(stdout); \
            fprintf(stderr, __VA_ARGS__); \
            funlockfile(stdout); \
        } \
    } while (0)

/**
 * Set verbose mode for debug output
 * @param zeta Pointer to emulator context
 * @param enable true to enable verbose debug output
 */
void zeta_set_verbose(zeta_sbc_v2_t* zeta, bool enable);

/* ============================================================================
 * Console Input Buffer (for HBIOS)
 * ============================================================================ */

/**
 * Add a string to the console input buffer
 * Useful for providing input during testing
 * @param str String to add
 */
void zeta_console_input_string(const char* str);

/**
 * Clear console input buffer
 */
void zeta_console_clear(void);

/* ============================================================================
 * CTC Timer Support (for RomWBW)
 * ============================================================================ */

/* CTC port base for ZETA2 */
#define ZETA_PORT_CTC_BASE      0x20

/* CTC modes */
#define CTCMODE_CTR             0  /* Counter mode */
#define CTCMODE_TIM16           1  /* Timer 16-bit */
#define CTCMODE_TIM256          2  /* Timer with 256 prescale */

/**
 * Initialize CTC
 */
void ctc_init(void);

/**
 * Reset CTC to initial state
 */
void ctc_reset(void);

/**
 * Read from CTC port
 * @param zeta Pointer to emulator context
 * @param port CTC port offset
 * @return Value read
 */
uint8_t ctc_read(zeta_sbc_v2_t* zeta, uint8_t port);

/**
 * Write to CTC port
 * @param zeta Pointer to emulator context
 * @param port CTC port offset
 * @param val Value to write
 */
void ctc_write(zeta_sbc_v2_t* zeta, uint8_t port, uint8_t val);

/**
 * Process CTC tick
 * @param zeta Pointer to emulator context
 * @param cycles Number of cycles since last tick
 */
void ctc_tick(zeta_sbc_v2_t* zeta, uint16_t cycles);

/**
 * Check if CTC has pending interrupt
 * @param zeta Pointer to emulator context
 * @return true if interrupt pending
 */
bool ctc_has_interrupt(zeta_sbc_v2_t* zeta);

/**
 * Acknowledge CTC interrupt
 * @param zeta Pointer to emulator context
 * @return Interrupt vector
 */
uint8_t ctc_ack_interrupt(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * Timer Control
 * ============================================================================ */

/* External timer armed flag - prevents spurious interrupts during boot */
extern bool timer_armed;

/**
 * Reset timer state to initial values
 */
void zeta_timer_reset(void);

/**
 * Arm the timer to enable interrupt generation
 * This must be called explicitly after timer configuration
 * to prevent spurious interrupts during boot
 * @param zeta Pointer to emulator context (for debug output)
 */
void zeta_timer_arm(zeta_sbc_v2_t* zeta);

/**
 * Auto-arm the timer for boot compatibility
 * Called automatically when boot timeout activates to allow HALT to wake
 * without requiring explicit ROM timer configuration
 * @param zeta Pointer to emulator context (for debug output)
 */
void zeta_timer_auto_arm(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * FDC Support (82077A/ZETA2 mode)
 * ============================================================================ */

/**
 * Initialize FDC
 */
void fdc_init(void);

/**
 * Reset FDC to initial state
 */
void fdc_reset(void);

/**
 * Read from FDC port
 * @param zeta Pointer to emulator context
 * @param port FDC port number
 * @return Value read from FDC
 */
uint8_t fdc_read(zeta_sbc_v2_t* zeta, uint8_t port);

/**
 * Write to FDC port
 * @param zeta Pointer to emulator context
 * @param port FDC port number
 * @param val Value to write
 */
void fdc_write(zeta_sbc_v2_t* zeta, uint8_t port, uint8_t val);

/**
 * Check if FDC has pending interrupt
 * @return true if interrupt pending
 */
bool fdc_has_interrupt(void);

/**
 * Acknowledge FDC interrupt
 * @param zeta Pointer to emulator context
 * @return ST0 status byte
 */
uint8_t fdc_ack_interrupt(zeta_sbc_v2_t* zeta);

/**
 * Get current FDC drive
 * @return Drive number (0-3)
 */
uint8_t fdc_get_drive(void);

/**
 * Get current FDC track
 * @return Current track number
 */
uint8_t fdc_get_track(void);

/* ============================================================================
 * PPI Support (8255)
 * ============================================================================ */

/**
 * Initialize PPI
 */
void ppi_init(void);

/**
 * Reset PPI to initial state
 */
void ppi_reset(void);

/**
 * Read from PPI port
 * @param zeta Pointer to emulator context
 * @param port PPI port number ($60-$63)
 * @return Value read from PPI
 */
uint8_t ppi_read(zeta_sbc_v2_t* zeta, uint8_t port);

/**
 * Write to PPI port
 * @param zeta Pointer to emulator context
 * @param port PPI port number ($60-$63)
 * @param val Value to write
 */
void ppi_write(zeta_sbc_v2_t* zeta, uint8_t port, uint8_t val);

/**
 * Get current port A value
 * @return Port A data
 */
uint8_t ppi_get_porta(void);

/**
 * Get current port B value
 * @return Port B data
 */
uint8_t ppi_get_portb(void);

/**
 * Get current port C value
 * @return Port C data
 */
uint8_t ppi_get_portc(void);

/**
 * Get current PPI mode
 * @return Mode (0, 1, or 2)
 */
uint8_t ppi_get_mode(void);

/**
 * Check if PC3 (CTC interrupt) is set
 * @return true if PC3 is high
 */
bool ppi_get_pc3_ctc_int(void);

/* ============================================================================
 * DS1302 RTC Support (3-wire interface)
 * ============================================================================ */

/**
 * Initialize DS1302 RTC
 */
void ds1302_init(void);
void ds1302_advance_one_second(void);

/**
 * Reset DS1302 to initial state
 */
void ds1302_reset(void);

/**
 * Read from RTC port ($70)
 * @param zeta Pointer to emulator context
 * @param port Port number (should be $70)
 * @return Value read from RTC
 */
uint8_t ds1302_read(zeta_sbc_v2_t* zeta, uint8_t port);

/**
 * Write to RTC port ($70)
 * @param zeta Pointer to emulator context
 * @param port Port number (should be $70)
 * @param val Value to write
 */
void ds1302_write(zeta_sbc_v2_t* zeta, uint8_t port, uint8_t val);

/**
 * Check if clock is running (not halted)
 * @return true if clock is running
 */
bool ds1302_is_running(void);

/**
 * Get current time as BCD buffer
 * @param buf Buffer to store time (7 bytes: YY, MM, DD, HH, MM, SS, DOW)
 */
void ds1302_get_time_bcd(uint8_t* buf);

/**
 * Set time from BCD buffer
 * @param buf Buffer with time (7 bytes: YY, MM, DD, HH, MM, SS, DOW)
 *
 * buf[2] is a plain BCD hour and the 24/12 mode bit is set for you.
 */
void ds1302_set_time_bcd(uint8_t* buf);

/**
 * Set time from a BCD buffer holding the hour register verbatim, mode bits and
 * all.  This is what a driver writing the hour register itself amounts to.
 */
void ds1302_set_time_bcd_raw(uint8_t* buf);

/**
 * Arm or clear the write-protect bit, bit 7 of the write-protect register.
 *
 * A seam for host tests, and it is a seam because nothing else can reach this
 * state: ds1302_init() and ds1302_reset() both leave the chip writable, and
 * every entry point above writes seven time registers and no protection.  A
 * board that arrives protected is a state the model could describe but never
 * enter, which is the same gap as not enforcing the bit at all - the write that
 * silicon drops would be one the model accepts.
 *
 * Takes the register byte, so a caller can set the low bits as well; only bit 7
 * is the protection and write_clock_reg() keeps the rest as FUZIX does.
 */
void ds1302_set_write_protect(uint8_t reg);

/**
 * Get time as binary values
 */
void ds1302_get_time_bin(uint8_t* sec, uint8_t* min, uint8_t* hour,
                         uint8_t* day, uint8_t* mon, uint8_t* year);

/**
 * Set time from binary values
 */
void ds1302_set_time_bin(uint8_t sec, uint8_t min, uint8_t hour,
                         uint8_t day, uint8_t mon, uint8_t year);

/**
 * Check if trickle charge is enabled
 * @return true if trickle charging is enabled
 */
bool ds1302_trickle_enabled(void);

/* ============================================================================
 * NS16550 UART Support (FUZIX mode)
 * ============================================================================ */

/**
 * Initialize UART
 */
void uart_init(void);

/**
 * Reset UART to initial state
 */
void uart_reset(void);

/**
 * Set serial callbacks for the UART
 */
void uart_set_serial_callbacks(zeta_serial_send_t send, 
                               zeta_serial_recv_t recv,
                               zeta_serial_available_t available,
                               void* userdata);

/**
 * Read from UART register
 * @param zeta Pointer to emulator context
 * @param offset Register offset from base (0x68)
 * @return Value read from UART
 */
uint8_t uart_read(zeta_sbc_v2_t* zeta, uint8_t offset);

/**
 * Write to UART register
 * @param zeta Pointer to emulator context
 * @param offset Register offset from base (0x68)
 * @param val Value to write
 */
void uart_write(zeta_sbc_v2_t* zeta, uint8_t offset, uint8_t val);

/**
 * Get current baud rate
 * @return Baud rate
 */
uint32_t uart_get_baud_rate(void);

/**
 * Set baud rate
 * @param baud New baud rate
 */
void uart_set_baud_rate(uint32_t baud);

/**
 * Check if UART has interrupt pending
 * @return true if interrupt pending
 */
bool uart_has_interrupt(void);

/**
 * Get and clear interrupt status
 * @return Interrupt type
 */
uint8_t uart_get_interrupt_status(void);

/**
 * Get line status register
 * @return LSR value
 */
uint8_t uart_get_lsr(void);

/**
 * Get character from UART (non-blocking)
 * @param ch Pointer to store character
 * @return true if character was read
 */
bool uart_get_char(uint8_t* ch);

/**
 * Put character to UART (non-blocking)
 * @param ch Character to send
 */
void uart_put_char(uint8_t ch);

/**
 * Check if UART transmitter is ready
 * @return true if ready to transmit
 */
bool uart_tx_ready(void);

/* ============================================================================
 * PPP Support (Parallel Port Propeller)
 * ============================================================================ */

/* PPP port addresses */
#define ZETA_PORT_PPP_DATA      0x00    /* Data I/O (connected to PPI Port A) */
#define ZETA_PORT_PPP_CTL       0x02    /* Control lines (connected to PPI Port C) */
#define ZETA_PORT_PPP_PPICTL    0x03    /* PPI Control register */

/**
 * Initialize PPP
 */
void ppp_init(void);

/**
 * Reset PPP to initial state
 */
void ppp_reset(void);

/**
 * Read from PPP port
 * @param zeta Pointer to emulator context
 * @param port PPP port number ($00, $02, $03)
 * @return Value read from PPP
 */
uint8_t ppp_read(zeta_sbc_v2_t* zeta, uint8_t port);

/**
 * Write to PPP port
 * @param zeta Pointer to emulator context
 * @param port PPP port number ($00, $02, $03)
 * @param val Value to write
 */
void ppp_write(zeta_sbc_v2_t* zeta, uint8_t port, uint8_t val);

/**
 * Check if PPP is present
 * @return true if Propeller was detected
 */
bool ppp_is_present(void);

/**
 * Set PPP present flag
 * @param present true if Propeller hardware detected
 */
void ppp_set_present(bool present);

/* ============================================================================
 * Boot Timeout Support (for skipping hardware-dependent operations)
 * ============================================================================ */

/**
 * Enable boot timeout
 * After the specified number of cycles, port reads will return timeout values
 * to allow boot to progress without actual hardware present
 * @param zeta Pointer to emulator context
 * @param timeout_cycles Number of cycles before timeout activates (0 = use default)
 */
void zeta_set_boot_timeout(zeta_sbc_v2_t* zeta, uint64_t timeout_cycles);

/**
 * Disable boot timeout
 * Port reads will always return actual hardware values
 * @param zeta Pointer to emulator context
 */
void zeta_disable_boot_timeout(zeta_sbc_v2_t* zeta);

/**
 * Check if boot timeout mode is active
 * @param zeta Pointer to emulator context
 * @return true if in timeout mode (no hardware)
 */
bool zeta_in_boot_timeout_mode(zeta_sbc_v2_t* zeta);

/**
 * Check and update boot timeout state
 * Call this in the execution loop to update timeout mode
 * @param zeta Pointer to emulator context
 */
void zeta_check_boot_timeout(zeta_sbc_v2_t* zeta);

/* ============================================================================
 * Z80pack/CPMSIM Support
 * ============================================================================ */

/**
 * Enable z80pack mode
 * When enabled, z80pack I/O ports (0x00-0x11) are active
 */
void z80pack_enable(void);

/**
 * Disable z80pack mode
 */
void z80pack_disable(void);

/**
 * Check if z80pack mode is enabled
 * @return true if z80pack mode is active
 */
bool z80pack_is_enabled(void);

/**
 * Enable CPMSIM boot mode
 * When enabled, boot sector is loaded from disk to address 0
 */
void cpmsim_enable(void);

/**
 * Disable CPMSIM boot mode
 */
void cpmsim_disable(void);

/**
 * Check if CPMSIM mode is enabled
 * @return true if CPMSIM boot mode is active
 */
bool cpmsim_is_enabled(void);

/**
 * Configure z80pack/CPMSIM mode
 * @param enable_z80pack Enable z80pack I/O ports
 * @param enable_cpmsim Enable CPMSIM boot sector loading
 */
void z80pack_configure(bool enable_z80pack, bool enable_cpmsim);

/**
 * Initialize z80pack subsystems
 * @param zeta Pointer to emulator context
 */
void z80pack_init(zeta_sbc_v2_t* zeta);

/**
 * Reset z80pack subsystems
 * @param zeta Pointer to emulator context
 */
void z80pack_reset(zeta_sbc_v2_t* zeta);

/**
 * Handle z80pack port I/O
 * @param zeta Pointer to emulator context
 * @param port Port number
 * @param is_read true for read, false for write
 * @return Value read (for read), 0 (for write)
 */
uint8_t z80pack_handle_io(zeta_sbc_v2_t* zeta, uint8_t port, bool is_read);

/**
 * Check if z80pack handles this port
 * @param port Port number
 * @return true if z80pack handles this port
 */
bool z80pack_handles_port(uint8_t port);

/**
 * Poll z80pack TTY for input
 * Should be called periodically during execution
 * @param zeta Pointer to emulator context
 */
void z80pack_poll(zeta_sbc_v2_t* zeta);

/**
 * Mount a disk image for z80pack FDC
 * @param drive Drive number (0-7)
 * @param filename Path to disk image file
 * @param geometry Pointer to disk geometry (NULL for auto-detect)
 * @return 0 on success, non-zero on error
 */
int z80pack_mount_disk(int drive, const char* filename, void* geometry);

/**
 * Unmount a disk from z80pack FDC
 * @param drive Drive number
 */
void z80pack_unmount_disk(int drive);

/**
 * Get z80pack version string
 * @return Version string
 */
const char* z80pack_get_version(void);

/**
 * Print z80pack status/debug information
 * @param zeta Pointer to emulator context
 */
void z80pack_debug_output(zeta_sbc_v2_t* zeta);

/**
 * Configure serial callbacks for z80pack TTY
 */
void z80pack_set_serial_callbacks(
    void (*send)(uint8_t, void*),
    uint8_t (*recv)(void*),
    bool (*available)(void*),
    void* userdata);

/**
 * Set boot disk path for CPMSIM
 * @param path Path to disk image file
 */
void cpmsim_set_boot_disk(const char* path);

/**
 * Perform automatic CPMSIM boot sequence
 * @param zeta Pointer to emulator context
 * @return 0 on success, non-zero on error
 */
int cpmsim_auto_boot_sequence(zeta_sbc_v2_t* zeta);

/**
 * Get z80pack FDC status
 * @return Status byte
 */
uint8_t z80pack_fdc_get_status(void);

/**
 * Get current z80pack FDC drive
 * @return Drive number
 */
int z80pack_fdc_get_drive(void);

/**
 * Get current z80pack FDC track
 * @return Track number
 */
int z80pack_fdc_get_track(void);

/**
 * Get current z80pack FDC sector
 * @return Sector number
 */
int z80pack_fdc_get_sector(void);

#endif /* ZETA_SBC_V2_EMULATOR_H */
