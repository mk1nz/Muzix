.module muzix_sdz80_runtime

.globl _muzix_sdz80_call_hl
.globl _muzix_sdz80_call_iy
.globl ___sdcc_call_hl
.globl ___sdcc_call_iy

.area _CODE

; Custom SDCC runtime call stubs for Muzix.
;
; The SDCC compiler emits indirect calls through ___sdcc_call_hl (for regular
; function-pointer calls) and ___sdcc_call_iy (for __sdcccall(1) calls).
; A peephole file (muzix_sdccall.peep) rewrites those references to call
; our _muzix_sdz80_call_hl / _muzix_sdz80_call_iy symbols instead, so the
; compiler's z80.lib runtime is never linked.
;
; Both are single-instruction implementations: the CALL that invoked us
; already pushed the return address, so a JP through the register transfers
; control to the target and the target's RET returns to our caller.

; _muzix_sdz80_call_hl — indirect call via HL
;   HL = target function address
_muzix_sdz80_call_hl:
    jp (hl)

; _muzix_sdz80_call_iy — indirect call via IY
;   IY = target function address
_muzix_sdz80_call_iy:
    jp (iy)

; The real SDCC names for the same two stubs.
;
; These used to be reached through a peephole rewrite in muzix_sdccall.peep,
; which is how the compiler was told to call them at all - because --peep-file
; REPLACES SDCC's entire default peephole rule set, not just adds to it.  The
; kernel therefore ran with two rules and none of SDCC's own, and the result was
; a long series of failures that all had the same shape: a value left in a
; register across a call, or an indirect call that reached the wrong place,
; depending on nothing more than how big an unrelated module happened to be.
; Supplying the symbols directly means the default rules can stay in place.
; See the note in the Makefile.
___sdcc_call_hl:
    jp (hl)

___sdcc_call_iy:
    jp (iy)

; ___sdcc_enter_ix — function-frame entry for --stack-auto.
;
; Building with --opt-code-size makes SDCC factor the start of every function
; that has automatic variables out into a call to this helper instead of
; inlining it, which is most of the size saving. It is a z80.lib symbol, and
; z80.lib is deliberately never linked here (see above), so the kernel
; provides its own copy. This is a verbatim transcription of
; crtenter.s from the SDCC 4.5.0 z80n runtime, which is LGPL-2.1-or-later;
; the algorithm is five instructions and cannot usefully be reworded.
;
; The caller has already executed `call ___sdcc_enter_ix`, so the return
; address is on top of the stack. Swap it for the caller's IX, point IX at the
; frame, and return.
    .globl ___sdcc_enter_ix
___sdcc_enter_ix:
    pop     hl          ; return address
    push    ix          ; save the caller's frame pointer
    ld      ix, #0
    add     ix, sp      ; IX = the new stack frame
    jp      (hl)        ; and return
