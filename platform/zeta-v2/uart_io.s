.module muzix_zeta_uart

.globl _muzix_zeta_uart_init
.globl _muzix_zeta_uart_send
.globl _muzix_zeta_uart_recv
.globl _muzix_zeta_uart_available
; The tick counter, read inline by the bounded wait in muzix_zeta_uart_send.
; tick.rel has to be linked ahead of this module for sdldz80 to resolve it; the
; Makefile puts it immediately after muzix_kernel_entry, so it does.
.globl _muzix_tick_count

.area _CODE

; NS16550-compatible UART at ports 0x68-0x6D.
; LSR bit 0 means RX data is ready; bit 5 means THR is empty.
;
; The divisor is programmed, and it has to be: the baud rate depends on the
; board's default, and the CPU clock is not the UART clock. This adapts FUZIX's
; sequence for this same board; the clock is 1 843 200 Hz, not 7 372 800 Hz,
; so at 38400 baud
; the divisor is 3 rather than 12.  Getting that wrong is silent on the
; emulator, which defaults its divisor to the same 3, and produces nothing but
; noise on hardware.
;
; LCR bit 7 is DLAB: with it set, 0x68 and 0x69 are the divisor latches instead
; of the receive and interrupt-enable registers.  The previous version wrote
; 0x03 to 0x6C with DLAB clear, which is MCR - the divisor was never set at all,
; and the port ran at whatever rate the board was left in.  The user saw
; ".h.h.h" on the console, which is what a baud mismatch looks like.
;
; FUZIX also skips this when booting from ROM, because the ROM has already set
; the line parameters and overwriting them would break it.  We always boot from
; ROM into RAM, so we set them.

MUZIX_UART_LCR    .equ  0x6B
MUZIX_UART_IER    .equ  0x69      ; DLM once DLAB is set
MUZIX_UART_THR    .equ  0x68      ; DLL once DLAB is set
MUZIX_UART_FCR    .equ  0x6A
MUZIX_UART_MCR    .equ  0x6C
MUZIX_UART_CLOCK  .equ  1843200
MUZIX_UART_BAUD   .equ  38400
MUZIX_UART_DIVISOR .equ (MUZIX_UART_CLOCK / (16 * MUZIX_UART_BAUD))

; How long muzix_zeta_uart_send will wait for the transmit holding register to
; drain before giving the byte up.  Ticks of the 120 Hz CTC, so 4 is 33 ms -
; 128 character times at this board's 38400 baud, against a real worst case of
; one character time.  The reasoning is on the routine.
MUZIX_UART_SEND_TICKS .equ 4

_muzix_zeta_uart_init:
    ld      a, #0x80               ; LCR: DLAB on, divisor latches visible
    out     (MUZIX_UART_LCR), a
    ld      a, #MUZIX_UART_DIVISOR & 0xFF
    out     (MUZIX_UART_THR), a
    ld      a, #MUZIX_UART_DIVISOR >> 8
    out     (MUZIX_UART_IER), a
    ld      a, #0x03               ; LCR: 8 data bits, no parity, 1 stop
    out     (MUZIX_UART_LCR), a    ;      and DLAB off again
    ld      a, #0x07               ; FCR: enable and clear the FIFOs
    out     (MUZIX_UART_FCR), a
    ld      a, #0x03               ; MCR: DTR and RTS on
    out     (MUZIX_UART_MCR), a

; Interrupt enable: bit 0 = received data available, and nothing else.
;
; THIS BYTE WAS NEVER WRITTEN BEFORE, so the UART had no interrupts at all: the
; receive side was polled by fs/tty_device.c and the transmitter was never
; interrupt driven.  Turning on RX is what lets a keystroke wake a process that
; is parked, instead of that process having to wait out its park to ask again.
;
; The interrupt reaches the CPU by way of CTC channel 2, which muzix_tick_init
; programs as a rising-edge counter with a time constant of 1. The 16550 asserts
; RX interrupt when its FIFO becomes non-empty; the CTC edge wakes the parked
; reader, which then drains the FIFO. The handler is _muzix_uart_rx_isr in
; platform/zeta-v2/tick.s.
;
; Written here, after DLAB is off again above: port $69 is the divisor latch high
; byte while DLAB is set and the IER only once it is clear, so this must not
; move up or the divisor is rewritten with 0x01.
;
; Only bit 0.  Bit 1 (THRE) is deliberately left off: the transmitter is polled
; with a bounded wait in muzix_zeta_uart_send, and enabling it would add an
; interrupt per byte transmitted to do work the caller is already waiting on.
    ld      a, #0x01               ; IER: received data available only
    out     (MUZIX_UART_IER), a
    ret

; void muzix_zeta_uart_send(uint8_t byte, void *userdata)
;
; sdcccall(1) with this SDCC: a uint8_t first parameter arrives in A, and a
; 16-bit one in HL.  Confirmed from the generated code - trace.c's uart_putc
; tail-calls this with nothing loaded but `ld de,#0x0000`, so the byte is
; already in A.  Reading L instead of A transmits whatever L happened to hold
; and every boot message disappears, which is exactly what it did.
;
; param2 (userdata) in DE, ignored.
;
; THE WAIT IS NOW BOUNDED.  This used to be an unconditional `jr z,001$` on the
; line status register, with no exit: a UART that never reports THR empty - a
; board with no transceiver fitted, a line held off by flow control, a hang
; after the FIFO is disabled - meant this never returned, and because the
; kernel is non-preemptive and calls this from every console write, the whole
; machine stopped.  A wait that cannot end is not a wait.
;
; The bound is the CTC tick (platform/zeta-v2/tick.s, 120 Hz), not a loop
; count, so it is the same time base every other deadline in the system uses
; and it does not drift with the clock the way an iteration count does.
;
; 4 ticks = 33 ms.  At 38400 baud a character is 10 bit times = 260 us, and
; this loop waits for the THR to drain at most one character, so the real
; worst case is 260 us and the budget is 128 times it.  That margin is
; deliberate: a false expiry drops a console byte, which is a visible defect,
; so the budget is set where a false expiry is not something the board can
; produce.  The upper bound still exists, which is the point - a broken line
; costs 33 ms and then the byte is dropped, instead of costing forever.
;
; ON EXPIRY THE BYTE IS DROPPED, not written.  Writing it into a THR that is
; not ready either overwrites a character the UART is still shifting out or
; is silently discarded by the FIFO, and neither is recoverable; not writing it
; is at least a byte the caller can see is missing.  The function returns
; void, so it cannot report that - which is a real gap and is the reason the
; budget above is set as high as it can honestly be rather than lower.  The
; thing that would fix it properly is a send that reports failure, and that is
; a change to the TTY write path's contract rather than to this loop.
;
; Only the low-level LSR read is bounded.  Nothing about the line discipline -
; XON, XOFF, the canonical buffer, the kill and erase characters - is touched
; here; those live in fs/tty_device.c and have their own deadlines.
;
; The low tick byte is enough for this four-tick timeout: elapsed time is
; compared modulo 256, and the threshold is far below one low-byte wrap.
_muzix_zeta_uart_send:
    ld      c,a             ; the byte; A is used for the status read below
    push    bc
    push    de
    push    hl
    ld      hl,#_muzix_tick_count
    ld      e,(hl)          ; E = starting tick, low byte
001$:
    in      a, (0x6d)       ; LSR: bit 5 = THR empty
    and     #0x20
    jr      nz, 002$        ; ready - transmit
    ld      hl,#_muzix_tick_count
    ld      a,(hl)
    sub     e               ; elapsed ticks, modulo 256
    cp      #MUZIX_UART_SEND_TICKS
    jr      c, 001$         ; still inside the budget - keep waiting
    pop     hl              ; out of time: drop the byte and return
    pop     de
    pop     bc
    ret
002$:
    pop     hl
    pop     de
    pop     bc
    ld      a,c
    out     (0x68), a
    ret

; uint8_t muzix_zeta_uart_recv(void *userdata)
; sdcccall(1) param1 (userdata) in HL, ignored; return in A
_muzix_zeta_uart_recv:
    in      a, (0x68)
    ret

; uint8_t muzix_zeta_uart_available(void *userdata)
; sdcccall(1) param1 (userdata) in HL, ignored; return in A
_muzix_zeta_uart_available:
    in      a, (0x6d)
    and     #0x01
    ret