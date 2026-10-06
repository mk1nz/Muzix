/*
 * Muzix Z80 Bootloader
 * 
 * This bootloader is responsible for:
 * 1. Initializing Z80 hardware (Zeta V2 board)
 * 2. Setting up memory banks
 * 3. Loading kernel from storage
 * 4. Jumping to kernel entry point
 * 
 * The bootloader runs at address 0x0000 (reset vector)
 */

#include <stdint.h>

#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/uart_io.h"

/* Memory layout */
#define KERNEL_LOAD_ADDR 0x4000    /* Where kernel loads (after bootloader) */
#define KERNEL_SIZE 0x4000         /* Max 16KB kernel */
#define COMMON_BASE 0xC000         /* Common memory (always mapped) */

/* Boot configuration */
#define BOOT_SECTOR 0
#define SECTORS_TO_READ (KERNEL_SIZE / 512)

/* Kernel entry point signature */
typedef void (*kernel_entry_t)(void);
typedef int (*boot_sector_reader_t)(uint32_t sector, void *buffer);
typedef void (*boot_serial_writer_t)(char value);

static boot_sector_reader_t sector_reader;
static boot_serial_writer_t serial_writer;

void bootloader_set_sector_reader(boot_sector_reader_t reader)
{
    sector_reader = reader;
}

void bootloader_set_serial_writer(boot_serial_writer_t writer)
{
    serial_writer = writer;
}

/**
 * Initialize Z80 processor and Zeta V2 board
 */
static void init_hardware(void)
{
    __asm
        di
        ld sp, #0xFFFE
    __endasm;
}

/**
 * Serial port helpers for debug output
 */
static void serial_init(void)
{
    /* A host harness may inject its own writer; target builds use the Zeta
     * NS16550 binding at ports 0x68..0x6d. */
    if (!serial_writer) {
        muzix_zeta_uart_init();
    }
}

static void serial_putchar(char c)
{
    if (serial_writer) {
        serial_writer(c);
    } else {
        muzix_zeta_uart_send((uint8_t)c, 0);
    }
}

static void serial_puts(const char *str)
{
    while (*str) {
        serial_putchar(*str++);
    }
}

/**
 * Load sector from disk
 * 
 * On Zeta V2, this would typically read from:
 * - SD card via SPI
 * - CompactFlash via IDE
 * - Or simple block device
 */
static int read_sector(uint32_t sector, void *buffer)
{
    if (!sector_reader) {
        return -1;
    }
    return sector_reader(sector, buffer);
}

/**
 * Load kernel from storage
 */
static int load_kernel(void)
{
    uint32_t sector;
    uint8_t *destination = (uint8_t *)KERNEL_LOAD_ADDR;

    serial_puts("Bootloader: Loading kernel...\n");

    for (sector = 0; sector < SECTORS_TO_READ; sector++) {
        if (read_sector(BOOT_SECTOR + sector,
                        &destination[sector * 512u]) != 0) {
            serial_puts("Bootloader: sector read failed\n");
            return -1;
        }
    }

    serial_puts("Bootloader: Kernel loaded at 0x4000\n");
    return 0;
}

/**
 * Validate kernel signature
 */
static int validate_kernel(void)
{
    uint16_t *magic = (uint16_t *)KERNEL_LOAD_ADDR;
    
    serial_puts("Bootloader: Validating kernel...\n");
    
    /* Check magic number: 0x5A80 = "Z80" signature */
    if (*magic == 0x5A80) {
        serial_puts("Bootloader: Kernel signature valid\n");
        return 1;
    }
    
    serial_puts("Bootloader: Invalid kernel signature!\n");
    return 0;
}

/**
 * Setup initial memory map for kernel
 */
static void setup_kernel_memory(void)
{
    serial_puts("Bootloader: Setting up memory map...\n");
    
    /* Map the four 16 KiB kernel pages using the same Zeta bank ABI as the
     * syscall runtime.  This replaces the obsolete one-register model that
     * used port 0x00. */
    muzix_zeta_enter_kernel_banks();
    
    serial_puts("Bootloader: Memory configured\n");
}

/**
 * Enable interrupts and jump to kernel
 */
static void jump_to_kernel(void)
{
    kernel_entry_t kernel = (kernel_entry_t)KERNEL_LOAD_ADDR;
    
    serial_puts("Bootloader: Jumping to kernel at 0x");
    /* Would print address */
    serial_puts("4000...\n");
    
    __asm
        ei
    __endasm;
    
    /* Jump to kernel */
    kernel();
    
    /* Should not return */
    while (1);
}

/**
 * Main bootloader entry point
 */
void bootloader_main(void)
{
    serial_init();
    
    serial_puts("================================\n");
    serial_puts("Muzix Z80 Bootloader v1.0\n");
    serial_puts("================================\n");
    
    init_hardware();
    
    if (load_kernel() != 0) {
        serial_puts("ERROR: Failed to load kernel\n");
        while (1);
    }
    
    if (!validate_kernel()) {
        serial_puts("ERROR: Kernel validation failed\n");
        while (1);
    }
    
    setup_kernel_memory();
    
    jump_to_kernel();
}

/**
 * Z80 Reset vector - entry point at 0x0000
 * This is the actual reset handler
 */
void reset_handler(void)
{
    /* Call bootloader_main */
    bootloader_main();
}
