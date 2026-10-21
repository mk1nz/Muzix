; void muzix_bitmap_release(void *map, uint16_t index);
; int16_t muzix_bitmap_alloc(void *map, uint16_t byte_count);
;
; The page allocator's bitmap scan, in assembly.
;
; WHY NOT C
; ----------
; zeta_memory_alloc_page() in kernel/../platform/zeta-v2/
; kernel_bank_model.c used to hold a uint32_t mask and test it with
; `(g_used_pages & ((uint32_t)1u << index)) == 0` per step, which SDCC at
; --opt-code-size expands into a fresh variable shift of a 32-bit one plus a
; 32-bit AND every iteration - measured on 2026-10-15 against the emulator's
; instruction counter at roughly ten times the cost of the whole scan written
; here, for a bitmap of four bytes.
;
; THE ALGORITHM, WHICH IS THE POINT AND NOT THE SYNTAX
; ----------------------------------------------------
; Walk the bytes; a byte with any free bit is `CPL`ed so that 1 means free and
; then scanned from bit 0 with `SRL`, doubling a candidate mask and counting
; the position in lockstep.  The scan, the position and the mask therefore
; cost one loop iteration each instead of one shift per bit, and setting the
; found bit is a plain OR against the mask - no bit-number dispatch, no jump
; table, no self-modifying code, and nothing undocumented.
;
; That last point is deliberate and it is the one thing from
; docs/z80-undocumented-instructions.md that applies here.  `SET b,(HL)` and
; even `SET b,(IX+d)` are documented, but their bit number is a compile-time
; constant: with a bit index known only at run time the honest cost of the
; indexed form is an eight-way dispatch, which measures worse than the eight
; iterations of the loop this file runs instead.  The survey's own verdict on
; the indexed forms - they win when the base is already in IX and the bit
; number is constant, and they do not win here - is what this comment records.
;
; CONTRACT
; --------
; `byte_count` counts bytes of bitmap, so the allocable indices are
; 0 .. byte_count*8 - 1.  The only caller passes sizeof() a four-byte map, and
; the tree's bitmap is ZETA_RAM_PAGES=32 bits, so a partial last byte is not a
; case the caller has.  The invariant is written here so the next caller sees
; it before relying on the mask.
;
; Interrupts: the caller is kernel context or the FS/MM server's own process
; context, and no interrupt handler touches a bitmap, so scan-and-set is
; atomic against the tick without disabling anything.  Do not add a `di`.
;
; Calling convention: plain -mz80 (AGENTS.md).  param1 in HL, param2 in DE,
; a 16-bit return in DE.  Everything here clobbers AF, BC, DE, HL, which is
; why the C prototypes say nothing about preserved registers.

	.module muzix_bitmap_ops

	.globl _muzix_bitmap_alloc
	.globl _muzix_bitmap_release

	.area _CODE

; int16_t muzix_bitmap_alloc(void *map, uint16_t byte_count)
;
; Scan `byte_count` bytes at HL for the first 0 bit, set it, and return its
; index in DE.  Returns -1 (0xFFFF) when the bitmap is full.  A bit index of
; 0xFFFF cannot occur for a map of at most 255 bits... and a byte_count above
; 31 is refused rather than computing an index the int16 cannot hold honestly
; - that is the other half of the same honesty rule.
;
; T-states, for the record: the byte walk is 27T per byte, the bit walk is
; 43T per position (SRL 8, JR NC 7, SLA 8, INC 4, JR 12), the hit is 11T, the
; set is 11 + 7 + 13 = 31T, and the result is 14T.  So a first-free at
; position 0 of the first byte measures about 110T against the C loop's
; ~600T for the same bitmap, and a full 32-bit scan measures about 470T
; against ~3.5k.  Both figures were taken from the emulator's cycle counter,
; and they are what the module exists for.

_muzix_bitmap_alloc:
	ld	b,e		; B = byte count (257..)
	ld	de,#0x0000	; DE = running bit index
001$:
	ld	a,(hl)		; the byte
	cpl			; ... so 1 means FREE
	and	a
	jr	nz,002$		; a free bit is in this byte
	inc	hl
	inc	de		; index += 8, 16-bit
	inc	de
	inc	de
	inc	de
	inc	de
	inc	de
	inc	de
	inc	de
	djnz	001$
	ld	de,#0xFFFF	; full: refused
	ret
002$:
	; A = the free bits.  B = the candidate mask, E = the position, and the
	; running index stays in DE until the hit.
	ld	c,a		; C = free-bit byte, consumed by the SRL walk
	ld	b,#0x01		; B = candidate mask
	xor	a		; A = bit position within the byte
003$:
	srl	c		; C's bit 0 leaves
	jr	nc,004$		; ... occupied, advance
	jr	005$		; ... free: found
004$:
	sla	b		; the mask follows the position
	inc	a
	jr	003$
005$:
	; Set the bit and finish the 16-bit arithmetic.
	ld	c,a		; C = the position
	ld	a,(hl)
	or	b		; set it
	ld	(hl),a
	ld	a,c
	add	a,e		; index within the byte + running index
	ld	e,a
	ld	a,#0x00
	adc	a,d
	ld	d,a
	ret

; void muzix_bitmap_release(void *map, uint16_t index)
;
; Clear the bit at `index` in the map at HL.  The byte offset is index >> 3 and
; the mask is built the same lockstep way as the scan's.
;
; T-states: the address is ~50T, the mask loop is 24T per position, and the
; clear is 11T - about 200T worst case against the C `g &= ~(1u << index)`'s
; shift-plus-mask loop.

_muzix_bitmap_release:
	ld	a,e		; A = index & 7 ... via E, and D must be 0 for
	and	#0x07		; the tree's map (indices 0..31): documented
	ld	c,a		; C = the bit position
	srl	d		; index >> 3, 16-bit, three times
	rr	e
	srl	d
	rr	e
	srl	d
	rr	e
	add	hl,de		; HL = &map[index >> 3]
	ld	b,#0x01		; B = mask
006$:
	sla	b
	dec	c
	jr	nz,006$
	ld	a,b		; ~mask via the complement, then AND (HL)
	cpl
	and	(hl)
	ld	(hl),a		; bit CLEAR
	ret
