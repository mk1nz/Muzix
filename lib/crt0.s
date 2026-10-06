.module muzix_crt0

.globl _main
.globl _exit
.globl s__INITIALIZED
.globl l__INITIALIZED
.globl s__INITIALIZER
.globl s__DATA
.globl l__DATA
.globl start

.area _CODE

; crt0 startup for MUZIX userspace binaries
; Where kernel/exec_loader.c leaves argc and argv for this program.  Keep in
; step with MUZIX_EXEC_ARGC_ADDR / MUZIX_EXEC_ARGV_ADDR there.
MUZIX_EXEC_ARGC_ADDR  .equ 0x7FF0
MUZIX_EXEC_ARGV_ADDR  .equ 0x7FEE
MUZIX_EXEC_SP_ADDR    .equ 0x7FEC

; SDCC Z80 sdcccall(1) calling convention
; Entry point (from MZ header) jumps here
start:
    ; Set up the stack pointer the exec loader chose.
    ;
    ; It used to be a constant, 0x7FF0, which assumes nothing lives between
    ; there and the top of window 1.  The argument block does live there - the
    ; pointer array just under 0x7FFF and the strings running down from it - so
    ; a program with a frame, such as `cat` with its 512-byte buffer, grew its
    ; stack straight over the arguments it had just been handed.  The loader now
    ; publishes the first address below the block; the stack grows down from
    ; there into empty window 1.
    ld hl, (MUZIX_EXEC_SP_ADDR)
    ld sp, hl

    ; Copy initialized data from _INITIALIZER (ROM) to _INITIALIZED (RAM)
    ; l__INITIALIZED = size of initialized data
    ; s__INITIALIZER = source address in ROM (where initializer is loaded)
    ; s__INITIALIZED = destination address in RAM
    ld hl, #s__INITIALIZER
    ld de, #s__INITIALIZED
    ld bc, #l__INITIALIZED
    call memcpy_init

    ; Zero the uninitialised data.
    ;
    ; This link has no _BSS area: SDCC places uninitialised globals in _DATA,
    ; so _DATA is what has to be cleared, and _INITIALIZED/_INITIALIZER are the
    ; copy pair for data that comes out of the image.  For shell that is
    ; _DATA = 0x9804 + 296, immediately below _INITIALIZED = 0x992C + 152.
    ;
    ; Two earlier attempts were wrong in opposite directions.  Deriving the
    ; length as l__DATA - l__INITIALIZED handed memset_bss 294 bytes for a
    ; 152-byte region; deriving it as (s__DATA + l__DATA) - (s__INITIALIZED +
    ; l__INITIALIZED) is worse, because _DATA is laid out BELOW _INITIALIZED,
    ; so that is 0x992C - 0x99C4 = -152.  memset_bss then cleared 65416 bytes
    ; upward from 0x99C4, straight through the end of the process's writable
    ; image and into whatever the text bank had mapped above it.  tools/
    ; check_userspace_layout.rb now fails the build if this stops being true.
    ld hl, #s__DATA
    ld bc, #l__DATA
    call memset_bss

    ; Hand main() its argc and argv.
    ;
    ; The exec loader writes both into fixed words just below the top of
    ; window 1, because a process starts with a fresh stack and has no other way
    ; to learn them.  These addresses are MUZIX_EXEC_ARGC_ADDR and
    ; MUZIX_EXEC_ARGV_ADDR in kernel/exec_loader.c; keep the two in step.
    ;
    ; main() is a C function, so it does NOT read argc/argv off the stack.  The
    ; project's own modules are compiled with plain `-mz80` and no --sdcccall,
    ; so SDCC passes the first parameter in HL, the second in DE, and the third
    ; and any after that on the stack.  The SDCC library linked alongside them
    ; is sdcccall(1) and does not share that mapping, so the name of the
    ; convention is no guide here - only the generated code is.
    ; platform/zeta-v2/syscall_entry.s already documents the register pair this
    ; depends on ("sdcccall(1): param1 in HL, param2 in DE") and apps/cat.lst
    ; is the proof on both sides: the caller sets HL=path, DE=flags before
    ; `call _open`, and _open reads its flags with `ld a,d`; main's own
    ; prologue spills HL to the slot it later compares against 2 for
    ; `argc < 2`, and DE to the slot it adds i*2 to for argv[i].
    ;
    ; Pushing them, as this used to, left argc in HL only by accident: the last
    ; `ld hl,(...)` happened to be the argc load, and argc is what HL carries.
    ; argv is DE, and nothing had ever loaded DE, so it still held whatever
    ; memcpy_init last put there.  Watched on the real image, `cat readme`
    ; entered main with HL=0x0002 and DE=0x92E5 - s__INITIALIZED, i.e. the
    ; middle of its own text window - while the loader had correctly published
    ; argv=0x7FFB with the array {0x7EF7, 0x7EF0} there.  argv[1] was then read
    ; out of the text window instead of the array, which is why the address the
    ; kernel was asked to copy a path from was below 0x4000 and why the console
    ; said "zeta: w0 map".
    ;
    ; So argv (the second parameter) goes in DE first, then argc (the first) in
    ; HL.  Neither load disturbs the other register, and main sets up its own
    ; frame, so no stack argument is involved and none is pushed.
    ld de, (MUZIX_EXEC_ARGV_ADDR)   ; argv: main's second parameter
    ld hl, (MUZIX_EXEC_ARGC_ADDR)   ; argc: main's first parameter

    ; Call main()
    call _main

    ; Call exit() with return value from main (in HL)
    push hl
    call _exit

; memcpy_init: copy BC bytes from HL to DE
; Preserves: none
memcpy_init:
    ld a, b
    or c
    ret z
memcpy_loop:
    ld a, (hl)
    ld (de), a
    inc hl
    inc de
    dec bc
    ld a, b
    or c
    jr nz, memcpy_loop
    ret

; memset_bss: zero BC bytes at HL
; Preserves: none
memset_bss:
    ld a, b
    or c
    ret z
memset_loop:
    ld (hl), #0
    inc hl
    dec bc
    ld a, b
    or c
    jr nz, memset_loop
    ret