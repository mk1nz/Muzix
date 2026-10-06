.module muzix_z80_io

.globl _z80_outb
.globl _z80_halt
.globl _z80_idle_halt
.globl _z80_capture_sp
.globl _z80_syscall_user_sp
.globl _z80_enable_interrupts
.globl _z80_disable_interrupts

.area _CODE

; void z80_outb(uint8_t port, uint8_t value)
; SDCC extern convention: param1 (port) in A, param2 (value) in HL (L)
_z80_outb:
    push af
    ld c,a        ; port from A
    ld a,l        ; value from L (low byte of HL)
    out (c),a
    pop af
    ret

; There is deliberately no z80_inb() here.  The Zeta V2 bank select registers
; $78-$7B are write-only and MPGENA ($7C) is write-only too; a generic
; "read a port" helper only ever invited callers to try to read the current
; bank out of hardware.  UART and FDC reads have their own, port-explicit
; routines.

; void z80_halt(void)
; Stop the processor until the next enabled interrupt.
;
; In ASSEMBLY and not inline asm in C, because a `__asm ... __endasm;` block is
; not valid host C and takes kernel_loop.c out of the host build with it - which is
; what happened the first time, and it took fifteen host tests down to eight
; without failing a single gate.  platform/zeta-v2/context_switch.c is still out of
; the host build for exactly that reason.
;
; Returns on the next interrupt, so this is a call and not a jump: the caller has a
; loop to carry on with.  `reti` in the handler leaves IFF1 as it found it, so the
; CTC tick at 120 Hz is what brings the CPU back.
_z80_halt:
    halt
    ret

; void z80_idle_halt(void)
; Enable maskable interrupts and halt without a call/return gap between them.
;
; EI takes effect after the following instruction. HALT is deliberately that
; instruction, so a pending interrupt wakes the CPU and execution continues at
; RET. This closes the race where an interrupt could be handled after a C call
; to z80_enable_interrupts() returned but before a separate call to z80_halt().
_z80_idle_halt:
    ei
    halt
    ret

; void z80_enable_interrupts(void)
; void z80_disable_interrupts(void)
;
; FUZIX does this, and it is the piece that was missing here.
;
; platform-zeta-v2's plt_idle() is a bare `halt`, and every one of its three call
; sites in Kernel/process.c is written as
;
;     ei();  plt_idle();  di();
;
; with the comment "yes please, interrupts on (WRS: they probably are already
; on?)".  FUZIX's scheduler runs with interrupts disabled, so it does not rely on
; IF happening to be set - it turns it on at the instruction before the halt.  That
; is the entire difference between "the processor waits for an interrupt" and "the
; processor waits forever", and it is stated in their source rather than inferred.
;
; Here the halt sits inside a system call and nothing on that path disables
; interrupts - no `di` in any C, no __critical, and tick.s:130's `di` is paired with
; an `ei` a few lines later - so IF should already be set.  "Should" is the whole
; problem.  Enabling it at the instruction before the halt makes the precondition
; hold regardless of who called, which is cheaper than a fourth argument.
;
; After the halt IF is left alone: this is a bare `ret`, and the context this is
; called from wants interrupts on, whereas FUZIX's scheduler wants them off again.
_z80_enable_interrupts:
    ei
    ret

_z80_disable_interrupts:
    di
    ret

; uint16_t z80_capture_sp(void)
; Read the stack pointer AS THE CALLER OF THIS FUNCTION SAW IT.
;
; On a Z80 the saved program counter of a suspended process is the return address
; sitting in its own stack frame, so saving SP saves both.  FUZIX relies on this
; and nothing more - the FUZIX switch-out pattern stores SP on the way out and
; restores it on the way back in, under the comment "FIXME: do we actually
; *need* to restore the stack!".
;
; The +2 undoes this function's own `call`, so what comes back is the SP the
; caller was running with - which is the value a park has to store, and at that
; address sits the return address into the caller.  Both are what
; _zeta_resume_jump needs: it restores SP and jumps to an explicit PC, and the PC
; is the word at the stored SP.
;
; Reading SP from C has no portable spelling, and an SDCC `__asm` block is not
; valid host C - which is how one in kernel_loop.c removed that module from the host
; build and took sixteen tests with it.  So it lives here, beside the other
; primitives, with a host stand-in.
_z80_capture_sp:
    ld      hl, #2
    add     hl, sp
    ld      e, (hl)
    inc     hl                  ; asxxxx has no 1(hl): displacement needs IX/IY
    ld      d, (hl)
    ret

; uint16_t z80_syscall_user_sp(void)
; The user's own stack pointer while a system call is in flight.
;
; SYSCALL_USER_SP is an equate in syscall_entry.s - 0xFFFC - so C cannot name it.
; Reading it is a platform primitive rather than arithmetic, so it lives here with
; the other ones and has a host stand-in: on a host, address 0xFFFC is an ordinary
; unmapped page and reading it faults, which is not a thing a test of the kernel
; should be doing.
;
; On entry to the trap SP points at the return address `call 0x0030` pushed, and
; the stub subtracts two to reach its pre-call value, so this cell holds the
; PROCESS's stack pointer and the word at it is the address in the process that
; the trap's closing `ld sp,(SYSCALL_USER_SP) / ret` returns to.  The trap has
; computed both on every syscall and discarded them; a parked process is the case
; that has to keep them.
_z80_syscall_user_sp:
    ld      hl, #0xFFFC
    ld      e, (hl)
    inc     hl                  ; asxxxx has no 1(hl)
    ld      d, (hl)
    ret
