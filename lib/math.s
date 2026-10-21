.module muzix_math

.globl __divuint
.globl __divsint
.globl __moduint
.globl __modsint
.globl __mulint

.area _CODE

; uint16_t __divuint(uint16_t dividend, uint16_t divisor)
; dividend in HL, divisor in DE
;
; Restoring division, sixteen iterations, every operand the full 16 bits.
;
; The running remainder is a 16-bit value in HL and the trial subtract
; compares all of HL against all of DE.  The previous version kept the
; remainder in A and compared it against E alone, and it refused any divisor
; whose high byte was nonzero by returning 0xffff - so 1000/400 divided by
; 144 and 1000/0x0100 returned 0xffff.  Neither happens now.
;
; The quotient accumulates in BC, one bit per iteration shifted in at the
; bottom as each dividend bit leaves the top, and is copied to HL and then to
; DE before returning.  SDCC's generated caller reads a 16-bit result out of
; DE - the same disagreement lib/libc.c records for __mulint - so both
; registers are written rather than one being picked and right by accident.
; A is the loop counter and is never touched by the compare or the subtract,
; which work on HL, DE and the carry flag.
__divuint:
    ld a,d
    or e
    jp z,divuint_zero
    ld b,h
    ld c,l              ; BC = dividend, its bits leave the top one at a time
    ld hl,#0            ; HL = running remainder
    ld a,#16            ; A = iteration counter (dec a leaves the carry alone)
    or a                ; carry = 0: the first quotient bit shifted in is 0
divuint_loop:
    rl c
    rl b                ; BC <<= 1; last iteration's quotient bit comes in at
                        ; the bottom and the dividend's top bit leaves in the
                        ; carry
    add hl,hl           ; HL = (HL << 1) | that dividend bit
    or a                ; clear the carry add hl,hl left behind
    sbc hl,de           ; trial: remainder -= divisor
    jr c,divuint_borrow ; borrow: the divisor did not fit, undo it
    scf                 ; it fit: this quotient bit is 1
    jr divuint_next
divuint_borrow:
    add hl,de           ; restore the remainder
    or a                ; this quotient bit is 0
divuint_next:
    dec a
    jr nz,divuint_loop
    ld h,b
    ld l,c              ; quotient to HL, ...
    ld d,h
    ld e,l              ; ... then to DE, where the caller reads it
    ret

divuint_zero:
    ld hl,#0xffff
    ld de,#0xffff
    ret

; int16_t __divsint(int16_t dividend, int16_t divisor)
; dividend in HL, divisor in DE
;
; Signs first, then __divuint on non-negative operands.  The quotient's sign
; is bit 7(H) ^ bit 7(D); it is pushed across the call because __divuint
; occupies every other register.
;
; The previous version derived that sign with two `rla`s and an XOR, then
; tested bit 7 of the result - which is bit 6 of (dividend ^ divisor), not
; bit 7.  Same-sign operands whose bit 6 differed were negated and
; different-sign operands whose bit 6 agreed were not, so 16576 / 2 came back
; as -8288.  `and #0x80` on the XOR of the two high bytes is the sign, and
; nothing else.
__divsint:
    ld a,d
    or e
    jp z,divsint_zero
    ld a,h
    xor d
    and #0x80           ; A bit 7 = sign(dividend) ^ sign(divisor)
    push af
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
    call __divuint      ; quotient in HL and DE
    pop af              ; sign back into the flags
    ret p               ; positive: both registers already hold it
    ; Negate: HL = -HL, then DE = HL
    xor a
    sub l
    ld l,a
    ld a,#0
    sbc a,h
    ld h,a
    ld d,h
    ld e,l
    ret

divsint_zero:
    ld hl,#0x7fff
    ld de,#0x7fff
    ret

; uint16_t __moduint(uint16_t dividend, uint16_t divisor)
; dividend in HL, divisor in DE
;
; The same loop as __divuint without the quotient bits: the remainder in HL
; is the answer.  It is a full 16-bit value and the trial subtract compares
; all of HL against all of DE.  The previous version kept the remainder in A
; and compared it against the divisor's LOW byte only, so a divisor of 400
; behaved as 144 and the remainder came back in A rather than in a register
; pair the caller could read.
;
; BC is the dividend and is discarded at the end; the carry that leaks into
; its low bit from the restore path is junk that never reaches the top within
; sixteen iterations, because the un-consumed dividend bits are always above
; it.
__moduint:
    ld a,d
    or e
    jp z,moduint_zero
    ld b,h
    ld c,l              ; BC = dividend
    ld hl,#0            ; HL = running remainder
    ld a,#16
moduint_loop:
    rl c
    rl b                ; dividend's top bit leaves in the carry
    add hl,hl           ; remainder = (remainder << 1) | that bit
    or a
    sbc hl,de           ; trial: remainder -= divisor
    jr nc,moduint_next  ; no borrow: keep the subtraction
    add hl,de           ; borrow: undo it
moduint_next:
    dec a
    jr nz,moduint_loop
    ld d,h
    ld e,l              ; result to DE as well as HL
    ret

moduint_zero:
    xor a
    ld h,a
    ld l,a
    ld d,a
    ld e,a
    ret

; int16_t __modsint(int16_t dividend, int16_t divisor)
; dividend in HL, divisor in DE
;
; The result takes the sign of the DIVIDEND, which is what `a % b` means in C
; and what the compiled caller expects: -7 % 2 is -1, not 1.  The divisor is
; made positive only so __moduint can run on non-negative operands; its own
; sign never reaches the answer.  The previous version dropped the
; dividend's sign entirely, so every negative operand produced a positive
; remainder.
__modsint:
    ld a,d
    or e
    jp z,modsint_zero
    ld a,h
    and #0x80           ; A bit 7 = sign of the dividend
    push af
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
    call __moduint      ; remainder in HL and DE
    pop af
    ret p               ; dividend was positive: both registers already hold it
    ; Dividend was negative: negate (a zero remainder stays zero)
    xor a
    sub l
    ld l,a
    ld a,#0
    sbc a,h
    ld h,a
    ld d,h
    ld e,l
    ret

modsint_zero:
    xor a
    ld h,a
    ld l,a
    ld d,a
    ld e,a
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
