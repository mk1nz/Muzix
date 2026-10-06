# Zeta SBC V2 hardware and code provenance

## Main components used by Muzix

| Component | Role in this system |
| --- | --- |
| Zilog Z80A CPU | Executes the 8-bit kernel and userspace; this board uses the 4 MHz U17 CPU clock. |
| Bank controller | Maps four 16 KiB windows into the 64 KiB CPU address space. |
| SRAM | 512 KiB total, represented by physical RAM page IDs `0x20`–`0x3F`. |
| Zilog CTC | Timer/counter channels at I/O ports `0x20`–`0x23`; drives the 120 Hz scheduler tick and UART interrupt path. |
| NS16550-compatible UART | Serial console at ports `0x68`–`0x6D`; configured by Muzix for 38,400 baud. Its 1.8432 MHz reference is distinct from the CPU clock. |
| DS1302 RTC | Calendar clock accessed through the board's RTC interface at port `0x70`; supplies wall-clock time. |
| ROM | 512 KiB image containing the boot stub, kernel, and read-only filesystem using the MINIX-v1 format. |

The CTC tick is derived from the board wiring, not from U17: channel 0 counts
`UART_CLK/2` (921.6 kHz) down by 256, then channel 1 counts the resulting
3.6 kHz pulses down by 30, yielding 120 Hz. Channel 2 receives the UART
interrupt signal. Channel 3 is used for CPU-cycle measurement and is calibrated
at startup against the tick.

The bank selectors are write-only: software keeps the active map in kernel
state and restores it from that saved map rather than reading back the ports.
The selectors are:

| Register | I/O port | CPU window |
| --- | ---: | --- |
| `MPGSEL_0` | `0x78` | `0x0000`–`0x3FFF` |
| `MPGSEL_1` | `0x79` | `0x4000`–`0x7FFF` |
| `MPGSEL_2` | `0x7A` | `0x8000`–`0xBFFF` |
| `MPGSEL_3` | `0x7B` | `0xC000`–`0xFFFF` |
| `MPGENA` | `0x7C` | Paging enable |

In the active Muzix process map, windows 0 and 3 retain kernel pages while
windows 1 and 2 map allocated process pages. This is Muzix's memory policy;
it is not a hardware restriction on the four selectors.

## FUZIX reference and Muzix adaptations

Some low-level Zeta-specific code and design patterns in Muzix were adapted
from FUZIX. FUZIX is credited here as the source of those adaptations; Muzix
is a distinct system, not a wholesale FUZIX port. The relevant source files in
the upstream project informed the following components:

- **Bank and board conventions:** bank-register, UART, and CTC conventions
  informed the Zeta V2 definitions in `platform/zeta-v2/`.
- **RTC transactions:** `platform/zeta-v2/rtc_ds1302.c` adapts DS1302 pin
  sequencing and write-only port-shadow handling. Muzix's C implementation
  and interfaces are its own.
- **UART setup:** `platform/zeta-v2/uart_io.s` adapts NS16550 divisor-latch
  initialization for the board.
- **Z80 context and idle patterns:** saved-stack handling, non-returning
  switch-out, and idle sequencing in `kernel/proc_table.c`,
  `kernel/kernel_loop.c`, and `platform/zeta-v2/z80_io.s` were informed by
  FUZIX's Z80 techniques. Muzix's process table, syscall resume frame, and
  scheduler are separate implementations.

The board's register map and interrupt wiring are hardware facts, not code
ownership. The CTC divisor chain and Muzix process-map policy are implemented
and validated in this project; do not attribute those Muzix-specific choices
to FUZIX.

Consult and preserve the applicable FUZIX license and upstream notices when
redistributing derived source or binaries. This attribution is not a
replacement for those license terms.

## References

- Zeta SBC V2 board documentation:
  https://github.com/skiselev/zeta_sbc
