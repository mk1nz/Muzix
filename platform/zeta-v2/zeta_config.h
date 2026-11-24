/*
 * zeta_config.h - Zeta SBC V2 platform configuration
 *
 * Centralizes every platform-specific constant so the kernel can be
 * retargeted to another board by editing this one file instead of hunting
 * hardcoded addresses through the source.
 *
 * Everything here is a compile-time constant.  Nothing in this header is
 * board-specific logic - it is purely the numeric interface between the
 * kernel and the Zeta SBC V2 hardware.
 *
 * To port Muzix to a different board, create a new platform directory
 * (e.g. platform/other-board/) with its own zeta_config.h and change the
 * Makefile's vpath/include path.  The kernel sources must not change.
 */

#ifndef MUZIX_ZETA_CONFIG_H
#define MUZIX_ZETA_CONFIG_H

#include "ztypes.h"

/* =========================================================================
 * Bank switching and memory windows (Zeta SBC V2)
 * =========================================================================
 *
 * The Z80 has four 16 KiB windows controlled by bank-select ports $78-$7B,
 * and a paging enable at $7C.  Window 0 is always the kernel's page; window 3
 * is also kernel at runtime; windows 1-2 are process text/stack.
 */

/* Bank-select I/O ports (write-only in strict hardware mode). */
#define MUZIX_ZETA_BANK_PORT_0   0x78u   /* Window 0: $0000-$3FFF */
#define MUZIX_ZETA_BANK_PORT_1   0x79u   /* Window 1: $4000-$7FFF */
#define MUZIX_ZETA_BANK_PORT_2   0x7au   /* Window 2: $8000-$BFFF */
#define MUZIX_ZETA_BANK_PORT_3   0x7bu   /* Window 3: $C000-$FFFF */
#define MUZIX_ZETA_PAGING_PORT   0x7cu   /* MPGENA: paging enable, bit 0 */

/* Kernel physical page IDs (allocator RAM: 0x20-0x3F). */
#define MUZIX_ZETA_KERNEL_BANK_0  32u   /* 0x20 - window 0, always kernel */
#define MUZIX_ZETA_KERNEL_BANK_1  33u   /* 0x21 - process stack */
#define MUZIX_ZETA_KERNEL_BANK_2  34u   /* 0x22 - process text */
#define MUZIX_ZETA_KERNEL_BANK_3  35u   /* 0x23 - window 3, always kernel */

/* The four kernel pages together form the kernel map; windows 1-2 of a
 * process map are the process's own pages. */
#define MUZIX_ZETA_KERNEL_BANK_COUNT  4u

/* Window address boundaries (16 KiB each). */
#define MUZIX_ZETA_WINDOW_0_START  0x0000u
#define MUZIX_ZETA_WINDOW_0_END    0x3FFFu
#define MUZIX_ZETA_WINDOW_1_START  0x4000u
#define MUZIX_ZETA_WINDOW_1_END    0x7FFFu
#define MUZIX_ZETA_WINDOW_2_START  0x8000u
#define MUZIX_ZETA_WINDOW_2_END    0xBFFFu
#define MUZIX_ZETA_WINDOW_3_START  0xC000u
#define MUZIX_ZETA_WINDOW_3_END    0xFFFFu

/* =========================================================================
 * Kernel link layout
 * =========================================================================
 *
 * The kernel link is driven by the Makefile, not by kernel/kernel.lk.
 * _CODE starts at 0x0098 (fixed boot-stub entry); _DATA is derived from the
 * linked _CODE extent by tools/kernel_data_base.rb.  These are the compile-time
 * counterparts of those link-time values.
 */

/* Kernel entry point - the boot stub jumps here after copying ROM pages
 * 0-3 to RAM pages 32-35. */
#define MUZIX_ZETA_KERNEL_ENTRY   0x0098u

/* End of kernel _CODE / base of kernel _DATA, derived from the link map.
 * tools/kernel_data_base.rb prints the current value; the Makefile links
 * twice, using this value on the second pass.  0xEA00 is the value at the
 * time of writing; regenerate after any code-size change. */
#define MUZIX_ZETA_KERNEL_DATA_BASE  0xEA00u

/* Minimum headroom below 0x10000 required by tools/check_kernel_layout.rb. */
#define MUZIX_ZETA_KERNEL_HEADROOM_MIN  1024u

/* =========================================================================
 * Userspace link layout
 * =========================================================================
 *
 * Userspace programs link at _CODE = 0x8000; _DATA follows _CODE by the
 * linker default.  There is no -b _DATA = 0x9000 any more. */

#define MUZIX_ZETA_USER_CODE_BASE   0x8000u

/* =========================================================================
 * ROM image layout
 * =========================================================================
 *
 * The ROM is 512 KiB: boot stub at 0x0000, kernel at 0x0098, MINIX-v1
 * filesystem at 0x20000. */

/* Boot stub address - copies ROM pages 0-3 to RAM pages 32-35, then jp to
 * MUZIX_ZETA_KERNEL_ENTRY. */
#define MUZIX_ZETA_BOOT_STUB_ADDR   0x0000u

/* MINIX-v1-format filesystem offset in ROM (384 KiB). */
#define MUZIX_ZETA_ROMFS_OFFSET     0x20000u

/* ROM image total size (512 KiB). */
#define MUZIX_ZETA_ROM_SIZE         (512u * 1024u)

/* =========================================================================
 * Interrupt vectors (IM2 mode)
 * =========================================================================
 *
 * The board runs IM2.  The vector table is at 0x0080; channel 1's tick
 * arrives as vector 0x82 and the UART's receive interrupt as 0x84.  An IM1
 * JP to the tick handler is planted at 0x0038 as a floor under the mode the
 * boot stub leaves behind. */

/* Syscall trap vector: CALL 0x0030 from userspace. */
#define MUZIX_ZETA_SYSCALL_VECTOR   0x0030u

/* IM1 fallback tick vector (JP to tick handler at 0x0038). */
#define MUZIX_ZETA_TICK_IM1_VECTOR  0x0038u

/* IM2 vector table base. */
#define MUZIX_ZETA_IM2_TABLE_BASE   0x0080u
#define MUZIX_ZETA_IM2_TICK_VECTOR  0x82u
#define MUZIX_ZETA_IM2_UART_VECTOR  0x84u

/* =========================================================================
 * Platform I/O devices
 * =========================================================================
 */

/* NS16550-compatible UART base port. */
#define MUZIX_ZETA_UART_PORT   0x68u

/* DS1302 RTC port (bit-banged).  Individual bits live in rtc_ds1302.h. */
#define MUZIX_ZETA_RTC_PORT    0x70u

/* 8255 PPI ports. */
#define MUZIX_ZETA_PPI_PORT_A  0x60u   /* Port A (8 bits) */
#define MUZIX_ZETA_PPI_PORT_B  0x61u   /* Port B (8 bits) */
#define MUZIX_ZETA_PPI_PORT_C  0x62u   /* Port C (D0-D7) */
#define MUZIX_ZETA_PPI_CTL_PORT 0x63u  /* Control register */

/* =========================================================================
 * Boot-time diagnostics
 * =========================================================================
 */

/* Trace enable flag: read at boot to decide whether to enable the trace
 * ring buffer.  Address 0xFC00 is common memory in z80pack mode; on real
 * hardware it is a port the boot stub leaves in a known state. */
#define MUZIX_ZETA_TRACE_FLAG_ADDR   0xFC00u

/* =========================================================================
 * Memory management
 * =========================================================================
 */

/* Staging buffer size for cross-bank copies (256 bytes = one ROM block).
 * Defined here once; platform/zeta-v2/context_switch.h and
 * servers/mm/mm_copy.c each have their own copy - they must stay in sync. */
#define MUZIX_ZETA_COPY_STAGING   256u

/* =========================================================================
 * Init task memory layout
 * =========================================================================
 *
 * The init task (slot 0) is laid out with text, data and stack at fixed
 * virtual addresses.  These are the boot-time values before the first
 * exec replaces them. */

/* Init task virtual addresses (window-relative offsets within a 16 KiB
 * page).  Window 0 is kernel, so init lives in windows 1-2. */
#define MUZIX_ZETA_INIT_TEXT_VADDR   0x0000u   /* window 1 offset */
#define MUZIX_ZETA_INIT_DATA_VADDR   0x1000u   /* window 1 offset */
#define MUZIX_ZETA_INIT_STACK_VADDR  0x2000u   /* window 2 offset */

/* =========================================================================
 * Process accounting
 * =========================================================================
 */

/* Sentinel deadline value for "park forever" (woken by IPC only). */
#define MUZIX_ZETA_PARK_FOREVER   0xFFFFu

#endif /* MUZIX_ZETA_CONFIG_H */
