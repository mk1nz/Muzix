; MUZIX UNA BIOS stage-2 bring-up entry.
;
; Stage-1 validates the MUZ2 header, maps the UNA user page, and enters here
; at 0x0088 + 16.  This deliberately small freestanding payload proves the
; reset-to-stage-2 handoff while the C kernel is still being made freestanding.

        .module muzix_una_stage2

ENTRY_ADDRESS         = 0x0098
STACK_TOP             = 0xfe00
; Bound on the transmit-ready wait.  Stage-1 enters here with interrupts off
; and there is no interrupt-driven path out of it, so an unbounded spin on a
; UART that never asserts THR-empty (unclocked, wedged, or a board with no
; serial fitted) would hang the machine with IF off and no way to observe it.
; One iteration of the countdown below is about 45 T-states, so 60000 is
; roughly 2.7 M T-states: about 0.37 s at the emulator's 7.3728 MHz profile
; (0.675 s at the board's 4 MHz U17 clock), versus the ~6400
; T-states (86 us) a single character takes at 115200 8N1.  Generous by four
; orders of magnitude, and bounded.
TX_TIMEOUT            = 60000

        .area _STAGE2 (ABS)
        .org ENTRY_ADDRESS

stage2_entry:
        di
        ld sp,#STACK_TOP

        ; Zeta V2 NS16550-compatible UART: 8N1, FIFO enabled, 115200 baud.
        ; Program divisor 1 (1.8432 MHz / (16 * 115200) = 1) with DLAB.
        ld a,#0x83
        out (0x6b),a
        ld a,#0x01
        out (0x68),a
        xor a
        out (0x69),a
        ld a,#0x03
        out (0x6b),a
        ld a,#0x07
        out (0x6a),a
        ld a,#0x03
        out (0x6c),a

        ld hl,#banner
print_next:
        ld a,(hl)
        or a
        jr z,entered
        inc hl
        ld e,a
        ld bc,#TX_TIMEOUT
wait_tx:
        in a,(0x6d)
        and #0x20        ; LSREM.THRE
        jr nz,tx_ready
        dec bc
        ld a,b
        or c
        jr nz,wait_tx
        ; Timed out.  Drop the rest of the banner and complete the firmware
        ; handoff: hanging here with IF off and no other exit is worse than
        ; silence on a board whose UART is not coming up.
        jr entered

tx_ready:
        ld a,e
        out (0x68),a
        jr print_next

entered:
        ; The firmware handoff is complete.  Do not invent an FDC ABI here;
        ; the future C kernel enters through this same raw-image contract.
        ei
idle:
        halt
        jr idle

banner:
        .ascii "MUZIX UNA stage-2 entered\r\n"
        .db 0
