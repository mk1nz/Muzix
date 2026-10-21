.module muzix_kernel_entry

.globl _kernel_main
.globl _muzix_z80_syscall_entry
.globl _muzix_zeta_uart_init
.globl _muzix_zeta_enter_userspace
.globl _muzix_userspace_banks
.globl _g_enter_entry_asm
.globl _g_enter_entry_asm_local
.globl _g_zeta_state_ptr
.globl _muzix_zeta_resume_from
.globl _zeta_resume_process
.globl _g_resume_pc
.globl _g_resume_sp
.globl _g_resume_ret
.globl _muzix_resume_pc
.globl _muzix_resume_sp
.globl _muzix_resume_ret
.globl _muzix_resume_banks
.globl _zeta_resume_process
.globl _zeta_load_map
.globl _muzix_zeta_jump_to_entry
.globl _muzix_boot_phase_return_addr
.globl _muzix_set_entry_point
.globl _muzix_zeta_uart_send
.globl _muzix_live_banks
.globl s__DATA
.globl l__INITIALIZED
.globl s__INITIALIZED
.globl s__INITIALIZER
.globl l__INITIALIZER

.area _CODE

; MUZ2 stage-2 entry point at 0x0098.
; rom_boot.s jumps here after copying ROM pages 0..3 to RAM pages 32..35.
; The Makefile links _CODE at 0x0098.

_start:
    di
    ld sp,#0xfffe

    ; Zero BSS (_DATA from s__DATA up to s__INITIALIZED, exclusive).
    ;
    ; The length is subtracted from the linker's own symbols instead of being
    ; written down.  The constant that stood here described an older layout
    ; (s__INITIALIZED was 0xAD52 then, and _INITIALIZER sat below the zeroed
    ; range).  The layout has since grown: s__INITIALIZED is now 0xAD34 and
    ; _INITIALIZER follows it at 0xAD43, so the old 0x0D52 ran 0x1E bytes
    ; past the end of _DATA and wiped both.  Measured on this tree with the
    ; emulator's store watch on 0xAD43 - the boot stub's page copy writes the
    ; first byte of &g_loop there (PC=0040, value 0x11) and this loop then
    ; stores 0x00 over it (PC=00A8) - leaving g_loop_ptr NULL, every user
    ; copy refused, and the first server to sendrec spinning forever.
    ld de, #s__DATA
    ld hl, #s__INITIALIZED
    xor a
    sbc hl, de
    ld b, h
    ld c, l
    ex de, hl
bss_zero_loop:
    ld a, b
    or c
    jr z, bss_zero_done
    ld (hl), #0
    inc hl
    dec bc
    jr bss_zero_loop
bss_zero_done:

    ; Publish the C static initialisers: SDCC emits the initial values into
    ; _INITIALIZER and leaves the startup to copy them to _INITIALIZED - the
    ; copy lib/crt0.s performs for every userspace program.  This kernel
    ; never did it, and its _GSINIT area is empty in this link (l__GSINIT is
    ; 0), so there is no generated routine to call; this is it.  Both areas
    ; are in window 2, which the boot stub's page copy already filled from
    ; ROM page 2 and the zero loop above deliberately spares.
    ld de, #s__INITIALIZED
    ld hl, #s__INITIALIZER
    ld bc, #l__INITIALIZER
    ld a, b
    or c
    jr z, init_done
    ldir
init_done:

    ; Install syscall vector: JP instruction at 0x0030 -> _muzix_z80_syscall_entry
    ld hl,#_muzix_z80_syscall_entry
    ld a,#0xc3
    ld (0x0030),a
    ld a,l
    ld (0x0031),a
    ld a,h
    ld (0x0032),a

    ; Hand off to C kernel - UART init moved to kernel_main after IM2/EI
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
; registers are written by the memory manager; all this does is hand over the
; entry point and jump.
_muzix_zeta_enter_userspace:
    ; Stash the entry point in a window-0 cell: _g_enter_entry_asm lives in
    ; window 0 (kernel page) which stays mapped.
    ld hl,(_g_enter_entry_asm)
    ld (_g_enter_entry_asm_local),hl

    ; Load zeta state and process map
    ld hl,(_g_zeta_state_ptr)
    ld de,#_muzix_userspace_banks

    ; NULL checks for state and target_map
    ld a,h
    or l
    jr z, _enter_userspace_refused
    ld a,d
    or e
    jr z, _enter_userspace_refused

    ; Load the process map (inline, no CALL because map change invalidates stack)
    ; HL = state, DE = target_map (process_map_t* with 4 pages)
    ;
    ; No push/pop at all.  Every machine push or pop after the bank 3 OUT would
    ; read or write a window that no longer belongs to the kernel, so the four
    ; pages are gathered into registers and written out immediately.  The state
    ; pointer is not needed on this path: the process map arrived in DE and the
    ; map is written straight from it.
    ;
    ; Use HL for memory access since Z80 only supports LD r, (HL)
    ex de, hl                   ; HL = target_map pointer
    inc hl                      ; skip pages[0] (bank 0 is always 0x20)
    ld b, (hl)                  ; pages[1]
    inc hl
    ld c, (hl)                  ; pages[2]
    inc hl
    ld d, (hl)                  ; pages[3]

    ; Window 3 is the kernel's own window, in the kernel map and in every
    ; process map: muzix_mproc_to_zeta_map() (mm/mproc_model.c) starts from
    ; {0x20,0x21,0x22,0x23} and replaces only windows 1 and 2, so pages[3] is
    ; 0x23 for anything that came through the MM and 0x20 is pinned into bank 0
    ; unconditionally below.  A map that does not carry 0x23 in pages[3] is a
    ; map that would hand window 3 - the kernel stack, the C frame of whoever
    ; called this, and every _DATA static - to a process page, and the jump at
    ; the bottom of this routine never returns to report it.  The copy paths
    ; already refuse a region-3 target for the same reason (ZETA_TRACE_WINDOW3
    ; in context_switch.c, reached from map_call_impl and from all three
    ; zeta_copy_* entry points); this is the handoff path refusing the same
    ; map on its way in.  Latent, not live: every publisher of
    ; _muzix_userspace_banks copies a slot map built by muzix_mproc_to_zeta_map,
    ; so the only way to reach the refusal is a caller that fills the array by
    ; hand - which is what the check is for.
    ;
    ; Register use is deliberate: A is the only register this touches, and B,
    ; C and D still hold the three pages the OUTs below need.
    ld a, d
    cp #0x23
    jr nz, _enter_userspace_refused

    ; Bank 0 is always kernel page 0x20
    ld a, #0x20
    out (0x78), a
    
    ; Bank 1 = pages[1] (was in B)
    ld a, b
    out (0x79), a
    
    ; Bank 2 = pages[2] (was in C)
    ld a, c
    out (0x7A), a
    
    ; Bank 3 = pages[3] (now in D)
    ld      a, d
    out     (0x7B), a

    ; Publish the same four pages into the window-0 mirror.
    ;
    ; This entry writes the bank registers itself instead of going through
    ; write_all_banks(), and write_all_banks() is the only thing that keeps
    ; _muzix_live_banks in step (context_switch.c).  The trap on the next
    ; syscall reads that mirror to learn which map it displaced, and copies it
    ; into _g_saved_user_context for the exit path to restore - so with the
    ; mirror still holding the kernel map from the last write_all_banks(), this
    ; process's first syscall handed it the *kernel* map back: it resumed
    ; running kernel code under its own stack, the `ret` at the end of the trap
    ; popped a return address out of kernel *code* in window 1, and the machine
    ; wandered into the boot stub and re-entered kernel_main.
    ;
    ; Window 0 is page 0x20 in the kernel map and in every process map, so these
    ; four writes are safe on both sides of the switch above, and none of them
    ; touches the stack.
    ld      hl, #_muzix_live_banks
    ld      (hl), #0x20
    inc     hl
    ld      (hl), b
    inc     hl
    ld      (hl), c
    inc     hl
    ld      (hl), d

    ; Do NOT touch the stack from here: the map is now the process's, so window
    ; 3 - which holds the kernel stack - maps the process's page.  The earlier
    ; push/pop pair "restored" two words nobody needed, and each of them read and
    ; wrote a window that had just stopped being the kernel's; the `call` below
    ; pushed a return address into the same dead window.  Fall straight through
    ; into the window-0 jump routine, which uses no stack.

    ; Jump to the process entry point (window-0 code, no stack use)
    jp _muzix_zeta_jump_to_entry

_enter_userspace_refused:
    ; Output '!' and spin
    ld a,#'!'
    call _muzix_zeta_uart_send
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

    call    _zeta_resume_process    ; installs map and jumps to process, never returns

_enter_resume_spin:
    halt
    jr      _enter_resume_spin

; Resume point published by the syscall trap (window 1 mapped, user stack visible).
; These live in _CODE (window 0) so they survive the map switch.
.globl _muzix_resume_pc
.globl _muzix_resume_sp
.globl _muzix_resume_ret
.globl _muzix_resume_banks
.globl _muzix_switch_pending
.globl _muzix_userspace_banks
_muzix_resume_pc:
    .dw #0
_muzix_resume_sp:
    .dw #0
_muzix_resume_ret:
    .dw #0
_muzix_resume_banks:
    .ds 4
_muzix_switch_pending:
    .dw #0
_muzix_userspace_banks:
    .ds 4

; Window-0 storage for the entry point. Deliberately in _CODE and not _DATA:
; _DATA is in window 3 and is remapped away by this very transition.
.globl _g_enter_entry_asm
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

; Local copy for zeta_enter_process to read BEFORE the map changes.
_g_enter_entry_asm_local:
    .dw #0

; Boot phase return-to-kernel support
_muzix_zeta_jump_to_entry:
    ld hl, (_g_enter_entry_asm_local)
    jp (hl)

_muzix_boot_phase_return_addr:
    .dw #0

; Mirror of the four bank registers, in window 0 so the syscall trap can read
; what is currently mapped while window 2 is still the process's own page.
; write_all_banks() keeps it in step with every map it installs, which is why
; the trap can trust it: the alternative is reading the ports, which are
; write-only, or reading state->bank_reg[] in _DATA, which is window 2 and
; therefore not the kernel's page at that point.
_muzix_live_banks:
    .ds 4

; Helper to write entry point from C (avoids fragile inline asm).
; Called as: void muzix_set_entry_point(uint16_t entry)
; Entry point passed in HL (sdcccall(1) convention).
_muzix_set_entry_point:
    ld (_g_enter_entry_asm), hl
    ret
