# Muzix

Muzix is an operating system for the Zilog Z80 and the Zeta SBC
V2. It uses the MINIX-v1 filesystem format and draws on selected ideas from
MINIX 1.0 (message-passing IPC, cooperative scheduling, priority queues).
Some low-level Zeta V2 support routines and techniques are adapted from
FUZIX (DS1302 RTC, NS16550 UART, Z80 saved-stack/switch-out patterns,
bank-switch conventions).

https://github.com/user-attachments/assets/4791f19b-e926-4950-8006-2b37b8837c56

## What's included

- `kernel/` — entry point, kernel loop, process table, scheduling, system
  calls, and user-program loading.
- `mm/` — physical-page allocation and user-memory copying.
- `fs/` — filesystem, ROM-backed block device, and TTY service.
- `pm/` — a limited process-management service.
- `platform/zeta-v2/` — banked memory, context switching, UART, RTC, and CTC.
- `lib/`, `shell/`, `apps/` — user library, shell, and the `cat`, `echo`, `ls`,
  `date`, `top`, `uptime`, `loadavg`, and `hwdiag` programs.
- `tools/` — image converters and build checks.

The scheduler is cooperative. The CTC runs at 120 Hz and handles time
accounting and `park` deadlines, but does not preempt a running userspace
process. UART interrupts wake waiting processes. `time()` reads the calendar
from the DS1302 and converts it to seconds since 2000-01-01; `uptime()` uses an
RTC reading captured at boot.

Muzix implements a subset of the MINIX ABI: `wait()` reaps an
already-exited child once or returns `-1`, and pipes are bounded without
blocking the caller. For implementation limits and platform rules, see
[`AGENTS.md`](./AGENTS.md)

## Low-level code origins

Selected Zeta V2 hardware routines and patterns were adapted from FUZIX:
DS1302 transactions and write-only port shadowing, NS16550 divisor setup, and
Z80 stack-saving, switch-out, and idle patterns. FUZIX was also used as a
reference when checking the bank map and peripheral behavior. These are
Selective adaptations: Muzix has its own process
model, syscall ABI, CTC divisor chain, allocator, and filesystem. The
components and their attribution are documented in
[`docs/hardware-notes.md`](./docs/hardware-notes.md). Preserve applicable
upstream license notices when redistributing derived source or binaries. This
document is included in the ROM filesystem as `hw-notes`.

## AI use

Muzix is, in part, a “vibe-frankensteined” project: AI tools were used
extensively in developing the system, individual components, and
documentation. The code was reviewed and tested iteratively, but AI
involvement is no guarantee that it is bug-free. Before using it on hardware,
verify the build and system behavior.

## Building and testing

### Prerequisites

The ROM build and tests require:

- SDCC **4.5.0**: `sdcc`, `sdasz80`, and `sdldz80` (the Z80 assembler and
  linker). Install them as system commands available on `PATH`.
- `make`, `gcc` (the system clang compiler is also suitable), `sed`, `grep`,
  and standard Unix utilities.
- Ruby for build and validation scripts. Only Ruby's standard library is
  used; no third-party gems are required.

On macOS, install the Command Line Tools, SDCC, and Ruby:

```sh
xcode-select --install
brew install sdcc ruby
```

`xcode-select --install` provides `make`, clang (available as `gcc`), `sed`,
`grep`, and the tools needed to build the emulator. If SDCC 4.5.0 or Ruby is
already installed and available on `PATH`, you can omit that package from the
Homebrew command.

On Linux, install the system build tools and Ruby (for example,
`sudo apt install build-essential make ruby sdcc`). Make sure the installed
SDCC version is **4.5.0**; if your distribution provides a different version,
install or build SDCC 4.5.0 for your OS. Other versions have not been validated
for this project.

Check that the required tools are available:

```sh
sdcc --version
command -v sdasz80 sdldz80 make gcc ruby
```

The expected compiler version is `4.5.0`.

### Build

Run all commands from the `Muzix/` root:

```sh
make rom
make check
```

`make rom` builds and validates the 512 KiB ROM image. `make check` runs the
source checks and the subset of host tests that can run with the host's memory
model. The Makefile is the supported build entry point.

To build the emulator separately, you also need a C compiler and POSIX
pthreads (provided by the Command Line Tools on macOS or `build-essential` on
Debian/Ubuntu):

```sh
make -C emulator
```

Then launch it from the emulator directory:

```sh
cd emulator
./z80pack-emu -s muzix ../build/muzix-zeta-v2.rom -r -i
```

## Hardware Peripheral Connections (Zeta SBC V2)

Muzix uses the Zeta SBC V2 PPI (8255A / КР580ВВ55А clone at I/O ports
0x60–0x63) for bit-bang I²C and SPI. The following tables are derived from the
official Zeta SBC V2 KiCad schematic (files/Zeta SBC V2 - KiCad - 2.0.zip,
netlist `Z80-SBC.net`) and verified against the Muzix production source
(`platform/zeta-v2/spi_io.c`, `platform/zeta-v2/w5100.c`, `lib/i2c.c`,
`lib/i2c.h`).

### Zeta SBC V2 Connectors

| Connector | Type | Function |
|-----------|------|----------|
| P1 | CONN_3 (3-pin) | Power input (+5V, GND, CONFIG jumper) |
| P2 | CONN_2 | Reset button |
| P3 | CONN_DE9 (DB9 male) | Serial console (UART, 38400 baud 8N1) |
| P4 | CONN_13X2 | Parallel port (PPI lines for I²C and SPI) |
| P5 | CONN_17X2 | Floppy disk (WD37C65 FDC) |
| P6 | CONN_2 | 5V power output |

### Power (P1)

| P1 pin | Signal | Description |
|--------|--------|-------------|
| 1 | VCC | +5V power input (center positive) |
| 2 | CONFIG | Jumper JP1 pin 1 (configuration) |
| 3 | GND | Ground |

### Serial Console (P3, DB9)

| P3 pin | Signal | Muzix usage |
|--------|--------|-------------|
| 1 | — | Not used by Muzix |
| 2 | /RX | UART receive (console input) |
| 3 | /TX | UART transmit (console output) |
| 4 | — | Not used by Muzix |
| 5 | GND | Ground |
| 6 | — | Not used by Muzix |
| 7 | /RTS | UART RTS (flow control) |
| 8 | /CTS | UART CTS (flow control) |
| 9 | — | Not used by Muzix |

Baud rate is 38400, 8N1. UART clock divider is configured at boot via
`muzix_zeta_uart_init()` (`platform/zeta-v2/uart_io.s`).

### I²C Display (PCF8574 LCD adapter via PPI Port C)

The I²C bus is bit-banged on PPI Port C. The PCF8574 I²C address is `0x4E`
(7-bit). Standard Chinese "blue" I²C LCD adapter pin mapping:
RS=P0, RW=P1 (unused), E=P2, Backlight=P3, D4-D7=P4-P7.

| Signal | PPI line | PPI port/bit | U5 pin | P4 pin | Notes |
|--------|----------|--------------|--------|--------|-------|
| SDA | PC0 | Port C, bit 0 | 14 | 1 | Bidirectional, open-drain, pull-up to VCC |
| SCL | PC4 | Port C, bit 4 | 13 | 9 | Output, pull-up to VCC |

PPI control register (0x63):
- `0x90` — Port C lower OUTPUT (SDA driving), Port C upper OUTPUT (SCL output)
- `0x91` — Port C lower INPUT (SDA released for ACK sampling), Port C upper OUTPUT

I²C timing: Standard mode (~100 kHz), tLOW ≥ 4.7 µs, tHIGH ≥ 4.0 µs.
КР580ВВ55А settling: 2 NOPs (~500 ns) after each port write.

Source: `lib/i2c.c`, `lib/i2c.h`, `platform/zeta-v2/spi_io.c` (comment block).

### W5100 Ethernet Controller (via SPI via PPI)

The W5100 is accessed via SPI bit-bang on PPI Ports A and C. The SPI driver
uses `muzix_zeta_spi_*` functions (`platform/zeta-v2/spi_io.c`).

| Signal | PPI line | PPI port/bit | U5 pin | P4 pin | Polarity |
|--------|----------|--------------|--------|--------|----------|
| SPI_CLK | PC5 | Port C, bit 5 | 12 | 11 | Output |
| SPI_MOSI | PC6 | Port C, bit 6 | 11 | 13 | Output |
| SPI_CS_N | PC7 | Port C, bit 7 | 10 | 15 | Output, active low |
| SPI_MISO | PA0 | Port A, bit 0 | 4 | 2 | Input |

PPI control register (0x63): `0x90` — Port A INPUT (MISO), Port C upper OUTPUT
(CLK/MOSI/CS), Port C lower OUTPUT.

SPI timing: SCLK ≤ 25 kHz, tHIGH ≥ 20 µs, tLOW ≥ 20 µs. Frequency-compensating
delay is inserted at 20 MHz CPU to meet the minimum half-period.

W5100 SPI opcodes: WRITE = 0xF0, READ = 0x0F. Chip select is active low
(asserted low during transaction).

Source: `platform/zeta-v2/spi_io.c`, `platform/zeta-v2/w5100.c`,
`platform/zeta-v2/w5100.h`.

### P4 Parallel Port Pinout (Complete)

All PPI lines are routed to P4 (CONN_13X2). Pins not listed below are not
connected in the Zeta SBC V2 schematic.

| P4 pin | Signal | PPI line | U5 pin | Muzix usage |
|--------|--------|----------|--------|-------------|
| 1 | PC0 (SDA) | Port C, bit 0 | 14 | I²C SDA |
| 2 | PA0 (MISO) | Port A, bit 0 | 4 | SPI MISO |
| 3 | PA1 | Port A, bit 1 | 3 | — |
| 4 | PA2 | Port A, bit 2 | 2 | — |
| 5 | PC2 | Port C, bit 2 | 16 | — |
| 6 | PA3 | Port A, bit 3 | 1 | — |
| 7 | PC3 | Port C, bit 3 | 17 | — |
| 8 | PA4 | Port A, bit 4 | 40 | — |
| 9 | PC4 (SCL) | Port C, bit 4 | 13 | I²C SCL |
| 10 | PA5 | Port A, bit 5 | 39 | — |
| 11 | PC5 (CLK) | Port C, bit 5 | 12 | SPI CLK |
| 12 | PA6 | Port A, bit 6 | 38 | — |
| 13 | PC6 (MOSI) | Port C, bit 6 | 11 | SPI MOSI |
| 14 | PA7 | Port A, bit 7 | 37 | — |
| 15 | PC7 (CS_N) | Port C, bit 7 | 10 | SPI CS (active low) |
| 16 | — | — | — | Not connected |
| 17 | PB7 | Port B, bit 7 | 25 | — |
| 18 | — | — | — | Not connected |
| 19 | PB6 | Port B, bit 6 | 24 | — |
| 20 | — | — | — | Not connected |
| 21 | PB5 | Port B, bit 5 | 23 | — |
| 22 | — | — | — | Not connected |
| 23 | PB4 | Port B, bit 4 | 22 | — |
| 24 | PB3 | Port B, bit 3 | 21 | — |
| 25 | PAR_VCC | — | — | Power (+5V via JP2) |

### Notes

- The PPI (8255A / КР580ВВ55А) control register is **write-only**; Muzix
  saves the shadow state in `_DATA` and never reads back the port registers.
- I²C SDA (PC0) switches between OUTPUT (driving) and INPUT (ACK sampling)
  by changing the PPI control word (`0x90` ↔ `0x91`). SCL (PC4) remains OUTPUT
  at all times.
- W5100 chip select (PC7) is active low. The SPI driver asserts CS low before
  each transaction and deasserts it after.
- No separate W5500 support; Muzix uses W5100 only. Do not substitute W5500
  without modifying the SPI protocol handling.
- Physical pin numbers for P4 are from the Zeta SBC V2 KiCad schematic
  (`Z80-SBC.net` netlist in `files/Zeta SBC V2 - KiCad - 2.0.zip`).
  Schematic PDF: `files/Zeta SBC V2 - Schematic - Color - 2.0.pdf`.
- Repository: https://github.com/skiselev/zeta_sbc

