/**
 * Z80pack Emulator - Main Entry Point
 * 
 * A clean z80pack-style Z80 emulator that supports:
 * - RomWBW
 * - FUZIX
 * - Muzix
 * 
 * Usage:
 *   z80pack [options] [rom_file]
 * 
 * Options:
 *   -s, --system SYS   System: romwbw, fuzix, muzix (default: romwbw)
 *   -d, --disk FILE    Disk image file
 *   -r, --run          Run continuously
 *   -c, --cycles N     Max cycles (0=unlimited)
 *   -v, --verbose      Verbose output
 *   -h, --help         Show help
 *   --version          Show version
 * 
 * Examples:
 *   z80pack ZETA2_std.rom                  # Run RomWBW
 *   z80pack -s fuzix -d fuzix.img          # Run FUZIX
 *   z80pack -s muzix rom.bin               # Run Muzix
 *   z80pack -d disk.img -r                 # Run with disk, continuous
 * 
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/select.h>

#include "z80pack/z80pack.h"
#include "z80/z80.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define EMULATOR_NAME      "Z80pack Emulator"
#define EMULATOR_VERSION   "1.0.0"

/* Global emulator instance for signal handler */
static z80pack_t* g_emulator = NULL;
static bool g_running = true;

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    fprintf(stderr, "\nReceived interrupt, stopping...\n");
    g_running = false;
    if (g_emulator) {
        g_emulator->running = false;
    }
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ============================================================================
 * Help and Version
 * ============================================================================ */

static void print_version(void)
{
    printf("%s v%s\n", EMULATOR_NAME, EMULATOR_VERSION);
    printf("Z80 CPU emulator based on z80pack architecture\n");
    printf("\n");
    printf("z80pack version: %s\n", z80pack_get_version());
    printf("License: MIT\n");
}

static void print_help(const char* program)
{
    printf("Usage: %s [options] [rom_file]\n", program);
    printf("\n");
    printf("%s - Z80 CPU emulator based on z80pack\n", EMULATOR_NAME);
    printf("\n");
    printf("Options:\n");
    printf("  -s, --system SYS   System to emulate:\n");
    printf("                      romwbw - RomWBW operating system\n");
    printf("                      fuzix  - FUZIX Unix-like OS\n");
    printf("                      muzix  - Muzix OS\n");
    printf("                      cpmsim - CP/M simulator (boot only)\n");
    printf("  -d, --disk FILE    Disk image file for FDC\n");
    printf("  -r, --run          Run continuously (until halt or Ctrl+C)\n");
    printf("  -c, --cycles N     Maximum cycles to execute (0=unlimited)\n");
    printf("  -i, --stdin        Enable stdin-to-UART input forwarding\n");
    printf("      --strict-zeta  Enforce strict ZETA SBC V2 behavior\n");
    printf("      --compat-hacks Enable legacy compatibility shortcuts\n");
    printf("  -v, --verbose      Enable verbose debug output\n");
    printf("  -w, --watch ADDR   Report machine state whenever PC reaches ADDR (hex)\n  --watchw ADDR      Report every store to ADDR with its PC\n  --trace N          Print the last N program counters on exit\n");
    printf("  -h, --help         Show this help message\n");
    printf("  --version          Show version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s ZETA2_std.rom               # Run RomWBW\n", program);
    printf("  %s -s fuzix -d fuzix.img       # Run FUZIX\n", program);
    printf("  %s -s muzix rom.bin -r -i      # Run Muzix with stdin input\n", program);
    printf("  %s -d disk.img -c 1000000      # Run with disk, 1M cycles\n", program);
    printf("\n");
    printf("Port Usage (z80pack style):\n");
    printf("  TTY:  0x00-0x01, 0x28-0x29, 0x2A-0x2B, 0x32-0x33\n");
    printf("  FDC:  0x0A-0x11\n");
    printf("  Bank: 0x78-0x7B\n");
    printf("  Common memory: 0xF000-0xFFFF\n");
}

/* ============================================================================
 * Serial Output
 * ============================================================================ */

static void serial_output(uint8_t byte, void* userdata)
{
    (void)userdata;
    /* Output character immediately - use putchar for better buffering control */
    putchar(byte);
    fflush(stdout);  /* Ensure it's flushed immediately */
}

static void pump_stdin_to_input_buffer(z80pack_t* pack)
{
    if (!pack || !pack->enable_stdin_input) {
        return;
    }

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        uint8_t ch;
        ssize_t n;
        size_t next_tail;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) {
            return;
        }
        if (!FD_ISSET(STDIN_FILENO, &rfds)) {
            return;
        }

        n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            return;
        }

        next_tail = (pack->input_tail + 1) % sizeof(pack->input_buffer);
        if (next_tail == pack->input_head) {
            return; /* input buffer full */
        }
        pack->input_buffer[pack->input_tail] = ch;
        pack->input_tail = next_tail;
    }
}

static uint8_t serial_recv(void* userdata)
{
    z80pack_t* pack = (z80pack_t*)userdata;

    if (!pack) return 0;
    pump_stdin_to_input_buffer(pack);

    if (pack->input_head == pack->input_tail) return 0;
    uint8_t ch = pack->input_buffer[pack->input_head];
    pack->input_head = (pack->input_head + 1) % sizeof(pack->input_buffer);
    return ch;
}

static bool serial_available(void* userdata)
{
    z80pack_t* pack = (z80pack_t*)userdata;
    if (!pack) return false;
    pump_stdin_to_input_buffer(pack);
    return (pack->input_head != pack->input_tail);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void atexit_dump_trace(z80pack_t *pack)
{
    if (pack) {
        z80pack_dump_trace(pack);
    }
}

/* Addresses on the command line are hexadecimal, full stop.
 *
 * strtol with base 0 auto-detects, so the bare hex form the link map prints -
 * "0098" - is read as OCTAL, because it starts with a zero. strtol then stops
 * at the first digit it cannot use and returns something else entirely, with
 * no error: "-w 0098" watched 0x62, not 0x98.
 *
 * That is worth calling out because it fails as "the watchpoint never fired"
 * rather than as a parse error, which reads as evidence about the system under
 * test rather than about the tool. Several observations during the exec-path
 * investigation were wrong because of it. A 0x prefix is still accepted, since
 * strtol handles that itself. */
static uint16_t z80_addr(const char *text)
{
    return (uint16_t)strtol(text, NULL, 16);
}

int main(int argc, char* argv[])
{
    /* Make output unbuffered */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    /* Configuration */
    const char* rom_file = NULL;
    const char* disk_file = NULL;
    z80pack_system_t system = SYSTEM_ROMWBW;
    bool run_continuously = false;
    uint64_t max_cycles = 0;
    bool verbose = false;
    bool enable_stdin = false;
    bool strict_zeta = true;
    uint16_t watch_addr = 0xFFFFu;
    bool have_watch = false;
    uint16_t watchw_addr = 0xFFFFu;
    bool have_watchw = false;
    int trace_len = 0;
    bool have_trace = false;
    
    /* Command-line options */
    static struct option long_options[] = {
        {"system",   required_argument,  NULL, 's'},
        {"disk",     required_argument,  NULL, 'd'},
        {"run",      no_argument,        NULL, 'r'},
        {"cycles",   required_argument,  NULL, 'c'},
        {"stdin",    no_argument,        NULL, 'i'},
        {"strict-zeta", no_argument,     NULL,  2},
        {"compat-hacks", no_argument,    NULL,  3},
        {"verbose",  no_argument,        NULL, 'v'},
        {"watch",    required_argument,  NULL, 'w'},
        {"watchw",   required_argument,  NULL, 4},
        {"trace",    required_argument,  NULL, 5},
        {"help",     no_argument,        NULL, 'h'},
        {"version",  no_argument,        NULL,  1},
        {NULL, 0, NULL, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "s:d:rc:ivhw:45:", long_options, NULL)) != -1) {
        switch (c) {
            case 's':
                if (strcmp(optarg, "romwbw") == 0) {
                    system = SYSTEM_ROMWBW;
                } else if (strcmp(optarg, "fuzix") == 0) {
                    system = SYSTEM_FUZIX;
                } else if (strcmp(optarg, "muzix") == 0) {
                    system = SYSTEM_MUZIX;
                } else if (strcmp(optarg, "cpmsim") == 0) {
                    system = SYSTEM_CPMSIM;
                } else {
                    fprintf(stderr, "Unknown system: %s\n", optarg);
                    return 1;
                }
                break;
                
            case 'd':
                disk_file = optarg;
                break;
                
            case 'r':
                run_continuously = true;
                break;
                
            case 'c':
                max_cycles = (uint64_t)atol(optarg);
                break;
                
            case 'i':
                enable_stdin = true;
                break;
                
            case 'v':
                verbose = true;
                break;
            case 'w':
                watch_addr = z80_addr(optarg);
                have_watch = true;
                break;
            case 4:
                watchw_addr = z80_addr(optarg);
                have_watchw = true;
                break;
            case 5:
                trace_len = atoi(optarg);
                have_trace = true;
                break;

            case 2:  /* --strict-zeta */
                strict_zeta = true;
                break;

            case 3:  /* --compat-hacks */
                strict_zeta = false;
                break;
                
            case 'h':
                print_help(argv[0]);
                return 0;
                
            case 1:  /* --version */
                print_version();
                return 0;
                
            default:
                fprintf(stderr, "Try '%s --help' for more information\n", argv[0]);
                return 1;
        }
    }
    
    /* Remaining argument is ROM file */
    if (optind < argc) {
        rom_file = argv[optind];
    }

    /* Print banner */
    print_version();
    printf("\n");
    
    /* Disable buffering for real-time output */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Create emulator */
    printf("Initializing %s...\n", EMULATOR_NAME);
    g_emulator = z80pack_create();
    if (!g_emulator) {
        fprintf(stderr, "Error: Failed to create emulator\n");
        return 1;
    }
    
    /* Configure emulator */
    z80pack_set_system(g_emulator, system);
    z80pack_set_strict_zeta(g_emulator, strict_zeta);
    z80pack_set_verbose(g_emulator, verbose);
    if (have_watch) {
        z80pack_set_watch(g_emulator, watch_addr);
    }
    if (have_watchw) {
        z80pack_set_watchw(g_emulator, watchw_addr);
    }
    if (have_trace) {
        z80pack_set_trace(g_emulator, trace_len);
    }
    z80pack_set_stdin_input(g_emulator, enable_stdin);
    z80pack_set_serial_callbacks(g_emulator, serial_output, serial_recv, serial_available, g_emulator);
    
    /* Load ROM if specified */
    if (rom_file) {
        if (z80pack_load_rom(g_emulator, rom_file, 0x0000) != 0) {
            z80pack_destroy(g_emulator);
            return 1;
        }
    }
    
    /* Mount disk if specified */
    if (disk_file) {
        if (z80pack_mount_disk(g_emulator, 0, disk_file) != 0) {
            fprintf(stderr, "Warning: Failed to mount disk %s\n", disk_file);
        }
    }
    
    /* Reset emulator */
    printf("Resetting emulator...\n");
    z80pack_reset(g_emulator);
    
    printf("\n");
    printf("Starting execution at PC=0x%04X\n", z80pack_get_pc(g_emulator));
    printf("System: ");
    switch (system) {
        case SYSTEM_ROMWBW: printf("RomWBW\n"); break;
        case SYSTEM_FUZIX:  printf("FUZIX\n"); break;
        case SYSTEM_MUZIX:  printf("Muzix\n"); break;
        case SYSTEM_CPMSIM: printf("CPMSIM\n"); break;
    }
    if (enable_stdin) {
        if (system == SYSTEM_MUZIX || system == SYSTEM_FUZIX) {
            printf("stdin:  Enabled (forwarding to UART port 0x68 - NS16550)\n");
        } else {
            printf("stdin:  Enabled (forwarding to UART port 0x02 - z80pack TTY)\n");
        }
    }
    printf("strict-zeta: %s\n", strict_zeta ? "enabled" : "disabled");
    printf("\n");
    
    /* For MUZIX default to quiet boot unless stdin interaction is enabled.
     * Auto-injected demo scripts can destabilize long-run boot verification.
     */
    if (system == SYSTEM_MUZIX && !enable_stdin) {
        printf("No demo input injected; run with -i for interactive shell.\n");
    } else if (enable_stdin && system == SYSTEM_MUZIX) {
        printf("Waiting for interactive input... (type directly in console)\n");
    }
    
    /* Run emulator */
    if (run_continuously) {
        printf("Running continuously (Ctrl+C to stop)...\n");
        z80pack_run_until_halt(g_emulator);
        atexit_dump_trace(g_emulator);
        printf("\nEmulation halted.\n");
    } else if (max_cycles > 0) {
        printf("Running for %llu cycles...\n", (unsigned long long)max_cycles);
        z80pack_run(g_emulator, max_cycles);
    atexit_dump_trace(g_emulator);
        printf("\nExecution stopped after %llu cycles.\n",
               (unsigned long long)z80pack_get_cycles(g_emulator));
    } else {
        printf("Running until halt (no cycle limit)...\n");
        z80pack_run_until_halt(g_emulator);
        printf("\nExecution halted after %llu cycles.\n",
               (unsigned long long)z80pack_get_cycles(g_emulator));
    }
    
    /* Print final status */
    printf("\n");
    z80pack_print_status(g_emulator);
    
    /* Cleanup */
    z80pack_destroy(g_emulator);
    g_emulator = NULL;
    
    printf("\nEmulator terminated.\n");
    
    return 0;
}
