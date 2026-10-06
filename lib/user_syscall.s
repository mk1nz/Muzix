.module muzix_user_syscall

; SDCC 4.5 emits `call __syscall` for the SYSCALL() macro, because _syscall is
; declared in a header and its body lives in this file.  The old __syscall was
; five instructions long -- it moved the HIGH word of num into A and jumped
; straight at 0x0030 without ever building the argument frame -- so every one of
; the 37 call sites in lib/syscall.c dispatched on syscall number 0.
.globl _syscall
.globl __syscall
.globl _muzix_syscall_void
.globl _syscall_result

.area _CODE

; int32_t __syscall(int32_t num, int32_t arg1, int32_t arg2, int32_t arg3)
;
; sdcccall(1) with int32_t parameters, as SDCC 4.5 actually lays them out:
;
;   num  -> DE = low word, HL = high word
;   arg1 -> stack at  2(SP)   (low word first, high word second)
;   arg2 -> stack at  6(SP)
;   arg3 -> stack at 10(SP)
;   32-bit return in DE:HL
;
; Kernel ABI (platform/zeta-v2/syscall_entry.s):
;
;   syscall number in A (low byte)
;   three 32-bit arguments as six words at SP+2 .. SP+13
;   result in HL
;   every other register clobbered, SP restored to its value at the call
;
; The trap cannot preserve the caller's registers, so this stub does not try.
; The arguments the kernel needs are on the process stack, which is in window 1,
; and the handler must run with the kernel map installed - the entry copies the
; arguments up onto the kernel stack before switching maps, and clobbering
; everything else costs nothing.  All this stub has to do is lay the six
; argument words out where the entry expects them, take the number, and sign
; extend the result on the way back.
__syscall:
    ; Save the index registers across the trap.  sdcccall(1) makes IX and IY
    ; callee-saved: SDCC uses them as the frame pointer of every function with
    ; an automatic frame (`push ix / ld ix,#0 / add ix,sp ... ld sp,ix / pop ix`)
    ; and restores them itself, so a C caller may keep a live frame pointer in
    ; them across a call.  The trap cannot honour that - it clobbers every
    ; register but DE - so the stub has to.
    ;
    ; This is not a theoretical hole.  A syscall is the only place a process is
    ; ever suspended: on SYS_FORK the trap switches to the child, and when the
    ; child exits the trap switches back with `ld sp,(_g_resume_sp) / ld de,... /
    ; ld hl,... / jp (hl)`, which restores the stack pointer, the result and
    ; nothing else.  The parent came back inside this stub with IX and IY
    ; holding whatever the kernel's last C frame left in them.  shell.c's
    ; execute_external() is one `ld sp,ix / pop ix / ret` away from the end of
    ; its own frame, so it used a kernel stack address as a stack pointer, and
    ; the `ret` that followed popped a kernel return address off the kernel
    ; stack: the shell jumped to 0x2103, ran a fragment of the syscall
    ; handler, wandered through the kernel stack into the middle of the ROM
    ; boot stub at 0x003B, and the stub's `ldir`/`jp 0x0098` re-entered
    ; kernel_main.
    ; That is the reboot that followed `ls`.
    push    ix
    push    iy

    ; num's low word is in DE, and nothing below touches DE.
    ld      a, e

    ; Move the caller's three 32-bit arguments (at SP+2 .. SP+13) to the top of
    ; the stack.  Twenty bytes are reserved rather than twelve because `call
    ; 0x0030` then pushes two more, and the four bytes just saved above need
    ; somewhere to sit: the trap sees the return address at its own SP and the
    ; arguments at SP+2, which is this new SP.  Reserving twelve put the
    ; arguments four bytes too high, and the trap's epilogue restored a stack
    ; pointer two bytes below the return address, so the final `ret` popped the
    ; first argument word as a jump target and the process ran off into the boot
    ; stub.
    ld      hl, #-20
    add     hl, sp
    ld      sp, hl
    ex      de, hl              ; DE = new top of stack = destination
    ld      hl, #26
    add     hl, sp               ; HL = old SP + 2 = the caller's arguments
    ld      bc, #12
    ldir                         ; reads (HL), writes (DE)

    call    0x0030

    ; A 32-bit return leaves the low word in DE and the high word in HL.  The
    ; 16-bit result arrives in HL, so move it down and sign extend.
    ;
    ; This was briefly written the other way round - result in HL, extension in
    ; DE - on the theory that the two plausible readings of a 32-bit return
    ; disagree about which word is low.  They do disagree, but only for a
    ; function with real 32-bit arithmetic to return: measured here, a plain
    ; `-mz80 --opt-code-size` u32-returning function ends with
    ; `ld de,(lo) / ld hl,(hi)`, and the userspace - built the same way - reads
    ; sys_read's result as DE first.  With the result in HL the low word read as
    ; zero, so every syscall reported 0, getchar() saw something that was not 1,
    ; returned -1, and the shell read its own prompt as end of file.
    ; Publish it.  This is the value the C wrappers actually return - see the
    ; note on SYSCALL in lib/syscall.h - so it must not depend on a register
    ; convention that this build applies inconsistently.
    ld      (_syscall_result), de
    ld      a, d
    rlca
    rlca
    rlca
    rlca                         ; bit 7 of A = bit 15 of the result
    sbc     a, a                 ; 0x00 or 0xFF
    ld      e, a
    ld      d, a
    ld      (_syscall_result + 2), de
    ld      h, a
    ld      l, a                 ; DE = low word, HL = high word

    ; Drop the twenty reserved bytes and the two saved index registers, and
    ; return to the caller.  The result is already in DE:HL, so the SP
    ; arithmetic can use HL.  The pops must come after the SP fixup: this
    ; instruction is the resume point for a process the kernel switched back
    ; into, and at that moment SP is the value the trap left behind - the new
    ; SP - not the value the syscall was entered with.
    ld      hl, #20
    add     hl, sp
    ld      sp, hl
    pop     iy
    pop     ix
    ret

.area _DATA
; The trap's result, as two 16-bit words: low first, then the sign extension.
_syscall_result:
    .dw #0
    .dw #0

.area _CODE
; Both spellings share one body.  _syscall is what the header declares and
; __syscall is what SDCC actually emits, so they have to agree.
_syscall:
    jp      __syscall

; The void form, which is what SYSCALL() uses.  The trap itself is already
; self-contained - it reserves sixteen bytes, fills them, calls 0x0030 and
; discards them again - so it has nothing to hand back in registers, and saying
; so here is what keeps the compiler from having to guess a return convention.
;
; It used to go through a C shim, _syscall_dispatch, that called the trap and
; then returned syscall_result.  SDCC compiled that shim without a stack frame
; and emitted the epilogue `ld hl,#12 / add hl,sp / ld sp,hl / pop af / pop af
; / ret`: twelve bytes discarded plus four popped, against twelve bytes actually
; pushed for three stack parameters, so its `ret` came back four bytes above the
; real return address.  Every syscall result was therefore lost on the way to
; the caller - which is why the shell echoed what was typed, took one character,
; and then read the rest of the line as end of file.
_muzix_syscall_void:
    jp      __syscall
