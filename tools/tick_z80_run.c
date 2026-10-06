/*
 * tick_z80_run.c - run the real platform/zeta-v2/tick.s as a linked Z80 image
 * and report what each of the three readers actually returned.
 *
 * Why this exists
 * ---------------
 * Every host test of the tick module tests a C model of it: test_tick.c calls
 * muzix_host_tick, a uint16_t in test_host/z80_io_host.c.  A C model has one
 * byte order, one register file and no multi-byte object, so it cannot see any
 * of the faults that were in tick.s for a month - a reader that took the low
 * byte of the counter first put the deadline in window 0, and the model agreed
 * with it perfectly.
 *
 * So the routine has to be tested as what it is: an assembled image, run.  This
 * program assembles nothing and expects nothing; it is handed a linked image
 * and a link map (tools/tick_z80.rb builds both) and drives it through the
 * tree's own Z80 core - emulator/z80/z80.c, the same core the ROM is booted
 * on, so a disagreement between this and the machine is a disagreement about
 * the image rather than about the emulator.
 *
 * The window-0 read counter is the part that makes F1 a measurement.  The image
 * is linked at _CODE = 0x8000 / _DATA = 0x9000, so window 0 holds nothing at
 * all and any read of it is a read of a counter byte's low half used as an
 * address high half.  It is counted and reported rather than asserted, because
 * a reader that stops doing it is worth knowing about even while every value it
 * returns is right.
 *
 * Usage: tick_z80_run <image.ihx> <image.map>
 *
 * Exit status is 0 only if every case matched.  It does not print a summary
 * that says "OK" anywhere: the caller is a test, and a test that cannot fail
 * cannot pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "z80.h"

#define SENTINEL 0xA5u

static uint8_t mem[65536];

/* Reads below 0x4000, with the value found there.  The image is linked above
 * 0x8000 and its stack is at 0xFF00, so nothing the harness itself does reads
 * window 0: every hit here is the counter reader leaving the object. */
static unsigned long w0_reads;
static unsigned      w0_addr[64];
static unsigned      w0_val[64];
static unsigned      nw0;

static uint8_t rd(void *u, uint16_t a)
{
    (void)u;
    if (a < 0x4000) {
        w0_reads++;
        if (nw0 < 64) { w0_addr[nw0] = a; w0_val[nw0] = mem[a]; nw0++; }
    }
    return mem[a];
}
static void wr(void *u, uint16_t a, uint8_t v) { (void)u; mem[a] = v; }
static uint8_t pin(z80 *u, uint8_t p) { (void)u; (void)p; return 0xFF; }
static void    pout(z80 *u, uint8_t p, uint8_t v) { (void)u; (void)p; (void)v; }
static struct z80 cpu;

static void load(const char *f)
{
    memset(mem, 0, 65536);
    /* window 0 and everything above the two linked areas get a marker, so a
     * byte the image does not account for reads as the marker and not as a
     * plausible zero */
    memset(mem + 0x0000, SENTINEL, 0x4000);
    memset(mem + 0xA000, SENTINEL, 0x6000);
    FILE *h = fopen(f, "r");
    char line[600];
    if (!h) { fprintf(stderr, "tick_z80_run: no image %s\n", f); exit(2); }
    while (fgets(line, sizeof line, h)) {
        unsigned len = 0, addr = 0, type = 0, i;
        if (line[0] != ':') continue;
        if (sscanf(line + 1, "%2x%4x%2x", &len, &addr, &type) != 3) continue;
        if (type != 0) continue;
        for (i = 0; i < len; i++) {
            unsigned b = 0;
            if (sscanf(line + 9 + 2 * i, "%2x", &b) != 1) break;
            mem[addr + i] = (uint8_t)b;
        }
    }
    fclose(h);
}

/* Addresses come out of the link map rather than out of constants here, so the
 * runner and the linker cannot disagree about where the harness put its inputs
 * and both be right. */
static unsigned sym(const char *map, const char *name)
{
    FILE *h = fopen(map, "r");
    char line[600];
    if (!h) { fprintf(stderr, "tick_z80_run: no map %s\n", map); exit(2); }
    while (fgets(line, sizeof line, h)) {
        unsigned a;
        char nm[128];
        if (sscanf(line, " %x %127s", &a, nm) == 2 && strcmp(nm, name) == 0) {
            fclose(h);
            return a;
        }
    }
    fclose(h);
    fprintf(stderr, "tick_z80_run: symbol %s not in %s\n", name, map);
    exit(2);
}

static uint16_t r16(unsigned a) { return (uint16_t)(mem[a] | (mem[a + 1] << 8)); }
static uint32_t r32(unsigned a) { return (uint32_t)r16(a) | ((uint32_t)r16(a + 2) << 16); }

static unsigned A_caseval, A_deadline, A_aptcnt, A_aptstamp, A_aptnow;
static unsigned A_resnow, A_resexp, A_resapt, A_resstamp, A_resstampout, A_resisr;
static unsigned A_ctl, A_start;
static const char *img;

static unsigned failures;

static void run(void)
{
    w0_reads = 0;
    nw0 = 0;
    z80_init(&cpu);
    cpu.read_byte = rd; cpu.write_byte = wr; cpu.port_in = pin; cpu.port_out = pout;
    cpu.pc = (uint16_t)A_start; cpu.sp = 0xFF00; cpu.halted = 0;
    for (long k = 0; k < 200000; k++) {
        z80_step(&cpu);
        if (cpu.halted) break;
    }
    if (!cpu.halted) {
        fprintf(stderr, "tick_z80_run: the harness never halted - it has looped\n");
        exit(2);
    }
}

static void fail(const char *what)
{
    printf("      FAIL  %s\n", what);
    failures++;
}

int main(int argc, char **argv)
{
    unsigned i;
    const char *map;

    if (argc < 3) {
        fprintf(stderr, "usage: tick_z80_run <image.ihx> <image.map>\n");
        return 2;
    }
    img  = argv[1];
    map  = argv[2];
    A_caseval     = sym(map, "h_caseval");
    A_deadline    = sym(map, "h_deadline");
    A_aptcnt      = sym(map, "h_aptcnt");
    A_aptstamp    = sym(map, "h_aptstamp");
    A_aptnow      = sym(map, "h_aptnow");
    A_resnow      = sym(map, "h_resnow");
    A_resexp      = sym(map, "h_resexp");
    A_resapt      = sym(map, "h_resapt");
    A_resstamp    = sym(map, "h_resstamp");
    A_resstampout = sym(map, "h_resstampout");
    A_resisr      = sym(map, "h_resisr");
    A_ctl         = sym(map, "_muzix_tick_count");
    A_start       = sym(map, "_start");

    /* ---- muzix_tick_now: the 16-bit return is in DE --------------------- */
    {
        /* Both halves differ from each other in every case, and no two cases
         * are byte-swaps of one another, so a reader that assembles the two
         * bytes in the wrong order fails every case here rather than some. */
        static const uint16_t NOW[8] =
            { 0x0000u, 0x0124u, 0x00FFu, 0x7FFFu, 0x8000u, 0xFF24u, 0x1234u, 0xFFFFu };
        printf("      muzix_tick_now (return in DE):\n");
        for (i = 0; i < 8; i++) {
            uint16_t got;
            load(img);
            mem[A_caseval]     = (uint8_t)(NOW[i] & 0xFFu);
            mem[A_caseval + 1] = (uint8_t)(NOW[i] >> 8);
            run();
            got = r16(A_resnow);
            if (got != NOW[i]) {
                printf("      FAIL  counter %04X returned %04X\n", NOW[i], got);
                failures++;
            }
        }
    }

    /* ---- muzix_tick_expired --------------------------------------------- */
    {
        /* The rule, from tick.h and from every caller: expired exactly when
         * the deadline is in the past half of the range.  Four of these
         * deadlines cross the counter's 0xFFFF -> 0x0000 wrap and two sit on
         * the half-range boundary, because that is where a comparison against
         * the CARRY instead of the SIGN goes wrong while looking right
         * everywhere else. */
        static const struct { uint16_t now, dl; long rel; } EX[10] = {
            { 0x0000u, 0x0001u,      1L }, { 0x0124u, 0x0124u,      0L },
            { 0x00FFu, 0x0100u,      1L }, { 0x8000u, 0x8004u,      4L },
            { 0xFFFFu, 0x0006u,      7L }, { 0xFFF9u, 0x0001u,      8L },
            { 0x0001u, 0xFFF9u,     -8L }, { 0x0000u, 0xFFFFu,     -1L },
            { 0x0000u, 0x8000u, -32768L }, { 0x8000u, 0x0000u, -32768L },
        };
        printf("      muzix_tick_expired (deadline in HL, result in A):\n");
        for (i = 0; i < 10; i++) {
            /* (uint16_t) before the comparison: now - deadline in int
             * arithmetic wraps to a large positive number, and without the
             * cast every deadline ahead of the counter reads as past. */
            unsigned want = ((unsigned)(uint16_t)(EX[i].now - EX[i].dl) < 0x8000u) ? 1u : 0u;
            unsigned got;
            load(img);
            mem[A_caseval]     = (uint8_t)(EX[i].now & 0xFFu);
            mem[A_caseval + 1] = (uint8_t)(EX[i].now >> 8);
            mem[A_deadline]    = (uint8_t)(EX[i].dl & 0xFFu);
            mem[A_deadline + 1]= (uint8_t)(EX[i].dl >> 8);
            run();
            got = mem[A_resexp];
            if (got != want) {
                printf("      FAIL  now %04X deadline %04X (%+ld ticks): "
                       "expected %u, got %u\n", EX[i].now, EX[i].dl, EX[i].rel, want, got);
                failures++;
            }
        }
    }

    /* ---- muzix_tick_add_process_time ------------------------------------ */
    {
        /* The charges are chosen so that a byte-swapped difference is wrong on
         * all of them: three cross a 256-boundary, one crosses the counter's
         * own wrap, one is large enough that the inflation factor is visible
         * in the answer, and the last two are the cheap cases a fix has to get
         * right too. */
        static const struct { uint32_t cnt; uint16_t stamp, now; } APT[8] = {
            { 0x00000000u, 0x0002u, 0x0005u },
            { 0x0000FFFFu, 0x1231u, 0x1234u },
            { 0x00000000u, 0x7FFDu, 0x8000u },
            { 0x00000010u, 0xFFF0u, 0x0000u },
            { 0x00000005u, 0x0100u, 0xF000u },
            { 0x00000000u, 0x0410u, 0x0411u },
            { 0x00000000u, 0x0001u, 0xFFFFu },
            { 0x00000000u, 0x4000u, 0x4000u },
        };
        printf("      muzix_tick_add_process_time (counter in HL, stamp in DE):\n");
        for (i = 0; i < 8; i++) {
            uint32_t before = APT[i].cnt;
            uint32_t want   = before + (uint16_t)(APT[i].now - APT[i].stamp);
            uint32_t after;
            uint16_t stamp2;
            load(img);
            mem[A_aptcnt]     = (uint8_t)(before & 0xFFu);
            mem[A_aptcnt + 1] = (uint8_t)((before >> 8) & 0xFFu);
            mem[A_aptcnt + 2] = (uint8_t)((before >> 16) & 0xFFu);
            mem[A_aptcnt + 3] = (uint8_t)((before >> 24) & 0xFFu);
            mem[A_aptstamp]   = (uint8_t)(APT[i].stamp & 0xFFu);
            mem[A_aptstamp + 1]= (uint8_t)(APT[i].stamp >> 8);
            mem[A_aptnow]     = (uint8_t)(APT[i].now & 0xFFu);
            mem[A_aptnow + 1] = (uint8_t)(APT[i].now >> 8);
            run();
            after   = r32(A_resapt);
            stamp2  = r16(A_resstamp);
            if (after != want) {
                printf("      FAIL  charge %04X onto %08lX: expected %08lX, got %08lX\n",
                       (unsigned)(uint16_t)(APT[i].now - APT[i].stamp),
                       (unsigned long)before, (unsigned long)want, (unsigned long)after);
                failures++;
            }
            if (stamp2 != APT[i].now) {
                printf("      FAIL  restamp: expected %04X, got %04X\n", APT[i].now, stamp2);
                failures++;
            }
        }
    }

    /* ---- the stamp store and the handler, which the readers rely on ----- */
    load(img);
    run();
    printf("      muzix_tick_stamp_store at 0x1234, and the ISR from 0x00FE:\n");
    if (mem[A_resstampout] != 0x34 || mem[A_resstampout + 1] != 0x12) {
        printf("      FAIL  stamp_store wrote %02X %02X, expected 34 12"
               " (low byte at the label)\n",
               mem[A_resstampout], mem[A_resstampout + 1]);
        failures++;
    }
    if (mem[A_resisr] != 0x01 || mem[A_resisr + 1] != 0x01) {
        printf("      FAIL  three ticks from 0x00FE gave %02X %02X, expected 01 01\n",
               mem[A_resisr], mem[A_resisr + 1]);
        failures++;
    }

    /* ---- and the window-0 read count, which is the F1 measurement -------- */
    printf("      counter at %04X (window 3); window-0 reads in one run with "
           "the counter at 0x0124: ", A_ctl);
    load(img);
    mem[A_caseval] = 0x24; mem[A_caseval + 1] = 0x01;
    mem[A_deadline] = 0x24; mem[A_deadline + 1] = 0x01;
    run();
    if (w0_reads != 0) {
        unsigned k, n = nw0 < 8 ? nw0 : 8;
        printf("%lu, first", w0_reads);
        for (k = 0; k < n; k++) printf(" %04X=%02X", w0_addr[k], w0_val[k]);
        printf("\n");
        fail("a reader fetched a counter byte from window 0");
    } else {
        printf("0\n");
    }

    if (failures) {
        printf("      tick_z80: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("      tick_z80: 26 cases, 0 wrong, 0 window-0 reads\n");
    return 0;
}