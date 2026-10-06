/**
 * Z80pack-Based Emulator
 * 
 * A clean z80pack-style Z80 emulator that supports:
 * - RomWBW
 * - FUZIX
 * - Muzix
 * 
 * Based on z80pack architecture:
 * https://www.autometer.de/unix4fun/z80pack/
 * 
 * License: MIT
 */

#ifndef Z80PACK_H
#define Z80PACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* DS1302 RTC: standard three-wire interface on a single port (Zeta SBC V2).
 * Bit 7 DATA (bidirectional), bit 6 CLK, bit 5 RD, bit 4 CE, bit 0 the bit the
 * chip drives back to the CPU.  The model is emu_ds1302.c; the port is hooked
 * in z80pack_port_in()/z80pack_port_out(). */
#define Z80PACK_PORT_RTC 0x70

void ds1302_init(void);
void ds1302_reset(void);
uint8_t ds1302_read(void* zeta, uint8_t port);
void ds1302_write(void* zeta, uint8_t port, uint8_t val);
void ds1302_advance_one_second(void);
void ds1302_set_time_bin(uint8_t sec, uint8_t min, uint8_t hour,
                         uint8_t date, uint8_t month, uint8_t dow,
                         uint8_t year);
#include <termios.h>

/* ============================================================================
 * Z80 CPU Configuration
 * ============================================================================ */

/* CPU frequency used by this emulator model; it is not the board's U17 clock. */
#define Z80PACK_CPU_CLOCK       7372800UL
/* UART_CLK/2, the signal the Zeta SBC V2 wires to CTC channel 0's CLK/TG input
 * (board README, "Interrupts").  It is the CPU clock divided by 8 at this
 * build's rate, and channel 1 counts what channel 0 divides it down to. */
#define Z80PACK_UART_CLK_2      921600UL

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

/* Memory sizes */
#define Z80PACK_RAM_SIZE        (512 * 1024)    /* 512KB RAM: pages 0x20-0x3F */
#define Z80PACK_ROM_SIZE        (512 * 1024)    /* 512KB ROM: pages 0x00-0x1F */
#define Z80PACK_BANK_SIZE       (16 * 1024)     /* 16KB bank size */
#define Z80PACK_NUM_BANKS       64
#define Z80PACK_VISIBLE_RAM     (64 * 1024)    /* 64KB visible address space */

/* MUZIX common memory window */
#define Z80PACK_COMMON_BASE     0xFC00
#define Z80PACK_COMMON_SIZE     0x0400

/* Memory regions */
#define REGION_0000     0   /* $0000-$3FFF */
#define REGION_4000     1   /* $4000-$7FFF */
#define REGION_8000     2   /* $8000-$BFFF */
#define REGION_C000     3   /* $C000-$FFFF */

/* ============================================================================
 * Port Configuration
 * ============================================================================ */

/* z80pack TTY ports */
#define Z80PACK_PORT_TTY1_STAT      0x00    /* TTY1 status (read) */
#define Z80PACK_PORT_TTY1_DATA      0x00    /* TTY1 data (write) */
#define Z80PACK_PORT_TTY1_CTL       0x01    /* TTY1 control */

#define Z80PACK_PORT_TTY2_STAT      0x28    /* TTY2 status */
#define Z80PACK_PORT_TTY2_DATA      0x28    /* TTY2 data */
#define Z80PACK_PORT_TTY2_CTL       0x29    /* TTY2 control */

#define Z80PACK_PORT_TTY3_STAT      0x2A    /* TTY3 status */
#define Z80PACK_PORT_TTY3_DATA      0x2A    /* TTY3 data */
#define Z80PACK_PORT_TTY3_CTL       0x2B    /* TTY3 control */

#define Z80PACK_PORT_TTY4_STAT      0x32    /* TTY4 status */
#define Z80PACK_PORT_TTY4_DATA      0x32    /* TTY4 data */
#define Z80PACK_PORT_TTY4_CTL       0x33    /* TTY4 control */

/* z80pack FDC ports */
#define Z80PACK_PORT_FDC_DRIVE      0x0A    /* Drive select */
#define Z80PACK_PORT_FDC_TRACK      0x0B    /* Track register */
#define Z80PACK_PORT_FDC_SECTOR     0x0C    /* Sector register */
#define Z80PACK_PORT_FDC_CMD        0x0D    /* Command register */
#define Z80PACK_PORT_FDC_STAT       0x0E    /* Status register */
#define Z80PACK_PORT_FDC_DMAL       0x0F    /* DMA address low */
#define Z80PACK_PORT_FDC_DMAH       0x10    /* DMA address high */
#define Z80PACK_PORT_FDC_SECTH      0x11    /* Sector register high */

/* z80pack bank control ports */
#define Z80PACK_PORT_BANK0          0x78    /* Bank for $0000-$3FFF */
#define Z80PACK_PORT_BANK1          0x79    /* Bank for $4000-$7FFF */
#define Z80PACK_PORT_BANK2          0x7A    /* Bank for $8000-$BFFF */
#define Z80PACK_PORT_BANK3          0x7B    /* Bank for $C000-$FFFF */
#define Z80PACK_PORT_MPGENA         0x7C    /* Paging enable register */

/* RomWBW UART port ($70) - for ZETA2_std.rom compatibility */
#define Z80PACK_PORT_UART_RWB       0x70    /* RomWBW UART read/write */

/* ZETA2 UART ports ($68-$6F) */
#define Z80PACK_PORT_UART_ZETA_BASE 0x68    /* ZETA2 UART base */
#define Z80PACK_PORT_UART_ZETA_OFFSET 8     /* UART registers span 8 ports */

/* Zeta SBC V2 CTC ports */
#define Z80PACK_PORT_CTC0           0x20
#define Z80PACK_PORT_CTC1           0x21
#define Z80PACK_PORT_CTC2           0x22
#define Z80PACK_PORT_CTC3           0x23

/* Zeta SBC V2 PPI ports */
#define Z80PACK_PORT_PPI_A          0x60
#define Z80PACK_PORT_PPI_B          0x61
#define Z80PACK_PORT_PPI_C          0x62
#define Z80PACK_PORT_PPI_CTL        0x63

/* DS1302 latch bits on port 0x70 */
#define Z80PACK_DSRTC_DATA          0x80
#define Z80PACK_DSRTC_CLK           0x40
#define Z80PACK_DSRTC_RD            0x20
#define Z80PACK_DSRTC_CE            0x10
#define Z80PACK_DSRTC_IN            0x01

/* FDC commands */
#define FDC_CMD_READ            0x00    /* Read sector */
#define FDC_CMD_WRITE           0x01    /* Write sector */
#define FDC_CMD_FORMAT          0x02    /* Format track */
#define FDC_CMD_SEEK            0x03    /* Seek */
#define FDC_CMD_RESTORE         0x04    /* Restore (seek to track 0) */

/* FDC status bits */
#define FDC_STAT_BUSY           0x01    /* Controller busy */
#define FDC_STAT_BUSERR         0x02    /* Bus error */
#define FDC_STAT_READY          0x04    /* Drive ready */
#define FDC_STAT_WRPROT         0x08    /* Write protected */
#define FDC_STAT_HEAD           0x10    /* Head status */
#define FDC_STAT_TRK0           0x20    /* Track 0 */
#define FDC_STAT_INDEX          0x40    /* Index pulse */
#define FDC_STAT_DRQ            0x80    /* Data request */

/* TTY status bits */
#define TTY_STAT_RX_READY       0x01    /* Receive data ready */
#define TTY_STAT_TX_READY       0x02    /* Transmit buffer empty */
#define TTY_STAT_TX_EMPTY       0x04    /* Transmitter empty */
#define TTY_STAT_ERR            0x08    /* Error condition */

/* ============================================================================
 * Disk Geometry
 * ============================================================================ */

#define Z80PACK_MAX_DRIVES      8

/* Standard disk geometries */
#define DISK_8SSSD_SECTORS      26      /* 8" SSSD: 26 sectors/track */
#define DISK_8SSSD_TRACKS       77      /* 8" SSSD: 77 tracks */
#define DISK_8SSSD_SIZE         128     /* 8" SSSD: 128 bytes/sector */

#define DISK_8DSDD_SECTORS      52      /* 8" DSDD: 52 sectors/track */
#define DISK_8DSDD_TRACKS       77      /* 8" DSDD: 77 tracks */
#define DISK_8DSDD_SIZE         256     /* 8" DSDD: 256 bytes/sector */

typedef struct {
    const char* filename;         /* Disk image filename */
    uint8_t* image_data;          /* Loaded image data */
    size_t image_size;            /* Size of image in bytes */
    uint16_t sectors_per_track;   /* Sectors per track */
    uint16_t tracks;              /* Number of tracks */
    uint16_t sector_size;         /* Sector size in bytes */
} z80pack_disk_t;

/* ============================================================================
 * System Type
 * ============================================================================ */

typedef enum {
    SYSTEM_ROMWBW,       /* RomWBW operating system */
    SYSTEM_FUZIX,        /* FUZIX Unix-like OS */
    SYSTEM_MUZIX,        /* Muzix OS */
    SYSTEM_CPMSIM        /* CP/M simulator (boot only) */
} z80pack_system_t;

/* ============================================================================
 * Emulator Context
 * ============================================================================ */

/* Forward declaration */
struct z80pack_t;
typedef struct z80pack_t z80pack_t;

/* Serial callbacks */
typedef void (*z80pack_serial_send_t)(uint8_t byte, void* userdata);
typedef uint8_t (*z80pack_serial_recv_t)(void* userdata);
typedef bool (*z80pack_serial_available_t)(void* userdata);

/* HBIOS callback */
typedef uint16_t (*z80pack_hbios_call_t)(uint8_t func, void* regs, void* userdata);

/* Emulator context */
struct z80pack_t {
    /* Z80 CPU */
    struct z80* cpu;
    void* cpu_userdata;
    
    /* Memory */
    uint8_t* ram;                          /* Full RAM allocation */
    uint8_t* rom;                          /* Full ROM allocation */
    uint8_t visible_ram[Z80PACK_VISIBLE_RAM];  /* Visible 64KB */
    uint8_t common_mem[Z80PACK_COMMON_SIZE];   /* Common memory */
    uint8_t bank_reg[4];                   /* Bank registers */
    uint8_t mpgena;                        /* Paging control register ($7C) */
    bool paging_enabled;                   /* Whether paging is enabled */
    bool strict_zeta;                      /* Enforce strict ZETA SBC V2 behavior */

    /* --watch ADDR: report machine state whenever PC reaches ADDR. 0xFFFF
     * disables it. Set by z80pack_set_watch(). */
    uint16_t watch_pc;
    int watch_hits;
    uint16_t watch_waddr;                  /* --watchw: report stores here */
    int watch_whits;
    /* --trace N: keep a ring of the last N program counters and print it on
     * exit.  Watchpoints answer "is this address reached"; they do not answer
     * "what is it spinning in", which is what a hang actually needs. */
    uint16_t *trace_ring;
    int trace_len;
    int trace_pos;
    long trace_total;
    
    /* CTC state (ports $20-$23) */
    uint8_t ctc_vector_base;               /* IM2 vector base low bits (CH0) */
    uint8_t ctc_control[4];                /* Last control word per channel */
    uint8_t ctc_time_constant[4];          /* Time constant per channel */
    bool ctc_waiting_constant[4];          /* Next write is time constant */
    bool ctc_irq_enabled[4];               /* IRQ enabled for channel */
    uint64_t ctc_last_fire_cycles[4];      /* Last cycle count per channel */
    /* Previous level of (UART RX pending AND IER bit 0) - the signal on CH2's
     * CLK/TG on the board.  Remembered so CH2 fires once per rising edge rather
     * than once per instruction that finds the level asserted. */
    uint8_t ctc_uart_int_prev;
    /* Vectors raised while another interrupt was already pending.  The Z80 has
     * room for one pending vector; the CTC daisy chain has room for one per
     * channel, so the difference has to be modelled here. */
#define Z80PACK_IRQ_QUEUE_DEPTH 8
    uint8_t irq_queue[Z80PACK_IRQ_QUEUE_DEPTH];
    uint8_t irq_queue_len;

    /* DS1302 wall clock: total_cycles at the last whole second handed to the
     * chip.  The CPU is 7.3728 MHz, so a second is 7,372,800 cycles; the model
     * itself only knows how to move one second, and decides how often to be
     * called. */
    uint64_t rtc_last_second_cycles;
    int      rtc_started;
    uint64_t ctc_cycles_per_tick[4];       /* Computed tick interval per channel */

    /* 8255 PPI state (ports 0x60-0x63) */
    uint8_t ppi_port_a;
    uint8_t ppi_port_b;
    uint8_t ppi_port_c;
    uint8_t ppi_control;
    bool ppi_a_input;
    bool ppi_b_input;
    bool ppi_c_upper_input;
    bool ppi_c_lower_input;

    /* DS1302 RTC bit-bang state (port 0x70) */
    uint8_t rtc_latch;
    uint8_t rtc_cmd;
    uint8_t rtc_cmd_bits;
    uint8_t rtc_shift_in;
    uint8_t rtc_shift_out;
    uint8_t rtc_shift_bits;
    uint8_t rtc_read_buf[8];
    uint8_t rtc_read_len;
    uint8_t rtc_read_pos;
    bool rtc_read_mode;
    bool rtc_write_mode;
    bool rtc_ce_high;
    bool rtc_clk_high;
    
    /* Disk system */
    z80pack_disk_t disk[Z80PACK_MAX_DRIVES];
    
    /* FDC state */
    uint8_t fdc_drive;                     /* Selected drive */
    uint16_t fdc_track;                    /* Current track */
    uint16_t fdc_sector;                   /* Current sector */
    uint16_t fdc_dma;                      /* DMA address */
    uint8_t fdc_status;                    /* Status register */
    uint8_t fdc_command;                   /* Last command */
    bool fdc_busy;                         /* Operation in progress */
    bool fdc_drq;                          /* Data request */
    uint8_t* fdc_transfer_ptr;             /* Transfer pointer */
    size_t fdc_transfer_remaining;         /* Bytes remaining */
    
    /* TTY state */
    uint8_t tty_rx_buf[4][256];            /* Receive buffers */
    volatile size_t tty_rx_head[4];
    volatile size_t tty_rx_tail[4];
    uint8_t tty_tx_buf[4][256];            /* Transmit buffers */
    volatile size_t tty_tx_head[4];
    volatile size_t tty_tx_tail[4];
    uint8_t tty_status[4];                 /* Status registers */
    
    /* Input buffer for testing (simulates keyboard input) */
    uint8_t input_buffer[1024];            /* Input queue */
    volatile size_t input_head;            /* Read position */
    volatile size_t input_tail;            /* Write position */
    
    /* stdin input handling */
    bool stdin_ready;                      /* stdin set to non-blocking mode */
    bool enable_stdin_input;               /* Enable stdin-to-UART forwarding */
    struct termios original_termios;       /* Saved terminal settings */
    bool terminal_raw_mode;                /* Whether terminal is in raw mode */
    
    /* Serial callbacks */
    z80pack_serial_send_t serial_send;
    z80pack_serial_recv_t serial_recv;
    z80pack_serial_available_t serial_available;
    void* serial_userdata;
    
    /* UART register state (NS16550 subset) */
    uint8_t uart_lcr;
    uint8_t uart_ier;
    uint8_t uart_dll;
    uint8_t uart_dlh;
    uint8_t uart_mcr;
    uint8_t uart_scr;
    uint8_t uart_fcr;
    
    /* Configuration */
    z80pack_system_t system;               /* Current system */
    bool verbose;                          /* Verbose output */
    
    /* Statistics */
    uint64_t total_cycles;
    /* Cycles the CPU spent with HALT asserted and no interrupt taken.  This is
     * the emulator's own ground truth for how much of the elapsed time the
     * processor was actually stopped, and it is what the kernel's load figure
     * should agree with - the kernel only ever sees its own tick counter. */
    uint64_t halted_cycles;
    uint32_t instructions;
    uint32_t memory_reads;
    uint32_t memory_writes;
    uint32_t port_reads;
    uint32_t port_writes;
    
    /* State */
    bool running;
    bool halted;
};

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Create a new z80pack emulator instance
 * @return Pointer to emulator context, or NULL on error
 */
z80pack_t* z80pack_create(void);

/**
 * Destroy a z80pack emulator instance
 * @param pack Pointer to emulator context
 */
void z80pack_destroy(z80pack_t* pack);

/**
 * Reset the emulator to initial state
 * @param pack Pointer to emulator context
 */
void z80pack_reset(z80pack_t* pack);

/**
 * Set the system type
 * @param pack Pointer to emulator context
 * @param system System type to emulate
 */
void z80pack_set_system(z80pack_t* pack, z80pack_system_t system);

/**
 * Enable strict ZETA SBC V2 emulation (disable compatibility shortcuts)
 * @param pack Pointer to emulator context
 * @param enable Enable or disable strict mode
 */
void z80pack_set_strict_zeta(z80pack_t* pack, bool enable);

/**
 * Enable stdin-to-UART forwarding
 * @param pack Pointer to emulator context
 * @param enable Enable or disable stdin input
 */
void z80pack_set_stdin_input(z80pack_t* pack, bool enable);

/**
 * Load a ROM file
 * @param pack Pointer to emulator context
 * @param filename Path to ROM file
 * @param addr Load address (typically 0x0000)
 * @return 0 on success, non-zero on error
 */
int z80pack_load_rom(z80pack_t* pack, const char* filename, uint16_t addr);

/**
 * Mount a disk image
 * @param pack Pointer to emulator context
 * @param drive Drive number (0-7)
 * @param filename Path to disk image file
 * @return 0 on success, non-zero on error
 */
int z80pack_mount_disk(z80pack_t* pack, int drive, const char* filename);

/**
 * Unmount a disk
 * @param pack Pointer to emulator context
 * @param drive Drive number
 */
void z80pack_unmount_disk(z80pack_t* pack, int drive);

/**
 * Configure serial callbacks
 * @param pack Pointer to emulator context
 * @param send Send callback
 * @param recv Receive callback
 * @param available Available callback
 * @param userdata User data for callbacks
 */
void z80pack_set_serial_callbacks(z80pack_t* pack,
    z80pack_serial_send_t send,
    z80pack_serial_recv_t recv,
    z80pack_serial_available_t available,
    void* userdata);

/**
 * Execute a single instruction
 * @param pack Pointer to emulator context
 */
void z80pack_step(z80pack_t* pack);

/**
 * Run for a maximum number of cycles
 * @param pack Pointer to emulator context
 * @param max_cycles Maximum cycles (0 = unlimited)
 */
void z80pack_run(z80pack_t* pack, uint64_t max_cycles);

/**
 * Run until HALT
 * @param pack Pointer to emulator context
 */
void z80pack_run_until_halt(z80pack_t* pack);

/**
 * Get current PC
 * @param pack Pointer to emulator context
 * @return Current program counter
 */
uint16_t z80pack_get_pc(z80pack_t* pack);

/**
 * Get current SP
 * @param pack Pointer to emulator context
 * @return Current stack pointer
 */
uint16_t z80pack_get_sp(z80pack_t* pack);

/**
 * Get total cycles executed
 * @param pack Pointer to emulator context
 * @return Total cycles
 */
uint64_t z80pack_get_cycles(z80pack_t* pack);

/**
 * Enable verbose output
 * @param pack Pointer to emulator context
 * @param enable Enable verbose mode
 */
void z80pack_set_watch(z80pack_t* pack, uint16_t addr);
void z80pack_set_watchw(z80pack_t* pack, uint16_t addr);
void z80pack_set_trace(z80pack_t* pack, int n);
void z80pack_dump_trace(z80pack_t* pack);
void z80pack_set_verbose(z80pack_t* pack, bool enable);

/**
 * Print emulator status
 * @param pack Pointer to emulator context
 */
void z80pack_print_status(z80pack_t* pack);

/* ============================================================================
 * Memory Access
 * ============================================================================ */

/**
 * Read a byte from memory
 * @param pack Pointer to emulator context
 * @param addr Memory address
 * @return Byte value
 */
uint8_t z80pack_read_byte(z80pack_t* pack, uint16_t addr);

/**
 * Write a byte to memory
 * @param pack Pointer to emulator context
 * @param addr Memory address
 * @param val Value to write
 */
void z80pack_write_byte(z80pack_t* pack, uint16_t addr, uint8_t val);

/**
 * Read a word from memory
 * @param pack Pointer to emulator context
 * @param addr Memory address
 * @return Word value (little-endian)
 */
uint16_t z80pack_read_word(z80pack_t* pack, uint16_t addr);

/**
 * Write a word to memory
 * @param pack Pointer to emulator context
 * @param addr Memory address
 * @param val Word value to write (little-endian)
 */
void z80pack_write_word(z80pack_t* pack, uint16_t addr, uint16_t val);

/* ============================================================================
 * Port I/O
 * ============================================================================ */

/**
 * Read from port
 * @param pack Pointer to emulator context
 * @param port Port number
 * @return Value read
 */
uint8_t z80pack_port_in(z80pack_t* pack, uint8_t port);

/**
 * Write to port
 * @param pack Pointer to emulator context
 * @param port Port number
 * @param val Value to write
 */
void z80pack_port_out(z80pack_t* pack, uint8_t port, uint8_t val);

/* ============================================================================
 * Version Information
 * ============================================================================ */

#define Z80PACK_VERSION_MAJOR   1
#define Z80PACK_VERSION_MINOR   0
#define Z80PACK_VERSION_PATCH   0

const char* z80pack_get_version(void);

/**
 * Inject string into emulator input buffer (for testing TTY input)
 * @param pack Pointer to emulator context
 * @param str String to inject
 */
void z80pack_inject_input_string(z80pack_t* pack, const char* str);

/* ============================================================================
 * HBIOS Support
 * ============================================================================ */

/**
 * Configure HBIOS callback
 * @param pack Pointer to emulator context
 * @param callback Callback function for HBIOS calls
 * @param userdata User data for callback
 */
void z80pack_configure_hbios(z80pack_t* pack,
    z80pack_hbios_call_t callback,
    void* userdata);

/**
 * Set HBIOS verbose mode
 * @param enable Enable verbose output
 */
void z80pack_set_hbios_verbose(bool enable);

/**
 * Execute HBIOS function
 * @param pack Pointer to emulator context
 * @param func HBIOS function number
 * @return Return value from HBIOS
 */
uint16_t z80pack_hbios_exec(z80pack_t* pack, uint8_t func);

/**
 * Add string to console input buffer
 * @param str String to add
 */
void z80pack_console_input_string(const char* str);

/**
 * Clear console input buffer
 */
void z80pack_console_clear(void);

/**
 * Check if HBIOS RST 0x08 is at address
 * @param pack Pointer to emulator context
 * @param addr Address to check
 * @return true if HBIOS RST found
 */
bool z80pack_is_hbios_rst(z80pack_t* pack, uint16_t addr);

/**
 * Handle HBIOS RST 0x08 instruction
 * @param pack Pointer to emulator context
 */
void z80pack_handle_hbios_rst(z80pack_t* pack);

/**
 * Inject key into console buffer
 * @param key Key code to inject
 */
void z80pack_console_inject_key(uint8_t key);

/**
 * Inject string as key presses
 * @param str String to inject
 */
void z80pack_console_inject_string(const char* str);

#endif /* Z80PACK_H */
