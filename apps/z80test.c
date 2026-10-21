/*
 * z80test.c - Z80 CPU identification tests
 *
 * MIT License
 * Copyright (c) 2026 Muzix Project Contributors
 *
 * CPU identification using methods from z80type.asm (Sergey Kiselev):
 * Repository: https://github.com/skiselev/z80-tests
 * Methods based on research from z80.info:
 * - SCF/CCF YF/XF ("Q") theory: https://worldofspectrum.org/forums/discussion/41704
 * - Documented Z80: https://zilog.com/uploads/UM008008.pdf (Z80 CPU User Manual)
 * - Undocumented Z80: https://z80.info/zip/z80-documented.pdf (Sean Young, v0.91)
 * - NMOS/CMOS detection: OUT (C),0 (ED 71) instruction behavior
 * - MEMPTR/WZ register behavior: http://zx.pk.ru
 * - Interrupt acceptance timing: http://z80.info/interrupts.html
 * - TESTU880: OUTI carry flag behavior with HL=0FFFFh
 * - TESTXY: XF/YF signature via SCF/Q-flag theory (Patrik Rak)
 *
 * UART SCR port 0x6F used as read/write test port (safe, preserves value).
 */

#include "../lib/libc.h"
#include "z80test.h"

#define PORT_SCR 0x6F

static const char *cpu_names[] = {
    "Unknown",
    "Zilog Z80/Z08400 or similar NMOS",
    "Zilog Z84C00 (CMOS)",
    "Hitachi HD64180/Zilog Z180",
    "NEC D780C/D780C-1",
    "Toshiba TMPZ84C00AP/ST Z84C00AB",
    "Sharp LH5080A",
    "MME U880 (newer/Thesys)",
    "MME U880 (older)",
    "ST Z8400AB1 (NMOS)"
};

/*
 * TESTCMOS - NMOS/CMOS detection via OUT (C),0 (ED 71)
 *
 * On a clean port: NMOS reads back 0x00, CMOS reads back 0xFF.
 * Port 0x6F (UART SCR) has reserved bits 4-7 that read as 1 on all CPUs,
 * so NMOS reads 0xF0 (not 0x00) and CMOS reads 0xFF.
 *
 * This test returns:
 *   0 = NMOS (lower 4 bits read 0)
 *   1 = CMOS (lower 4 bits read F)
 *   0xFF = unknown (neither 0 nor F in lower nibble)
 */
static uint8_t test_cmos(void)
{
    uint8_t val;

    printf("\nTest 1: CMOS/NMOS detection (OUT (C),0)\n");
    printf("  Port 0x%02x (UART SCR)\n", PORT_SCR);

    val = z80_out_c_zero(PORT_SCR);
    printf("  Read-back: 0x%02x\n", val);

    /* Check lower 4 bits only, ignoring reserved bits 4-7 */
    if ((val & 0x0F) == 0) {
        printf("  -> NMOS (OUT (C),0 writes 0 in lower nibble)\n");
        return 0;
    } else if ((val & 0x0F) == 0x0F) {
        printf("  -> CMOS (OUT (C),0 writes 0xFF in lower nibble)\n");
        return 1;
    }
    printf("  -> Unknown (expected 0x00 or 0x0F in lower 4 bits)\n");
    return 0xFF;
}

/*
 * TESTU880 - Check if CPU is MME U880
 * Uses OUTI with HL=0FFFFh, B=01h, CF=1
 * U880: CF preserved, others: CF cleared
 */
static uint8_t test_u880(void)
{
    uint8_t result;

    printf("\nTest 2: U880 detection (OUTI CF behavior)\n");
    result = z80_test_u880();
    printf("  HL=0xFFFF, B=01h, CF=1, OUTI\n");
    printf("  CF after OUTI: %d\n", result);

    if (result) {
        printf("  -> U880-like (CF preserved)\n");
    } else {
        printf("  -> Not U880 (CF cleared)\n");
    }
    return result;
}

/*
 * TESTXY - Tests how SCF affects YF (bit 5) and XF (bit 3)
 *
 * Implements the full z80type algorithm:
 * - 255 iterations with counter C
 * - 4 combinations per iteration:
 *   1. F=0, A = (C|0x20)&0xF7 -> check YF
 *   2. F=0, A = (C|0x08)&0xDF -> check XF
 *   3. F = (C|0x20)&0xF7, A=0 -> check YF
 *   4. F = (C|0x08)&0xDF, A=0 -> check XF
 *
 * Returns 4-bit signature:
 *   bits 7:6 - YF case 1
 *   bits 5:4 - XF case 2
 *   bits 3:2 - YF case 3
 *   bits 1:0 - XF case 4
 *
 * Encoding per pair: 00=always 0, 11=always 1, 01=mostly 0, 10=mostly 1
 */
static uint8_t test_xy(void)
{
    uint8_t c = 0xFF;
    uint8_t xf[4] = {0}, yf[4] = {0};
    uint8_t f;
    uint8_t a_val, f_val;
    uint8_t sig = 0;
    uint8_t shift, i;

    printf("\nTest 3: XF/YF signature (SCF behavior)\n");
    printf("  255 iterations x 4 combinations\n");

    do {
        /* Case 1: F=0, A = (C|0x20)&0xF7 */
        a_val = (c | 0x20) & 0xF7;
        f = z80_test_scf_flags(a_val, 0);
        if (f & Z80_YF) yf[0]++;

        /* Case 2: F=0, A = (C|0x08)&0xDF */
        a_val = (c | 0x08) & 0xDF;
        f = z80_test_scf_flags(a_val, 0);
        if (f & Z80_XF) xf[1]++;

        /* Case 3: F = (C|0x20)&0xF7, A=0 */
        f_val = (c | 0x20) & 0xF7;
        f = z80_test_scf_flags(0, f_val);
        if (f & Z80_YF) yf[2]++;

        /* Case 4: F = (C|0x08)&0xDF, A=0 */
        f_val = (c | 0x08) & 0xDF;
        f = z80_test_scf_flags(0, f_val);
        if (f & Z80_XF) xf[3]++;

        c--;
    } while (c != 0);

    printf("  Counters (out of 255):\n");
    printf("    Case1 YF=%d\n", yf[0]);
    printf("    Case2 XF=%d\n", xf[1]);
    printf("    Case3 YF=%d\n", yf[2]);
    printf("    Case4 XF=%d\n", xf[3]);

    /* Form 4-bit signature following z80type pattern */
    /* Each count: 0=00, 0xFF=11, <0x80=01, >=0x80=10 */
    uint8_t counts[4] = {yf[0], xf[1], yf[2], xf[3]};
    for (i = 0; i < 4; i++) {
        shift = (3 - i) * 2;
        if (counts[i] == 0) sig |= (0 << shift);
        else if (counts[i] == 0xFF) sig |= (3 << shift);
        else if (counts[i] < 0x80) sig |= (1 << shift);
        else sig |= (2 << shift);
    }

    printf("  Signature: 0x%02x\n", sig);
    return sig;
}

int main(void)
{
    uint8_t cmos, u880, xy_sig;

    printf("Z80 CPU Identification\n");
    printf("======================\n");
    printf("Algorithm: adapted from z80type.asm (Sergey Kiselev)\n");
    printf("Source: https://github.com/skiselev/z80-tests\n");
    printf("Research: z80.info (Sean Young, Patrik Rak, Achim Flammenkamp)\n\n");

    cmos = test_cmos();
    u880 = test_u880();
    xy_sig = test_xy();

    printf("\n==============================\n");
    printf("Classification:\n");
    printf("  CMOS: %s\n", cmos ? "yes" : (cmos == 0xFF ? "unknown" : "no"));
    printf("  U880: %s\n", u880 ? "yes" : "no");
    printf("  XF/YF sig: 0x%02x\n", xy_sig);

    /* Full z80type classification table */
    if (cmos == 0xFF) {
        printf("  CPU: %s\n", cpu_names[0]);
        printf("  (CMOS test inconclusive - port has reserved bits)\n");
        printf("  XF/YF sig: 0x%02x\n", xy_sig);
        /* Use XF/YF signature as fallback for NMOS identification */
        if (!u880 && xy_sig == 0x33) {
            printf("  -> Likely: %s (ST Z8400AB1 NMOS)\n", cpu_names[9]);
        }
    } else if (cmos) {
        /* CMOS processors */
        if (xy_sig == 0xFF) {
            printf("  CPU: %s\n", cpu_names[2]);
            printf("  (Zilog Z84C00 - CMOS Z80)\n");
        } else if (xy_sig == 0x3F) {
            printf("  CPU: %s\n", cpu_names[5]);
            printf("  (Toshiba TMPZ84C00AP/ST Z84C00AB - CMOS)\n");
        } else {
            printf("  CPU: Unknown CMOS (sig=0x%02x)\n", xy_sig);
            printf("  (CMOS but XF/YF signature not recognized)\n");
        }
    } else {
        /* NMOS processors */
        printf("  (NMOS behavior detected)\n");

        if (u880 && (xy_sig == 0x30 || xy_sig == 0xFF)) {
            /* Valid U880 signature */
            if (xy_sig == 0xFF) {
                printf("  CPU: %s\n", cpu_names[8]);
                printf("  (MME U880 (older) or Thesys)\n");
            } else {
                printf("  CPU: %s\n", cpu_names[7]);
                printf("  (MME U880 (newer/Thesys))\n");
            }
        } else if (u880 && xy_sig != 0x30 && xy_sig != 0xFF) {
            /* U880 detected but signature doesn't match - false positive */
            if (xy_sig == 0xFD) {
                printf("  CPU: %s (NEC D780C)\n", cpu_names[4]);
            } else if (xy_sig == 0xF0) {
                printf("  CPU: %s (NEC D780C-1)\n", cpu_names[4]);
            } else if (xy_sig == 0x33) {
                printf("  CPU: %s\n", cpu_names[9]);
                printf("  (XF=1, YF=0 - non-standard ST Z8400)\n");
            } else if (xy_sig == 0xFF) {
                printf("  CPU: %s\n", cpu_names[1]);
                printf("  (Mostek MK3880N, SGS/ST Z8400, Sharp LH0080A, KR1858VM1)\n");
            } else if (xy_sig == 0xF4) {
                printf("  CPU: %s\n", cpu_names[1]);
                printf("  (KR1858VM1 - overclocked variant)\n");
            } else {
                printf("  CPU: Unknown NMOS (sig=0x%02x)\n", xy_sig);
            }
        } else if (!u880) {
            /* NMOS non-U880 classification */
            if (xy_sig == 0xFF) {
                printf("  CPU: %s\n", cpu_names[1]);
                printf("  (Zilog Z80/Z08400 or similar NMOS)\n");
                printf("  (Mostek MK3880N, SGS/ST Z8400, Sharp LH0080A, KR1858VM1)\n");
                printf("  (Mostek MK3880N, SGS/ST Z8400, Sharp LH0080A, KR1858VM1)\n");
            } else if (xy_sig == 0xFD) {
                printf("  CPU: %s (NEC D780C)\n", cpu_names[4]);
            } else if (xy_sig == 0xF0) {
                printf("  CPU: %s (NEC D780C-1)\n", cpu_names[4]);
            } else if (xy_sig == 0xF4) {
                printf("  CPU: %s\n", cpu_names[1]);
                printf("  (KR1858VM1 - overclocked variant)\n");
            } else if (xy_sig == 0x33) {
                printf("  CPU: %s\n", cpu_names[9]);
                printf("  (XF=1, YF=0 - non-standard ST Z8400 behavior)\n");
            } else {
                printf("  CPU: Unknown NMOS (sig=0x%02x)\n", xy_sig);
            }
        } else {
            printf("  CPU: Unknown NMOS (u880=%d, sig=0x%02x)\n", u880, xy_sig);
        }
    }

    return 0;
}
