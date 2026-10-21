.module muzix_zeta_bank_io

.globl _muzix_zeta_ldir_staged
.globl _muzix_zeta_staged_src
.globl _muzix_zeta_staged_dst
.globl _muzix_zeta_staged_len
.globl _muzix_zeta_ldir_copy
.globl _muzix_zeta_caller_in_window0

; Bank state has to survive a bank switch, so it cannot live in _CODE: the
; bytes would be fetched as instructions.  It used to be pinned at absolute
; addresses 0x00F0/0x00F4 on the theory that window 3 is "intentionally a user
; stack".  That is stale - exec_loader maps the process stack into window 1 and
; leaves window 3 on the kernel page - and 0x00F0 is not spare address space at
; all: it is inside kernel_entry.s, whose code starts at 0x0098.  So
; [removed 2026-10-02] muzix_zeta_set_cached_banks() used to LDIR 4 bytes over
; kernel_entry.s's `im 0 / ld hl,de / ex de,hl / add ix,ix / push ix`, and the
; next userspace handoff jumped into the middle of it.  Both scratch words now
; live in _DATA, which is window 3 = kernel page 0x23 - a page no process ever
; maps, so the state is as durable as it was at 0x00F0.

.area _CODE

; uint8_t muzix_zeta_caller_in_window0(void)
;
; The bank select registers are write-only, so the hardware cannot report
; where the CPU is fetching from.  The only way to find out is to look at the
; address the caller is about to return to.
;
; The two C forwarders in context_switch.c (zeta_cross_bank_call_impl and
; zeta_with_temp_map_impl) push HL and DE before calling us, so the caller's
; return address is at SP+6 from inside this function.  Direct callers that
; do not push anything would put it at SP+2, but the only call sites in the
; tree are those two forwarders, so SP+6 is the reliable offset.
;
; Returns 1 in A when the calling function resumes in window 0, 0 otherwise.
; Call this *directly* from the public entry point that is about to switch
; banks: any helper in between would hand back an address inside the kernel
; module, which is in window 0 by construction and would make the check
; vacuously true.
_muzix_zeta_caller_in_window0:
    ld hl, #6
    add hl, sp
    ld e, (hl)
    inc hl
    ld a, (hl)       ; high byte of the caller's return address
    and #0xc0        ; window index lives in bits 5 and 6
    jr nz,001$
    ld a, #0x01
001$:
    ret

; void muzix_zeta_ldir_staged(void)
;
; ldir from the staged source to the staged destination.  The three words live
; in _DATA rather than in registers because SDCC cannot be relied on to hand
; them over intact: an assembly callee declared with (src, dst, len) looks
; straightforward, but with the default sdcccall(1) convention SDCC emits
;
;     ld  hl, #_enter_kernel_hook
;     or  a, (hl)                  ; NULL test
;     call _muzix_sdz80_call_iy
;
; for a function-pointer call and a spill-and-restore shuffle around a direct
; one, and a run where it did - the count arrived in BC as something other than
; the length.  ldir runs until BC is zero, so the copy walked off the end of
; both the staging window and the destination.  Three plain word stores from C
; have no convention to disagree about.
;
; This is what makes the system usable to work on: the byte-at-a-time version
; cost roughly 145k cycles per 512-byte block, so loading the 6.7 KiB shell
; image took about 12 million cycles before userspace ran its first
; instruction, and a single emulator run barely reached the first syscall.
_muzix_zeta_ldir_staged:
    ld      de, (_muzix_zeta_staged_dst)
    ld      bc, (_muzix_zeta_staged_len)
    ld      hl, (_muzix_zeta_staged_src)
    ldir
    ret

_muzix_zeta_staged_src:
    .dw #0
_muzix_zeta_staged_dst:
    .dw #0
_muzix_zeta_staged_len:
    .dw #0

; void muzix_zeta_ldir_copy(uint8_t *dst, const uint8_t *src, uint16_t len)
;
; The general form of the staged copy: ldir from src to dst, len bytes, taking
; the operands in registers.  The staged version above exists because a direct
; asm callee cannot be relied on to RECEIVE its arguments - SDCC's peephole for
; a call to an assembly function is not one this tree can lean on - so the
; staged version takes them from three word cells in _DATA instead.
;
; This one takes the same cells, but for the caller that copies a chunk of a
; process image at boot.  The boot copy in kernel/rom_read.c did a byte loop
; with size_t indexing, which on this target is fifteen instructions per byte -
; every one of them an ix-relative fetch of the loop counter - and the shell
; prompt took 13.5 million instructions to reach, of which the exec phase alone
; was 5.4 million for about 54 KiB of images.  ldir is 21 cycles of setup plus
; 21 cycles per 256 bytes, so the same copy is roughly a fiftieth of the work.
;
; The byte loop's cost was not merely aesthetic: at the measured rate the boot
; was 37 seconds on a 4 MHz part, almost all of it before the first character
; of the banner.
_muzix_zeta_ldir_copy:
    ld      hl, (_muzix_zeta_staged_src)
    ld      de, (_muzix_zeta_staged_dst)
    ld      bc, (_muzix_zeta_staged_len)
    ldir
    ret

; _user_banks and _saved_banks were the four-byte shadows the removed bank
; entry points LDIRed between.  Nothing reads either of them now, so they are
; gone rather than left as storage that looks live: the comment that once
; explained their width was describing `ld bc,#4` in three functions that no
; longer exist.
