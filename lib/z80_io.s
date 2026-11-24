;
; lib/z80_io.s - Userspace I/O port access for Z80
;
; MIT License
; Copyright (c) 2026 Muzix Project Contributors
;
; Minimal I/O port access for userspace diagnostics.
; Unlike platform/zeta-v2/z80_io.s (which is kernel-only and
; intentionally lacks z80_inb), this module provides both
; inb and outb for safe userspace port access.
;
; Bank selector ports ($78-$7B) and MPGENA ($7C) are write-only
; and must never be read back.  Peripheral reads on ports like
; the 8255 PPI ($60-$63) are safe.
;
; SDCC 4.5.0 with -mz80 uses sdcccall(1) convention by default
; for extern assembly functions called from C:
;   8-bit param1 -> A
;   8-bit param2 -> L  (lower byte of HL for 16-bit, L for 8-bit)
;   8-bit return  -> A
;   16-bit return -> HL

    .module z80_io_userspace

    .globl _z80_inb
    .globl _z80_outb

    .area _CODE

; uint8_t z80_inb(uint8_t port)
; port in A (sdcccall(1) convention for 8-bit param1)
; returns 8-bit result in A
_z80_inb:
    ld  c, a          ; port from A
    ld  b, #0         ; upper byte = 0 for 8-bit ports
    in  a, (c)        ; result in A
    ret

; void z80_outb(uint8_t port, uint8_t value)
; port in A (param1, sdcccall(1)), value in L (param2, sdcccall(1))
_z80_outb:
    ld  c, a          ; port from A (first param)
    ld  b, #0         ; upper byte = 0 for 8-bit ports
    ld  a, l          ; value from L (second param)
    out (c), a
    ret

; void i2c_delay_cycles(uint8_t count)
; count in A (sdcccall(1) convention for single 8-bit parameter).
;
; Exact timing (T-states), count >= 1:
;   ld b, a           = 4
;   dec b             = 4
;   jr nz, loop       = 7 (not taken, last iteration) / 12 (taken)
;   ret               = 10
;
; Total: 16 * count + 9 T-states
;   count=1: 4+4+7+10 = 25
;   count=2: 4+(4+12)+(4+7)+10 = 41
;   count=N: 4 + 16*(N-1) + 11 + 10 = 16*N + 9
;
; Per-iteration cost (amortized): 16 T-states.
; At 4 MHz: 16 T-states = 4.0 us per iteration.
; At 20 MHz: 16 T-states = 0.8 us per iteration.
;
; The loop body is a single `dec b` (4 T) per iteration, making the
; cost fully predictable and independent of SDCC stack-frame codegen.

.globl _i2c_delay_cycles

_i2c_delay_cycles:
    ld      b, a          ; count -> B  (4 T)
00100$:
    dec     b             ; 4 T  (one iteration of delay work)
    jr      nz, 00100$    ; 7 T exit / 12 T continue
    ret                   ; 10 T

; void spi_delay_cycles(uint8_t count)
; count in A (sdcccall(1) convention for single 8-bit parameter).
;
; Exact timing (T-states), count >= 1: identical to _i2c_delay_cycles.
;   ld b, a           = 4
;   dec b             = 4
;   jr nz, loop       = 7 (not taken, last iteration) / 12 (taken)
;   ret               = 10
;
; Total: 16 * count + 9 T-states
;   count=1: 4+4+7+10 = 25
;   count=N: 16*N + 9
;
; Per-iteration cost: 16 T-states.
; At 4 MHz: 16 T-states = 4.0 us per iteration.
; At 20 MHz: 16 T-states = 0.8 us per iteration.

.globl _spi_delay_cycles

_spi_delay_cycles:
    ld      b, a          ; count -> B  (4 T)
00101$:
    dec     b             ; 4 T  (one iteration of delay work)
    jr      nz, 00101$    ; 7 T exit / 12 T continue
    ret                   ; 10 T