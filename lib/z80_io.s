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

    .module z80_io_userspace

    .globl _z80_inb
    .globl _z80_outb

    .area _CODE

; uint8_t z80_inb(uint8_t port)
; SDCC convention: param1 (port) in L, return in L
_z80_inb:
    ld c, l           ; port from SDCC param (L)
    ld b, #0          ; upper byte = 0 for 8-bit ports
    in a, (c)
    ld l, a
    ld h, #0
    ret

; void z80_outb(uint8_t port, uint8_t value)
; SDCC convention: param1 (port) in L, param2 (value) in E
_z80_outb:
    ld c, l           ; port from SDCC param (L)
    ld b, #0          ; upper byte = 0 for 8-bit ports
    ld a, e           ; value from SDCC param (E)
    out (c), a
    ret

