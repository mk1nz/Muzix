#ifndef MUZIX_ZETA_BANK_IO_H
#define MUZIX_ZETA_BANK_IO_H

#include <stdint.h>

#define MUZIX_ZETA_BANK_PORT_0 0x78u
#define MUZIX_ZETA_BANK_PORT_1 0x79u
#define MUZIX_ZETA_BANK_PORT_2 0x7au
#define MUZIX_ZETA_BANK_PORT_3 0x7bu
#define MUZIX_ZETA_PAGING_PORT  0x7cu

#define MUZIX_ZETA_KERNEL_BANK_0 32u
#define MUZIX_ZETA_KERNEL_BANK_1 33u
#define MUZIX_ZETA_KERNEL_BANK_2 34u
#define MUZIX_ZETA_KERNEL_BANK_3 35u


/* Staged ldir.  Set the three words, then call with no arguments - see the
 * comment on muzix_zeta_ldir_staged in bank_io.s for why the source, the
 * destination and the length do not travel in registers. */
extern uint16_t muzix_zeta_staged_src;
extern uint16_t muzix_zeta_staged_dst;
extern uint16_t muzix_zeta_staged_len;
void muzix_zeta_ldir_staged(void);


/* What this module still exports, after 2026-10-02.

 * The six that used to be here - switch_bank, enable_paging, enter_kernel_banks,
 * restore_user_banks, set_cached_banks and copy_window3 - were REMOVED, and the
 * reason is worth keeping: none of them had a single call site in the tree.  What
 * remained were their declarations here and comments describing them, in four
 * files, which is a thing that looks like an API and is not one.  They were also
 * exactly what the memory-manager rule forbids: manual bank switching from
 * outside the MM, which is why they were never wired up in the first place.
 *
 * So this header now describes what actually exists, and the bank register
 * discipline belongs to kernel_bank_model.c and kernel/mm_copy.c.
 */
/* Returns nonzero when the calling function resumes in window 0
 * ($0000-$3FFF), the only window that keeps mapping kernel page 0x20 across a
 * bank switch.  Call it directly from the public entry point that is about to
 * write a bank register: a helper in between would report an address inside
 * the platform layer, which is in window 0 by construction. */
uint8_t muzix_zeta_caller_in_window0(void);

#endif
