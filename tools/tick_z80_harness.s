; harness.s - drive the real platform/zeta-v2/tick.s, ONE case per run.
;
; Linked at _CODE = 0x8000, _DATA = 0x9000, so window 0 (0x0000-0x3FFF) is
; empty and every read the Z80 makes from it is a window-0 DATA access the
; runner can see, rather than an instruction fetch.  That is what turns "did
; the reader walk off the end of the counter" into a measurement instead of an
; argument.
;
; It lives in tools/ rather than beside tick.s because it is the rig, not part
; of the kernel: nothing here is linked into the ROM, and the kernel's module
; list is explicit so a second .s in platform/zeta-v2/ would raise the question
; of what it is for.
;
; No loops and no walking pointers, on purpose.  Every address below is an
; absolute literal, so what the harness wrote is exactly what this source says
; it wrote.  An earlier version walked two pointers eight times and the numbers
; it produced could not be reconciled with a single-stepped trace of the same
; instructions; one case per run removes every question of the kind at the cost
; of a few more emulator invocations, which are microseconds each.
;
; The runner (tools/tick_z80_run.c) supplies the counter value, the deadline and
; the accounting case in _DATA, runs this once, and reads the answers back out
; of _DATA.  Nothing here computes an expected value.
;
; Absolute addressing only, because this sdasz80 has no index displacement:
; `ld a,(hl+5)` does not assemble here.

	.module tkh1
	.globl _start
	.globl _muzix_tick_now
	.globl _muzix_tick_isr
	.globl _muzix_tick_expired
	.globl _muzix_tick_add_process_time
	.globl _muzix_tick_stamp_store
	.globl _muzix_tick_count

	.globl h_caseval
	.globl h_deadline
	.globl h_aptcnt
	.globl h_aptstamp
	.globl h_aptnow
	.globl h_resnow
	.globl h_resexp
	.globl h_resapt
	.globl h_resstamp
	.globl h_resstampout
	.globl h_resisr

	.area _CODE

; HL -> 16-bit value: low byte to the counter label, high byte to label+1.
; This is tick.s's own rule - the counter is a `.dw` and sdasz80 puts the LOW
; byte at the label - and it is written out here rather than shared, because a
; harness that got the byte order wrong would agree with a tick.s that got it
; wrong and the whole rig would pass on a broken counter.
setc:
	ld	a,(hl)
	ld	(_muzix_tick_count),a
	inc	hl
	ld	a,(hl)
	ld	(_muzix_tick_count+1),a
	ret

_start:
	di
	ld	sp,#0xFF00

; ---- muzix_tick_now: the 16-bit return is in DE (plain -mz80) -------------
	ld	hl,#h_caseval
	call	setc
	call	_muzix_tick_now
	ld	a,e
	ld	(h_resnow),a
	ld	a,d
	ld	(h_resnow+1),a

; ---- muzix_tick_expired: deadline in HL, result in A ---------------------
	ld	hl,#h_caseval
	call	setc
	ld	a,(h_deadline)
	ld	l,a
	ld	a,(h_deadline+1)
	ld	h,a
	call	_muzix_tick_expired
	ld	(h_resexp),a

; ---- muzix_tick_add_process_time: counter in HL, stamp in DE -------------
	ld	a,(h_aptcnt)
	ld	(_cell),a
	ld	a,(h_aptcnt+1)
	ld	(_cell+1),a
	ld	a,(h_aptcnt+2)
	ld	(_cell+2),a
	ld	a,(h_aptcnt+3)
	ld	(_cell+3),a
	ld	a,(h_aptstamp)
	ld	(_stamp),a
	ld	a,(h_aptstamp+1)
	ld	(_stamp+1),a
	ld	hl,#h_aptnow
	call	setc
	ld	hl,#_cell
	ld	de,#_stamp
	call	_muzix_tick_add_process_time
	ld	a,(_cell)
	ld	(h_resapt),a
	ld	a,(_cell+1)
	ld	(h_resapt+1),a
	ld	a,(_cell+2)
	ld	(h_resapt+2),a
	ld	a,(_cell+3)
	ld	(h_resapt+3),a
	ld	a,(_stamp)
	ld	(h_resstamp),a
	ld	a,(_stamp+1)
	ld	(h_resstamp+1),a

; ---- muzix_tick_stamp_store, which the restamp fix depends on -------------
	ld	a,#0x34
	ld	(_muzix_tick_count),a
	ld	a,#0x12
	ld	(_muzix_tick_count+1),a
	ld	hl,#_stamp
	call	_muzix_tick_stamp_store
	ld	a,(_stamp)
	ld	(h_resstampout),a
	ld	a,(_stamp+1)
	ld	(h_resstampout+1),a

; ---- the ISR: three ticks from 0x00FE must give 0x0101 ---------------------
; The handler's byte order is the other half of what the readers depend on: it
; increments the low byte first and the high byte last, so a reader that takes
; the high byte first is off by at most one tick.  If this ever changes and the
; readers are not changed with it, the ordering argument above the three
; routines stops being true and nothing else in the tree would notice.
	ld	a,#0xFE
	ld	(_muzix_tick_count),a
	ld	a,#0x00
	ld	(_muzix_tick_count+1),a
	call	_muzix_tick_isr
	call	_muzix_tick_isr
	call	_muzix_tick_isr
	ld	a,(_muzix_tick_count)
	ld	(h_resisr),a
	ld	a,(_muzix_tick_count+1)
	ld	(h_resisr+1),a

halt:	halt
	jr	halt

	.area _DATA
h_caseval:	.ds 2
h_deadline:	.ds 2
h_aptcnt:	.ds 4
h_aptstamp:	.ds 2
h_aptnow:	.ds 2
h_resnow:	.ds 2
h_resexp:	.ds 1
h_resapt:	.ds 4
h_resstamp:	.ds 2
h_resstampout:	.ds 2
h_resisr:	.ds 2
_cell:	.ds 4
_stamp:	.ds 2