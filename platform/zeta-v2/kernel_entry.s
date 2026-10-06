.module muzix_kernel_entry

.globl _kernel_main
.globl _muzix_z80_syscall_entry
.globl _muzix_zeta_uart_init
.globl _muzix_zeta_enter_userspace
.globl _muzix_userspace_entry
.globl _muzix_userspace_banks
.globl _zeta_enter_process
.globl _g_enter_entry_asm
.globl _g_zeta_state_ptr
.globl _muzix_zeta_resume_from
.globl _zeta_resume_jump
.globl _g_resume_pc
.globl _g_resume_sp
.globl _g_resume_ret
.globl _muzix_resume_pc
.globl _muzix_resume_sp
.globl _muzix_resume_ret
.globl _muzix_resume_banks
.globl _zeta_resume_process

.area _CODE

; MUZ2 stage-2 entry point at 0x0098.
; rom_boot.s jumps here after copying ROM pages 0..3 to RAM pages 32..35.
; The Makefile links _CODE at 0x0098.

_start:
    di
    ld sp,#0xfffe

    ; Install syscall vector: JP instruction at 0x0030 -> _muzix_z80_syscall_entry
    ld hl,#_muzix_z80_syscall_entry
    ld a,#0xc3
    ld (0x0030),a
    ld a,l
    ld (0x0031),a
    ld a,h
    ld (0x0032),a

    ; Initialize UART for console output
    call _muzix_zeta_uart_init

    ; Hand off to C kernel
    call _kernel_main

_halt:
    halt
    jr _halt

; Kernel -> process transition.  THIS ROUTINE IS IN WINDOW 0, and that is the
; entire reason it exists here.
;
; Window 0 is the only window the MM never remaps, so it is the only place
; from which a map change and the jump that immediately follows it can be
; performed.  The equivalent C function compiles, links and then fails at run
; time: the moment load_map() installs the process map, windows 1-3 show the
; process's pages, so the code doing the install stops fetching correctly and
; every _DATA static it reads (window 3) reads back as the process's page.
; A callee that RETURNS after installing is equally broken: its epilogue pops
; the return address out of window 3, which is now the process's page.
;
; There are deliberately no `out (0x79..0x7b)` instructions here. The bank
; registers are written by the memory manager, zeta_enter_process(); all this
; does is hand over the entry point and jump.
_muzix_zeta_enter_userspace:
    ; Stash the entry point in a window-0 cell: _muzix_userspace_entry lives in
    ; _DATA (window 3), which stops being readable for the kernel the moment the
    ; map changes, but this cell does not.
    ld hl,(_muzix_userspace_entry)
    ld (_g_enter_entry_asm),hl

    ; sdcccall(1) with a void return: param1 in HL, param2 in DE, so
    ; zeta_enter_process(state, target_map) needs HL = the zeta state and
    ; DE = the process map. These were the wrong way round: the callee read a
    ; 16-bit fragment of the bank bytes as `state` and the state pointer as
    ; `target_map`. Both non-NULL checks passed, so it installed a map read
    ; from whatever followed &g_mm.zeta in memory - the boot stub then ran
    ; again and the machine rebooted instead of entering userspace.
    ld hl,(_g_zeta_state_ptr)
    ld de,#_muzix_userspace_banks

    call _zeta_enter_process        ; never returns

    ; Unreachable: zeta_enter_process either jumps to the process or spins.
_enter_userspace_spin:
    halt
    jr _enter_userspace_spin

; Resume a process at a saved point, from a saved context switch.
;
; Same reasoning as muzix_zeta_enter_userspace above: this has to be window-0
; code, it installs the map, and it never comes back.  The difference is that it
; starts the process where it left off rather than at an entry point.
;
; The caller - the syscall trap's exit path - has written the resume point into
; _DATA, which window 3 keeps mapping as the kernel page only for as long as the
; kernel map is installed.  All four words are therefore copied into window-0
; cells here, before the map changes, exactly as the entry point above is.
;
; sdcccall(1): param1 -> HL, param2 -> DE, so the zeta state goes in HL and the
; process map in DE.  This is the same order muzix_zeta_enter_userspace uses,
; which the comments there record as a bug that rebooted the machine when it was
; the other way round.
_muzix_zeta_resume_from:
    ld      hl, (_muzix_resume_pc)
    ld      (_g_resume_pc), hl
    ld      hl, (_muzix_resume_sp)
    ld      (_g_resume_sp), hl
    ld      hl, (_muzix_resume_ret)
    ld      (_g_resume_ret), hl

    ; `#_g_zeta_state_ptr`, not `(_g_zeta_state_ptr)`. The parenthesised form
    ; assembles to `2A nn nn`, which loads the *word stored at* the cell - and
    ; that word is the pointer, 0xE657. The two `ld a,(hl)` / `ld h,(hl)` below
    ; then read the bytes of the state struct at that address, which are
    ; `bank_reg[0]` and `bank_reg[1]` = 0x20, 0x21, so HL came out as 0x2120:
    ; a code address. `_zeta_resume_process` was therefore handed 0x2120 as its
    ; `state`, wrote the bank-register shadow over the kernel's own code at
    ; 0x2120-0x2123, and left the real `zeta_state_t.bank_reg[]` holding the
    ; *previous* map. Every later save_map() snapshots that stale shadow, so the
    ; first syscall after a context switch restored the kernel map on its way
    ; out and the process resumed against the wrong windows. syscall_entry.s's
    ; dispatch path already loads it the `#` way; this one did not.
    ;
    ; Measured, with no instrumentation in the kernel: the emulator's store
    ; watchpoint on 0x2120 sees exactly two writes in a whole session, 0x20 and
    ; 0x21, from PC=0x15BD - `ld (bc),a` in write_all_banks, i.e. the
    ; bank_register[0] assignment - which can only happen if write_all_banks'
    ; `state` argument is 0x2120. A watchpoint on 0xF78F sees exactly one write
    ; in the whole session, 0x57 (the low byte of 0xE657), from PC=0x2048, which
    ; is muzix_z80_set_zeta_state() at boot: the cell itself is never corrupted.
    ld      hl, #_g_zeta_state_ptr
    ld      a, (hl)
    inc     hl
    ld      h, (hl)
    ld      l, a
    ld      de, #_muzix_resume_banks

    call    _zeta_resume_process    ; installs the map, then returns
                                    ; (the window-0 cells above survive it)
    call    _zeta_resume_jump       ; loads SP and PC, never returns

_enter_resume_spin:
    halt
    jr      _enter_resume_spin

; Load the stack and program counter of a saved process and go.  Window 0, and
; never returns.  The cells are filled by muzix_zeta_resume_from above, after
; the map has already been installed, so this must be here rather than in the C
; it is called from: window 1 is a process page by this point and would not hold
; the instructions.
; Written in assembly because SDCC's inline assembler cannot emit `ld sp,hl`.
_zeta_resume_jump:
    ld      sp, (_g_resume_sp)
    ld      de, (_g_resume_ret)
    ld      hl, (_g_resume_pc)
    jp      (hl)

; Window-0 storage for the entry point. Deliberately in _CODE and not _DATA:
; _DATA is in window 3 and is remapped away by this very transition.
_g_enter_entry_asm:
    .dw #0

; Same for a context switch's resume point: a PC into the process's own text
; page, the stack pointer it was using, and the value its pending syscall was
; going to return.  All three are copied out of _DATA before the map changes.
_g_resume_pc:
    .dw #0
_g_resume_sp:
    .dw #0
_g_resume_ret:
    .dw #0
