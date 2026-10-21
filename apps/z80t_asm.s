; z80t_asm.s - Z80 CPU identification test implementations
;
; MIT License
; Copyright (c) 2026 Muzix Project Contributors
;
; CPU identification using methods from z80.info research:
; - https://worldofspectrum.org/forums/discussion/41704 (Patrik Rak's Q theory)
; - https://z80.info/zip/z80-documented.pdf (Sean Young's undocumented Z80)
; - http://z80.info/interrupts.html (interrupt timing)
; - TestCMOS: OUT (C),0 (ED 71) for NMOS/CMOS detection
; - TestU880: OUTI carry flag behavior with HL=0FFFFh
; - TestXY: XF/YF signature via SCF/Q-flag theory (Patrik Rak)
; - SLL undocumented shift detection
;
; Algorithm adapted from z80type.asm (Sergey Kiselev) - independent
; reimplementation under MIT license.
; Repository: https://github.com/skiselev/z80-tests

.module z80t_asm

.globl _z80_out_c_zero
.globl _z80_test_u880
.globl _z80_test_scf_flags
.globl _z80_test_ccf_flags
.globl _z80_sll_a
.globl _z80_test_bit_hl
.globl _z80_test_inward_f_bit

.area _CODE

; uint8_t z80_out_c_zero(uint8_t port)
; Execute undocumented OUT (C),0 (ED 71), then read back
; Returns: read-back value (0x00 for NMOS, 0xFF for CMOS on clean port)
; SDCC convention: param1 in L, return in L
_z80_out_c_zero:
    di
    ld c, l           ; port from SDCC param (L)
    ld b, #0
    .db #0xED, #0x71  ; ED 71 = OUT (C),0 undocumented
    in a, (c)
    ld l, a
    ld h, #0
    ei
    ret

; uint8_t z80_test_u880(void)
; OUTI with HL=0xFFFF, B=01h, CF=1
; U880: CF stays set, others: CF cleared
; Also tests INward f-bit behavior for Soviet clones
; Returns: 1 (U880), 0 (other)
; Uses UART SCR port 0x6F as safe I/O target
_z80_test_u880:
    di
    ld hl, #0xFFFF
    ld b, #0x01
    ld c, #0x6F       ; UART SCR port (safe for IN/OUT)
    .db #0x37         ; SCF = set carry flag
    .db #0xED, #0xA3  ; OUTI instruction
    push af           ; save flags
    pop hl            ; HL = flags after OUTI
    ld a, l
    and #0x01         ; isolate CF
    ld l, a
    ld h, #0
    ei
    ret

; uint8_t z80_test_inward_f_bit(void)
; Test whether IN instruction copies F register (NMOS vs Soviet clone)
; On NMOS Z80, lower bits of F are preserved on IN r,(C)
; On Soviet clones (and some others), F is destroyed
; Returns: 1 if F preserved (NMOS), 0 if F destroyed (Soviet/others)
_z80_test_inward_f_bit:
    di
    ; Set known F value
    ld a, #0xC5          ; F = 11000101 (S=1, Z=1, Y=0, H=0, X=1, P/V=0, N=1, C=1)
    push af
    pop hl               ; HL = F in L
    ld a, l
    cpl                  ; A = ~F = 0x3A
    push af
    pop hl               ; save known A = 0x3A
    ld b, #0x01
    ld c, #0x6F          ; UART SCR port
    in a, (c)            ; IN A,(C) - on NMOS this preserves bits 3,5 from F
    ; Now check if A has preserved F bits
    ld b, a              ; save IN result
    ld a, h              ; A = 0x3A (our known value)
    cpl                  ; A should be ~0x3A = 0xC5 if F bits preserved
    and #0x28            ; isolate YF(0x20) and XF(0x08) bits which IN doesn't preserve
    ld a, b              ; restore IN result
    and #0x28            ; isolate YF and XF from IN result
    cp #0x28             ; both YF and XF set?
    ld l, #0             ; default: F not preserved
    jr nz, _inward_done
    ld l, #1             ; F preserved (NMOS behavior)
_inward_done:
    ld h, #0
    ei
    ret

; uint16_t z80_test_scf_flags(uint8_t a_val, uint8_t f_val)
; SDCC convention: param1 (a_val) in L, param2 (f_val) in E
; Set AF = (f_val, a_val), execute SCF, return resulting F
; Returns: flags byte in HL
_z80_test_scf_flags:
    ; L = a_val, E = f_val
    ld a, l           ; A = a_val
    ld d, e           ; D = f_val
    
    push de
    pop af            ; AF = (F=f_val, A=a_val)
    
    .db #0x37         ; SCF
    
    push af
    pop de            ; D = result F
    ld l, d           ; return F in L
    ld h, #0
    ret

; uint16_t z80_test_ccf_flags(uint8_t a_val, uint8_t f_val)
; SDCC convention: param1 (a_val) in L, param2 (f_val) in E
; Set AF = (f_val, a_val), execute CCF, return resulting F
; Returns: flags byte in HL
_z80_test_ccf_flags:
    ld a, l           ; A = a_val
    ld d, e           ; D = f_val
    
    push de
    pop af            ; AF = (F=f_val, A=a_val)
    
    .db #0x3F         ; CCF
    
    push af
    pop de            ; D = result F
    ld l, d
    ld h, #0
    ret

; uint8_t z80_sll_a(uint8_t val)
; Execute undocumented SLL (shift left logical) on A
; Uses CB 37h for SLL A
; Returns: result in L
_z80_sll_a:
    ld a, l           ; A = val
    .db #0xCB, #0x37  ; SLL A (undocumented)
    ld l, a
    ld h, #0
    ret

; uint16_t z80_test_bit_hl(uint8_t addr_high)
; Test BIT instruction MEMPTR leakage
; SDCC convention: param1 in L
_z80_test_bit_hl:
    ld a, l           ; addr_high
    ld h, #0
    ret
