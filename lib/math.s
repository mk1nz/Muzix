.module muzix_math

.globl __divuint
.globl __divsint
.globl __moduint
.globl __modsint
.globl __mulint

.area _CODE

; uint16_t __divuint(uint16_t dividend, uint16_t divisor)
; dividend in HL, divisor in DE
__divuint:
    ld a,d
    or e
    jp z,divuint_zero
    ; HL = dividend, DE = divisor
    ; Result in HL
    xor a
    ld b,#16
divuint_loop:
    rl l
    rl h
    rla
    cp e
    jp c,divuint_next
    sub e
    inc l
divuint_next:
    djnz divuint_loop
    ret

divuint_zero:
    ld hl,#0xffff
    ret

; int16_t __divsint(int16_t dividend, int16_t divisor)
; dividend in HL, divisor in DE
__divsint:
    ld a,d
    or e
    jp z,divsint_zero
    ; Handle signs
    ld a,h
    rla
    ld c,a
    ld a,d
    rla
    xor c
    ld c,a
    ; Make dividend positive
    bit 7,h
    jr z,divsint_div_pos
    xor a
    sub l
    ld l,a
    ld a,#0
    sbc a,h
    ld h,a
divsint_div_pos:
    ; Make divisor positive
    bit 7,d
    jr z,divsint_call_divuint
    xor a
    sub e
    ld e,a
    ld a,#0
    sbc a,d
    ld d,a
divsint_call_divuint:
    push bc
    call __divuint
    pop bc
    bit 7,c
    ret z
    ; Negate result
    xor a
    sub l
    ld l,a
    ld a,#0
    sbc a,h
    ld h,a
    ret

divsint_zero:
    ld hl,#0x7fff
    ret

; uint16_t __moduint(uint16_t dividend, uint16_t divisor)
; dividend in HL, divisor in DE
__moduint:
    ld a,d
    or e
    jp z,moduint_zero
    ; HL = dividend, DE = divisor
    ; Result in HL
    xor a
    ld b,#16
moduint_loop:
    rl l
    rl h
    rla
    cp e
    jp c,moduint_next
    sub e
moduint_next:
    djnz moduint_loop
    ret

moduint_zero:
    ex de,hl
    ret

; int16_t __modsint(int16_t dividend, int16_t divisor)
; dividend in HL, divisor in DE
__modsint:
    ld a,d
    or e
    jp z,modsint_zero
    ; Handle dividend sign
    bit 7,h
    jr z,modsint_div_pos
    xor a
    sub l
    ld l,a
    ld a,#0
    sbc a,h
    ld h,a
modsint_div_pos:
    ; Make divisor positive
    bit 7,d
    jr z,modsint_call_moduint
    xor a
    sub e
    ld e,a
    ld a,#0
    sbc a,d
    ld d,a
modsint_call_moduint:
    call __moduint
    ret

modsint_zero:
    ex de,hl
    ret

; uint16_t __mulint(uint16_t a, uint16_t b)
; a in HL, b in DE.  The product is left in BOTH HL and DE.
;
; Which of the two the back end reads is not something this file can settle, and
; the two answers in the tree disagreed: this routine returned it in HL, while
; generated code reads it out of D and E - `ld -8(ix),e / ld -7(ix),d`, low byte
; first, immediately after the call.  Both are written rather than one being
; picked and being right by accident; the product is already in HL, and copying
; it into DE costs four instructions in a userspace library.
;
; The zero-operand early exits are a separate and unambiguous bug either way.
; `ld a,h / or l / ret z` returns with DE still holding the *second* operand, so
; a multiply with a zero first operand came back as the second operand: 0 * 100
; returned 100.  That is not a rare path.  Multiplying a byte out of a 32-bit
; value means multiplying by zero for every byte above the last, and a load
; average's fixed-point arithmetic does exactly that, because a byte of a
; multiplier is zero wherever the product has a byte boundary.
__mulint:
    ld a,d
    or e
    jr nz, mulint_go
    xor a
    ld d,a
    ld e,a
    ret

mulint_go:
    ld a,h
    or l
    jr nz, mulint_16
    xor a
    ld d,a
    ld e,a
    ret

mulint_16:
    ld b,h
    ld c,l
    ld h,#0
    ld l,#0
    ld a,#16
mulint_loop:
    rl c
    rl b
    jr nc,mulint_next
    add hl,de
    jp c,mulint_out
mulint_next:
    ex de,hl
    add hl,hl
    ex de,hl
    dec a
    jr nz,mulint_loop

mulint_out:
    ld a,l
    ld e,a
    ld a,h
    ld d,a
    ret