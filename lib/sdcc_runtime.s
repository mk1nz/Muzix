.module muzix_sdcc_runtime

.globl ___sdcc_call_iy
.globl ___sdcc_call_hl

.area _CODE

; void ___sdcc_call_iy(void)
; Calls function pointer in IY
___sdcc_call_iy:
    jp (iy)

; void ___sdcc_call_hl(void)
; Calls function pointer in HL
___sdcc_call_hl:
    jp (hl)