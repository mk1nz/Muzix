.module muzix_syscall_entry

.globl _muzix_z80_syscall_entry
.globl _muzix_z80_dispatch_frame
.globl _g_zeta_state_ptr
.globl _muzix_resume_pc
.globl _muzix_resume_sp
.globl _muzix_resume_ret
.globl _muzix_switch_pending
.globl _muzix_zeta_resume_from
.globl _zeta_resume_process
.globl _muzix_trace_write
; _muzix_live_banks is the window-0 mirror of the four bank registers, and it is
; reached by the trap entry *before* the kernel map is installed, so it is a
; window-0 cell that tools/check_window0.rb pins.
.globl _muzix_live_banks
; Where the trap parks the map it displaced, for zeta_kernel_exit to restore.
.globl _g_saved_user_context
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
;   * return       16-bit result in DE
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
;
; SYSCALL_USER_SP is 0xFFFC, and that address is NOT a free choice - it is the
; one z80_io.s's z80_syscall_user_sp() reads.  These two MUST name the same
; cell, and while they did not the consequence was total:
;
;   z80_syscall_user_sp() reads 0xFFFC.  The trap stores the user SP at
;   0xE9FE.  Nothing ever wrote 0xFFFC, so the guard in
;   muzix_proc_table_park() - `if (sp != 0 && pc != 0)` - tested a cell of
;   never-written memory.  When that reads as zero, park() records no resume
;   point at all, so kernel_loop's `if (current_slot->resume_valid)` takes the
;   ELSE branch and re-enters the process at its ENTRY POINT instead of
;   resuming it at its syscall return address.
;
;   Every parked syscall therefore restarted the process from _main, on a stack
;   that was already deep from the previous attempt.  The banner came out as a
;   single repeated byte, PC ended up walking through non-code memory one
;   single-byte opcode at a time (measured over five runs: PC 0xF71B..0xF71E,
;   always in window 3, with SP correctly constant in window 1), and the shell
;   never printed.
;
;   It looked like a fault in the reply path, in the copy, and in the message
;   fields in turn, because the symptom is downstream of all three.  It was
;   one cell named differently in two files.
;
; 0xFFFC-0xFFFD is free: SYSCALL_ARGS ends at 0xFFFB, the dispatch call's return
; address lands at 0xFFEA (just under SYSCALL_SP), and the kernel stack below
; that is reachable in one push.  Keeping the user SP beside the other cells
; rather than in a distant "gap" is what makes the two files able to agree.
SYSCALL_USER_SP  = 0xFFFC        ; the PROCESS's SP; MUST equal z80_io.s's 0xFFFC
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
    ld      a, d
    cp      #0x40                    ; refuse a stack pointer below window 1
    jr      C, fork_stack_done
    cp      #0x80                    ; refuse a stack pointer above window 1
    jr      NC, fork_stack_done
    ; fall through to measure
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

    ; Install the kernel map HERE, in window 0, before any C dispatcher runs.
    ;
    ; Every C routine on this path reaches _DATA, and _DATA is window 2, and
    ; window 2 is the calling process's own page until the map is switched.
    ; g_zeta_state.bank_reg[] - which zeta_kernel_enter reads to know what to
    ; install - lives in _DATA, so asking it from here asked the process for
    ; the kernel's map and got process bytes back.
    ;
     ; The four constants need no memory at all, so they are safe first.  They are
     ; the four kernel pages of bank_model.h (0x20,0x21,0x22,0x23) written out as
     ; literals because sdasz80 cannot include a C header; write_all_banks() in
     ; context_switch.c pins window 0 to 0x20 for the same reason.
     ;
     ; The mirror in _muzix_live_banks is window 0, so it can be read afterwards
     ; to publish the map we displaced into the _DATA cell the exit path restores.
     ld      a, #0x20
     out     (0x78), a
     ld      a, #0x21
     out     (0x79), a
     ld      a, #0x22
     out     (0x7A), a
     ld      a, #0x23
     out     (0x7B), a

      ; Trace: syscall entry - the trace-buffer copy.  This is the only mark the
      ; trap leaves, and it goes to the ring buffer rather than to the UART:
      ; AGENTS.md reserves the serial line for boot output and puts kernel
      ; tracing in platform/zeta-v2/trace.c, dumped by the fallback shell's
      ; `trace` command.  An earlier version also wrote the same string straight
      ; to the THR before the map switch fired, which printed once per syscall
      ; and buried everything else; that call is gone.
      ;
      ; Everything up to the four OUTs above runs under the *process's* map:
      ; window 2 is the caller's own text page until 0x7A is written, _DATA
      ; starts at 0xA000 which is window 2, and SP is still the caller's stack
      ; in window 1.  muzix_trace_write() writes g_trace_ring[] and g_trace_head
      ; through _DATA and pushes its frame onto that stack, so calling it before
      ; the switch ran it against the process's page: it read g_trace_head back
      ; as process bytes and used it as an index into a 512-byte ring, and wrote
      ; every marker byte through window 2 into the caller's own text page.  The
      ; kernel stack was never touched, so nothing caught it; the caller just
      ; came back to its own code with holes in it.  That is the reboot.
      ;
      ; SP is 0xFFEC by now (SYSCALL_SP, set further up) so the frame lands on
      ; the kernel stack, and every register this clobbers is reloaded below.
      ; (trace call suppressed for clean output)

     ld      hl, #_muzix_live_banks        ; window 0: the displaced map
     ld      de, #_g_saved_user_context     ; _DATA, now the kernel's again
     ld      bc, #4
     ldir

     ; Publish the map we installed into the window-0 mirror.
     ;
     ; write_all_banks() is what normally keeps _muzix_live_banks in step, and
     ; the four OUTs above bypassed it to write the ports directly - so the
     ; mirror kept the displaced process map while the kernel map was actually
     ; installed.  The trap had already spent its copy of the displaced map
     ; into g_saved_user_context above, so this is safe, and every reader
     ; inside the handler now sees the truth.
     ;
     ; Measured without this: the FS server's first sendrec ran
     ; copy_temp_map() with the process map still in the mirror, so its
     ; user->kernel copy installed banks 20 25 24 23 for the staging buffer -
     ; 24 being the process's own text page and not the kernel's 22 - the
     ; staging writes landed in the process's text, and the restore read
     ; g_context_stack back through that same wrong page and installed
     ; 20 00 00 00.  The kernel was then running out of ROM: one reboot per
     ; trap.  The exit path does not repair it - zeta_kernel_exit() installs
     ; g_saved_user_context, which is the process's map, and the next trap
     ; re-displaces that.
     ld      hl, #_muzix_live_banks
     ld      (hl), #0x20
     inc     hl
     ld      (hl), #0x21
     inc     hl
     ld      (hl), #0x22
     inc     hl
     ld      (hl), #0x23

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
    ; project's KERNEL_CFLAGS: plain `-mz80 --opt-code-size`, with no
    ; --peep-file - that swap was taken out when platform/zeta-v2/
    ; sdz80_runtime.s began providing ___sdcc_call_hl/_iy itself (see the
    ; Makefile note above the flag definition).  Under plain -mz80 a 16-bit
    ; return value comes back in DE, and that is not read off the
    ; documentation: tools/check_z80_call_convention.rb measures it from a
    ; compiled probe and fails the build on a disagreement, and that gate runs
    ; from test-rom-image, i.e. from every `make rom`.
    ;
    ; DE is also where the OTHER exit delivers.  A syscall that parked its
    ; caller does not return through this trap at all: it comes back through
    ; _muzix_zeta_resume_from, which loads _muzix_resume_ret and hands it to
    ; the process in DE (zeta_resume_process: `ld de,(_g_resume_ret)` before
    ; `jp (hl)`).  So the parked exit and the ordinary exit put the syscall's
    ; answer in the same register, which is what lets one userspace stub
    ; (lib/user_syscall.s) publish it with one `ld (_syscall_result),de`.  An
    ; earlier revision of the park exits wrote the result through HL while the
    ; resume path read DE, and a resumed process therefore answered its
    ; syscall with whatever HL happened to hold.
    ;
    ; lib/user_syscall.s publishes the return by reading DE, so DE is where it
    ; has to be on exit from this trap.
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
    ; If the flags are ever changed and the 16-bit return moves out of DE,
    ; fix the files together: this trap, the register that
    ; lib/user_syscall.s stores, and the DE that zeta_resume_process hands a
    ; resumed process.  They have to agree.  The convention gate
    ; (tools/check_z80_call_convention.rb, run from test-rom-image) now
    ; measures the return register from a compiled probe, so a flags change
    ; that moves it fails the build rather than the machine - but the gate
    ; measures the CONVENTION, not this trap's agreement with the stub, so the
    ; three files are still a review item.

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
    ; There used to be a block here that went
    ;
    ;     ld hl, (_muzix_resume_ret)
    ;     ld (hl), e
    ;     inc hl
    ;     ld (hl), d
    ;
    ; on the theory that the resumed process needs this syscall's return value.
    ; It does not: muzix_zeta_resume_from() loads _muzix_resume_ret into
    ; _g_resume_ret and zeta_resume_process() hands it to the process in DE -
    ; the published value IS the delivery.  What the block actually did was
    ; store DE through that value as an ADDRESS, and the parked servers carry
    ; 0xFFFC there, which is SYSCALL_USER_SP itself: every server resume wrote
    ; two bytes over the cell the next trap reads the user stack pointer from,
    ; and the fork that records the child's resume point then captured a
    ; pointer to kernel code.  The forked child resumed into the kernel and
    ; never ran, which is why no userspace command ever completed.
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

; The current zeta_state_t, as one cell in window 0.  syscall_entry.c holds the
; storage; the trap reads it before the map switch, so it cannot be in _DATA.
_g_zeta_state_ptr:
    .dw #0

; The one marker the trap leaves, written into the trace ring by the C call
; above.  _CODE, not _DATA, for the same reason as _g_zeta_state_ptr: this is
; read through window 2, which is the calling process's own text page until the
; four bank OUTs above have run.
TRACE_SYSCALL_MSG:
    .asciz "trapentry\r\n"

; The syscall result, parked across the map restore - in window 0, NOT in
; _DATA.  It used to be a `static uint16_t` in syscall_entry.c, which put it in
; _DATA, and _DATA is window 2 ($8000-$BFFF): under the kernel map that window
; is kernel page 0x22, but under a process map it is the process's own page.
; muzix_z80_dispatch_frame stores the handler's result here, then calls
; restore_user_hook - which installs the calling process's map through
; zeta_kernel_exit()/write_all_banks() - and only then reads the cell back to
; return it to userspace.  With the cell in _DATA that read came out of the
; process's page instead of the kernel's.
;
; Measured on the emulator, five identical runs: the store wrote 0x0000 (the
; copy had in fact succeeded), and the reload at the dispatcher's return - the
; `ld de,(_g_syscall_result)` at kernel PC 0x3EDA, after the hook at 0x3ED7 -
; read DE=0x0000 under the kernel map (banks 20 21 22 23) but DE=0x4D98 with the
; FS server's map installed (banks 20 25 24 23); 0x4D98 is the FS server's own
; _g_fs_tty.send pointer at the same offset in its page (0x984D stored at
; 0xB456).  handle_write's `sys_ipc_srv_copy_from_client(...) != 0` therefore
; read true at fs_server.c:155-156, returned -1 before muzix_tty_write was ever
; called, left g_reply unwritten, and the shell's write() came back 0: no
; banner, no `uptime` output, then _sys_exit.
;
; Window 0 is the one window every map keeps as the kernel's - process maps are
; {0x20, stack, text, 0x23} per mm/mproc_model.c and tools/check_window0.rb -
; which is the same reason _g_zeta_state_ptr above, the _muzix_resume_* cells
; in kernel_entry.s and _muzix_live_banks all live out here and not in _DATA.
; The trap's ordinary return path reads this cell after the map restore, so it
; has to be one of them.  _CODE is RAM at runtime: rom_boot.s LDIRs the kernel
; region's pages 0-3 to RAM pages 32-35 before it jumps to 0x0098, which is why
; _g_zeta_state_ptr can be written the same way.
.globl _g_syscall_result
_g_syscall_result:
    .dw #0

; The fork stack depth, written by the trap's measurement block above.
;
; _CODE, not _DATA, for the same reason as _g_syscall_result, and this one was
; measured rather than argued about: the cell used to be a C static in
; kernel_main.c, which put it in _DATA at 0xBE2A - window $8000-$BFFF - and the
; measurement block runs at trap entry, BEFORE the four bank OUTs install the
; kernel map, so every store landed in the calling process's own text page.
; The fork handler then read the kernel's copy, which nothing had ever written,
; and concluded the parent's stack was 0 bytes deep: the child took the fallback
; and shared the parent's stack page, which is exactly the failure the
; measurement exists to prevent - the child runs over the frames its parent is
; suspended in, and the parent's later resume jumps through whatever the child
; left behind.
;
; Window 0 is the one window a process map never replaces (mm/mproc_model.c,
; tools/check_window0.rb), which is why the trap's other early cells - the
; resume point, the switch flag, the bank mirror - are up here with it.
_muzix_fork_stack_len:
    .dw #0
