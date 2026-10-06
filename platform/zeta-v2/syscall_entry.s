.module muzix_syscall_entry

.globl _muzix_z80_syscall_entry
.globl _muzix_z80_dispatch_frame
.globl _g_zeta_state_ptr
.globl _muzix_resume_pc
.globl _muzix_resume_sp
.globl _muzix_resume_ret
.globl _muzix_switch_pending
.globl _muzix_zeta_resume_from
; _muzix_fork_stack_scratch is not named here any more: this file used to ldir
; the active stack into it, and now it only measures the stack, so the buffer is
; reached from the fork handler in kernel/syscalls.c and the definition in
; kernel/kernel_main.c is the only one.
.globl _muzix_fork_stack_len

.area _CODE

; Userspace syscall trap.
;
; ABI, all of it decided by what the window layout allows:
;
;   * A            carries the syscall number
;   * SP+2..SP+13  the three 32-bit arguments, as six words, pushed by the
;                  userspace stub
;   * return       16-bit result in HL
;
; Every other register is clobbered, and the userspace stub is written to
; expect that.  That is not a simplification, it is forced: the argument words
; live on the *process* stack, which is in window 1, and the handler has to run
; with the kernel map installed - at which point window 1 holds kernel page
; 0x21 and the process's own stack is no longer addressable.  An earlier
; version left the arguments where the stub had pushed them and called the
; handler after switching maps, so the handler read the frame out of the
; middle of the kernel's code.  It read plausible values often enough to look
; like a working syscall and wrong ones often enough to reboot the machine into
; the boot stub.  The arguments are therefore copied onto the kernel stack
; *before* the map changes, while the process stack is still mapped.
;
; Nothing is pushed or popped around the handler: the trap clobbers the user
; registers, so saving and restoring them bought nothing.  Three small cells on
; the kernel stack carry what has to outlive the call into C - the user stack
; pointer, the arguments and the syscall number - and they sit above the
; handler's SP so nothing it pushes can reach them.

; Fixed kernel-stack cells.  These are addresses, not _DATA: the entry runs
; with the kernel map installed and has to be able to trust them, and a _DATA
; cell is one unlinked symbol away from being shared with a 256-byte staging
; buffer in another module.
;
; SYSCALL_SP has to sit *below* the number cell, not level with it.  The
; `call _muzix_z80_dispatch_frame` below pushes its return address at
; SYSCALL_SP-2, so with the two equal the call wrote 0xD1 - the low byte of the
; return address - over the syscall number, and the handler ran whatever
; instruction that address happened to end in.  Two bytes of gap is the whole
; difference between working and not.
SYSCALL_USER_SP  = 0xFFFC        ; where the `ret` has to land
SYSCALL_ARGS     = 0xFFF0        ; six words, copied off the process stack
SYSCALL_NUMBER   = 0xFFEE        ; low byte of num, plus a pad byte
SYSCALL_SP       = 0xFFEC        ; handler stack pointer; the call lands at 0xFFEA

; On entry to the trap, SP points at the return address that `call 0x0030`
; pushed, so the six argument words the stub left for it are at SP+2..SP+13.
; Both offsets matter: an earlier version read them from SP+4 and restored SP
; to the stub's pre-call value, which is two bytes below the return address, so
; the final `ret` popped the first argument word as a jump target and the process
; landed in the middle of the boot stub.

_muzix_z80_syscall_entry:
    ; Park the user stack pointer, so the `ret` below lands in the stub.
    ld      hl, #0
    add     hl, sp
    ld      (SYSCALL_USER_SP), hl

    ; Capture the resume point while window 1 still holds the process's page.
    ;
    ; SP points at the return address `call 0x0030` pushed, so the word at [SP]
    ; is where this process continues when the trap returns, and SP itself is
    ; the stack pointer it has to continue on.  Together they are the whole of a
    ; context: a process that is switched out has to come back to this
    ; instruction on this stack, not to a fresh entry point.
    ;
    ; This has to happen *here*, before the kernel map is installed.  After the
    ; switch, window 1 maps kernel page 0x21 and the word at [SP] is part of the
    ; kernel's own stack - reading it then would return the kernel's address and
    ; resume the process in the middle of the kernel.

    ; Park the syscall number.  This has to happen before anything else touches
    ; A: the number arrives only in A, and the resume capture below reads memory
    ; into A. Parking it second made every syscall dispatch on a byte of the
    ; return address instead of its own number, and the shell died on its first
    ; putchar.
    ld      hl, #SYSCALL_NUMBER
    ld      (hl), a
    ld      a, (hl)

    ; Capture the resume point while window 1 still holds the process's page.
    ;
    ; SP points at the return address `call 0x0030` pushed, so the word at [SP]
    ; is where this process continues when the trap returns, and SP itself is
    ; the stack pointer it has to continue on.  Together they are the whole of a
    ; context: a process that is switched out has to come back to this
    ; instruction on this stack, not to a fresh entry point.
    ;
    ; This has to happen *here*, before the kernel map is installed.  After the
    ; switch, window 1 maps kernel page 0x21 and the word at [SP] is part of the
    ; kernel's own stack - reading it then would return the kernel's address and
    ; resume the process in the middle of the kernel.
    ;
    ; Moved through HL rather than DE: sdasz80 has no `ld (nn),de`, so the store
    ; has to be a 16-bit HL store.
    ;
    ; The resume stack pointer is SP+2, not SP.  On the ordinary return path the
    ; trap ends with `ld sp,(SYSCALL_USER_SP) / ret`, and that `ret` pops the
    ; return address, leaving SP two bytes above where the trap found it.  The
    ; context-switch path does not pop - it jumps - so it has to apply that same
    ; two bytes itself.
    ;
    ; It did not, and the consequence is precise.  The syscall stub unwinds with
    ; `ld hl,#16 / add hl,sp / ld sp,hl / ret`, so from two bytes too low it
    ; lands on SP+16 pointing one word before the return address, and `ret`
    ; pops that neighbouring word instead.  Measured at the jump: HL=0x7FBE,
    ; SP=0x7FC0, PC=0x0000 - the child read a zero where the return address
    ; should have been, and went to the boot stub.
    ;
    ; The program counter is unaffected: [SP] is the return address itself, and
    ; that is the address the stub resumes at either way.
    ld      hl, (SYSCALL_USER_SP)
    ld      a, (hl)
    ld      c, a
    inc     hl
    ld      a, (hl)
    ld      b, a
    ld      h, b
    ld      l, c
    ld      (_muzix_resume_pc), hl
    ; Reload the stack pointer before adding the two bytes.  The inc's below
    ; operate on whatever is in HL, and at this point that is the program
    ; counter, not the stack pointer - so they produced PC+2 and stored it as
    ; the resume stack pointer.  The child then resumed with its stack pointer
    ; inside the text window, and the stub's `ret` popped a word of code.  Caught
    ; by watching the jump: SP and HL were the same value at kernel_main's
    ; entry.
    ld      hl, (SYSCALL_USER_SP)
    inc     hl
    inc     hl
    ld      (_muzix_resume_sp), hl

    ; Measure the active stack, for fork only, while window 1 still holds the
    ; process's own page.
    ;
    ; This is the only point on the whole syscall path where the parent's stack
    ; is still mapped. Once zeta_kernel_enter installs the kernel map, window 1
    ; is kernel page 0x21 and the parent's frames are simply gone, so the handler
    ; cannot copy them afterwards - it can only be told how deep they were.
    ;
    ; So the length is all that crosses, and the bytes cross later, through the
    ; memory manager: the fork handler reads them back out of the parent's page
    ; with the same primitive every other user copy uses, in
    ; MUZIX_ZETA_COPY_STAGING-sized pieces, into muzix_fork_stack_scratch, and
    ; on to the child.  The buffer is therefore one staging buffer rather than a
    ; whole-stack buffer, which is what lets it be 256 bytes instead of 1024 -
    ; 768 bytes of _DATA, which is what the process table's command names are
    ; paid for out of.
    ;
    ; This used to `ldir` the whole active stack into that buffer here. It had
    ; to: the buffer was sized to hold the lot, and 1024 bytes of _DATA was the
    ; price.  It cannot any more, and the reason it is safe to shrink is exactly
    ; the reason the copy is now two-sided - the handler below can reach the
    ; parent's page through the MM, so no single buffer ever has to hold more
    ; than one chunk.
    ;
    ; Without the copy at all the child runs on the parent's stack and overwrites
    ; the frames the parent is suspended in, so the parent's later resume jumps
    ; through whatever the child left behind and lands in the boot stub.
    ;
    ; The used stack runs from the stack pointer to the top of window 1, which
    ; for a program whose stack is nearly empty is a very short measurement. The
    ; cap below is not about the buffer any more - a deep stack is copied in as
    ; many chunks as it takes. It is about a stack pointer that is not in window
    ; 1 at all: 0x8000 - SP is then a large number rather than a small one, and
    ; without a bound the handler would be asked to copy 30 KB of a window it
    ; does not have.  Refusing is not a regression: the handler falls back to
    ; sharing the parent's stack, which is what every implementation did before
    ; any of this existed.
    ;
    ; The number has to be re-read from the cell, not taken from A: the resume
    ; capture above clobbers A twice, and this block used to `ld a,(hl)` with
    ; HL left pointing wherever the capture finished - so it compared whatever
    ; that byte was against 1, the comparison failed, and the copy was skipped
    ; for every fork. The parent and child then shared a stack, which is the
    ; bug this exists to remove.
    ld      hl, #SYSCALL_NUMBER
    ld      a, (hl)
    cp      #1                       ; SYS_FORK
    jr      NZ, fork_stack_done
    ld      hl, #0
    ld      (_muzix_fork_stack_len), hl

    ; Source and length are both the *resume* stack pointer, not the one the
    ; trap was entered on.
    ;
    ; The child does not resume where the `call 0x0030` sits: the kernel copies
    ; SP + 2 into _muzix_resume_sp, because the ordinary return path's `ret`
    ; pops the return address and the context-switch path jumps instead and has
    ; to apply the same two bytes itself. So the child comes up on SP + 2, and
    ; everything it will ever push or pop is at SP + 2 and above. Measuring from
    ; SP would measure two bytes the child cannot reach.
    ;
    ; 0x8000 - (SP+2), so the measurement ends exactly at the top of the window:
    ; no chunk of it can run past 0x7FFF, which is what the copy primitive
    ; refuses. An earlier version ended two bytes short of the top and the
    ; handler handed the primitive a length that overran the window by two, so
    ; the copy came back -1, the child's own page was never given to it, the
    ; child kept the parent's window 1, and it ran `ls` straight over the frames
    ; its parent was suspended in.
    ld      de, (SYSCALL_USER_SP)
    inc     de
    inc     de
    ld      hl, #0x8000
    or      a
    sbc     hl, de                   ; length = 0x8000 - resume stack pointer
    ld      a, h
    or      l
    jr      Z, fork_stack_done       ; nothing in use
    ld      a, h
    cp      #0x04                    ; refuse a stack pointer outside window 1
    jr      C, fork_stack_measure
    jr      fork_stack_done
fork_stack_measure:
    ld      (_muzix_fork_stack_len), hl
fork_stack_done:

    ; Copy the six argument words off the process stack, while window 1 still
    ; holds the process's page.
    ld      hl, #2
    add     hl, sp
    ld      de, #SYSCALL_ARGS
    ld      bc, #12
    ldir

    ld      sp, #SYSCALL_SP      ; kernel stack, below every cell above

    ld      hl, #_g_zeta_state_ptr
    ld      a, (hl)
    inc     hl
    ld      h, (hl)
    ld      l, a

    ; sdcccall(1): param1 in HL, param2 in DE.  DE is the argument block, in
    ; kernel memory, which stays mapped for the whole handler.
    ld      de, #SYSCALL_NUMBER
    call    _muzix_z80_dispatch_frame

    ; The result stays in DE.  muzix_z80_dispatch_frame is compiled with the
    ; project's replacement peephole set (--peep-file replaces SDCC's defaults
    ; wholesale, which is also why the indirect call here is a direct `call`),
    ; and under it the 16-bit return value is left in DE rather than the HL that
    ; stock SDCC would use.  lib/user_syscall.s publishes the return by reading
    ; DE, so DE is where it has to be on exit from this trap.
    ;
    ; There used to be an `ex de, hl` here, to put the result in HL on the
    ; theory that 16-bit returns live there.  It did the opposite of what was
    ; wanted: the swap moved the result into HL and moved HL's old contents -
    ; a _DATA address - into DE, which is the register the user stub reads.  So
    ; every syscall handed userspace that _DATA address instead of its result.
    ; Writes never showed it, because putchar ignores the result.
    ;
    ; Reads did, and it looked like a memory bug rather than a return-value bug.
    ; sys_read never reported 1, and lib/libc.c's getchar() maps any value other
    ; than 1 to end of file, so the shell printed its prompt and quit at once.
    ; That is the same symptom as a copy into the process failing, and the two
    ; were chased a long way apart before the data path was instrumented: the
    ; kernel's copy was verified correct by reading each byte back with the
    ; process map still installed, and a getchar() that ignored the return value
    ; entirely made the shell work perfectly.  The copy was never broken; only
    ; the number describing how many bytes it moved never arrived.
    ;
    ; If the peephole set is ever changed and 16-bit returns move back to HL,
    ; fix the two files together: this trap and the register that
    ; lib/user_syscall.s stores.  They have to agree, and nothing in the build
    ; checks it.

    ; A pending context switch overrides the ordinary return.
    ;
    ; The handler has published the target's resume point and map in
    ; _muzix_resume_* / _muzix_resume_banks and set the flag.  The switch itself
    ; is done by window-0 assembly, because the map is about to change and
    ; anything still executing in window 1 would be reading a page that is no
    ; longer there.  It does not come back.
    ;
    ; Clearing the flag first means a switch that is refused still leaves the
    ; trap in a state where the next syscall returns normally, rather than
    ; re-attempting the same switch forever.
    ld      hl, (_muzix_switch_pending)
    ld      a, l
    or      h
    jr      Z, syscall_return_normal
    ld      hl, #0
    ld      (_muzix_switch_pending), hl
    call    _muzix_zeta_resume_from   ; never returns

syscall_return_normal:
    ; Restore the process stack pointer without disturbing the result in DE.
    ; This used to be `ld hl,(SYSCALL_USER_SP) / ld sp,hl`, which
    ; threw the return value away a second time.  `ld sp,(nn)` is one
    ; instruction (ED 7B) and leaves HL alone.
    ld      sp, (SYSCALL_USER_SP)
    ret

; Global pointer to the current zeta_state_t.  This was emitted inside
; .area _CODE, which only worked because the kernel happens to be linked below
; 0x4000; a link-order change would have put the syscall entry's own data in a
; window the entry rewrites.
.area _DATA
_g_zeta_state_ptr:
    .dw #0
