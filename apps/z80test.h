/*
 * z80test.h - Z80 CPU identification test interface
 *
 * MIT License
 * Copyright (c) 2026 Muzix Project Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * CPU identification methods based on z80type.asm (Sergey Kiselev):
 * Repository: https://github.com/skiselev/z80-tests
 * Research sources from z80.info:
 * - Patrik Rak's Q theory for CCF/SCF YF/XF behavior (z80.info forums)
 * - Sean Young, "The Undocumented Z80 Documented" (z80.info/zip/z80-documented.pdf)
 * - MEMPTR/WZ leakage through BIT instructions (zx.pk.ru / redcode wiki)
 * - SLL undocumented shift detection
 */

#ifndef MUZIX_Z80TEST_H
#define MUZIX_Z80TEST_H

#include "../lib/libc.h"

/* Flag bit positions */
#define Z80_SF  0x80
#define Z80_ZF  0x40
#define Z80_YF  0x20
#define Z80_HF  0x10
#define Z80_XF  0x08
#define Z80_PVF 0x04
#define Z80_NF  0x02
#define Z80_CF  0x01

/* Assembly helpers - SDCC calling convention:
 * param1 (uint8_t) -> L, param2 (uint8_t) -> E, return (uint16_t) -> HL */
extern uint8_t z80_out_c_zero(uint8_t port);
extern uint8_t z80_test_u880(void);
extern uint8_t z80_test_inward_f_bit(void);
extern uint16_t z80_test_scf_flags(uint8_t a_val, uint8_t f_val);
extern uint16_t z80_test_ccf_flags(uint8_t a_val, uint8_t f_val);
extern uint8_t z80_sll_a(uint8_t val);
extern uint16_t z80_test_bit_hl(uint8_t addr_high);

#endif /* MUZIX_Z80TEST_H */
