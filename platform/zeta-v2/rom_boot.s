; Zeta SBC V2 512 KiB ROM reset stub.
; Copies ROM pages 0..3 to RAM pages 32..35, then enters the MUZ2-compatible
; stage-2 payload at 0x0098.  ROM pages 4..31 remain the read-only rootfs.

        .module muzix_zeta_rom_boot

        .area _LOADER (ABS)
        .org 0x0000

rom_start:
        di
        xor a
        out (0x78),a                 ; ROM page 0 in window 0
        ld a,#1
        out (0x7c),a                 ; MPGENA = 1, enable paging

        xor a
        out (0x79),a                 ; ROM source page in window 1
        ld a,#32
        out (0x7a),a                 ; corresponding RAM page in window 2
        ld hl,#0x4000
        ld de,#0x8000
        ld bc,#0x4000
        ldir

        ld a,#1
        out (0x79),a
        ld a,#33
        out (0x7a),a
        ld hl,#0x4000
        ld de,#0x8000
        ld bc,#0x4000
        ldir

        ld a,#2
        out (0x79),a
        ld a,#34
        out (0x7a),a
        ld hl,#0x4000
        ld de,#0x8000
        ld bc,#0x4000
        ldir

        ld a,#3
        out (0x79),a
        ld a,#35
        out (0x7a),a
        ld hl,#0x4000
        ld de,#0x8000
        ld bc,#0x4000
        ldir

        ; Switch all windows to the copied kernel image.  The following JP is
        ; fetched from the identically copied page 0 after the first OUT.
        ld a,#32
        out (0x78),a
        inc a
        out (0x79),a
        inc a
        out (0x7a),a
        inc a
        out (0x7b),a
        jp 0x0098

        .ds 0x98 - (.-rom_start)
