; MUZIX UNA BIOS stage-1 loader for Zeta V2 compatible systems.
;
; UNA loads sector 0 at 0x8000.  The loader first relocates to 0xfa00, maps
; the UNA user page, then loads a self-describing stage-2 image from LBA 2 to
; 0x0088.  The stage-2 header makes the entry and sector count explicit while
; retaining the filesystem reservation beginning at LBA 2048.

        .module muzix_una_boot

UNABIOS_ENTRY   = 0xfffd
UNABIOS_HISTORY = 0xfc
UNABIOS_BOOTGET = 0x00
UNABIOS_GETINFO = 0xfa
UNABIOS_GET_USER_PAGES = 0x05
UNABIOS_BANKEDMEM = 0xfb
UNABIOS_BANK_SET = 0x01
UNABIOS_GET_HMA = 0xf1
UNABIOS_SETLBA  = 0x41
UNABIOS_READ    = 0x42

LOAD_ADDRESS    = 0x0088
RELOCATED       = 0xfa00
BUFFER          = 0xf800
STACK_TOP       = 0xfe00
FIRST_LBA       = 2
MAX_SECTORS     = 124
STAGE2_VERSION  = 1
STAGE2_HEADER   = 16

        .area _LOADER (ABS)
        .org RELOCATED

start:
        jr relocate
        .ds 0x40 - (.-start)

relocate:
        ld hl,#0x8000
        ld de,#RELOCATED
        ld bc,#512
        ldir
        jp relocated

relocated:
        ld sp,#STACK_TOP

        ; Ask UNA which unit selected this boot sector.
        ld bc,#(UNABIOS_HISTORY << 8 | UNABIOS_BOOTGET)
        call #UNABIOS_ENTRY
        ld a,l
        ld (boot_unit),a

        ; Load into the page that UNA reserves for the booted user image.
        ; This is UNA's bank API, not an emulator FDC interface.
        ld bc,#(UNABIOS_GETINFO << 8 | UNABIOS_GET_USER_PAGES)
        call #UNABIOS_ENTRY
        ld bc,#(UNABIOS_BANKEDMEM << 8 | UNABIOS_BANK_SET)
        call #UNABIOS_ENTRY

        ; Preserve the UNA entry vector for a stage-2 that needs BIOS calls
        ; and mark the CP/M BDOS vector unavailable after a cold boot.
        ld hl,#UNABIOS_ENTRY
        ld de,#0x0008
        ld bc,#3
        ldir
        ld a,#0x76
        ld (0x0005),a
        ld c,#UNABIOS_GET_HMA
        call #UNABIOS_ENTRY
        xor a
        dec hl
        ld (hl),a
        dec hl
        ld (hl),a

        ld a,#FIRST_LBA
        ld (next_lba),a
        xor a
        ld (loaded_sectors),a
        ld hl,#LOAD_ADDRESS
        ld (copy_address),hl

load_next:
        ; UNA SETLBA accepts the 28-bit LBA in DEHL.  Stage-1 is bounded to
        ; the pre-filesystem gap, therefore its high three bytes are zero.
        xor a
        ld d,a
        ld e,a
        ld h,a
        ld a,(next_lba)
        ld l,a
        ld c,#UNABIOS_SETLBA
        ld a,(boot_unit)
        ld b,a
        call #UNABIOS_ENTRY
        jp nz,boot_error

        ; Read one 512-byte sector into a safe high-memory staging buffer.
        ld c,#UNABIOS_READ
        ld a,(boot_unit)
        ld b,a
        ld l,#1
        ld de,#BUFFER
        call #UNABIOS_ENTRY
        jp nz,boot_error

        ; The first sector carries the raw stage-2 ABI header:
        ; "MUZ2", version 1, header size, payload length, sector count at byte 8
        ; and a 6-byte little-endian entry offset at bytes 10..15.
        ld a,(loaded_sectors)
        or a
        jr nz,copy_sector
        ld a,(BUFFER+0)
        cp #'M'
        jp nz,boot_error
        ld a,(BUFFER+1)
        cp #'U'
        jr nz,boot_error
        ld a,(BUFFER+2)
        cp #'Z'
        jr nz,boot_error
        ld a,(BUFFER+3)
        cp #'2'
        jr nz,boot_error
        ld a,(BUFFER+4)
        cp #STAGE2_VERSION
        jr nz,boot_error
        ld a,(BUFFER+5)
        cp #STAGE2_HEADER
        jr nz,boot_error
        ld a,(BUFFER+8)
        or a
        jr z,boot_error
        cp #(MAX_SECTORS + 1)
        jr nc,boot_error
        ld (sector_count),a
        ; Read the whole entry-offset field, not just its low word.  The
        ; payload starts at a fixed 16-byte offset inside the sector today,
        ; but the field is what the ABI defines the entry to be, and a loader
        ; that validates it and then ignores it is one refactor away from
        ; jumping somewhere the image never intended.  A non-zero upper half
        ; would put the entry past this sector, so reject it.
        ld hl,(BUFFER+10)
        ld de,#STAGE2_HEADER
        ld a,h
        or l
        jr z,boot_error
        or a
        sbc hl,de
        jr nz,boot_error
        ld hl,(BUFFER+12)
        ld a,l
        or h
        jr nz,boot_error
        ld de,(BUFFER+14)
        ld a,e
        or d
        jr nz,boot_error
        ld hl,#STAGE2_HEADER
        ld (entry_offset),hl

copy_sector:
        ld hl,#BUFFER
        ld de,(copy_address)
        ld bc,#512
        ldir
        ld (copy_address),de

        ld a,(next_lba)
        inc a
        ld (next_lba),a
        ld a,(loaded_sectors)
        inc a
        ld (loaded_sectors),a
        ld b,a
        ld a,(sector_count)
        cp b
        jp nz,load_next

        ; Enter the payload at the offset the header declared, relative to
        ; where this loader put it.  Using the field rather than a hardcoded
        ; LOAD_ADDRESS + STAGE2_HEADER keeps stage-1 honest if the header
        ; layout ever changes.
        ld hl,(entry_offset)
        ld de,#LOAD_ADDRESS
        add hl,de
        jp (hl)

boot_error:
        ; Falling into halt_forever also makes the entry-offset check's "carry
        ; clear" path safe: `or a / sbc hl,de` with a zero HL would land here.
        di
halt_forever:
        halt
        jr halt_forever

boot_unit:
        .db 0
next_lba:
        .db 0
loaded_sectors:
        .db 0
sector_count:
        .db 0
entry_offset:
        .dw STAGE2_HEADER
copy_address:
        .dw LOAD_ADDRESS
        .ds 0x1fe - (.-start)
        .dw 0xaa55
