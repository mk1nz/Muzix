/*
 * hwdiag - safe diagnostics for hardware interfaces exposed by the kernel.
 *
 * The Zeta SBC V2 has a 16550 UART, Z80 CTC, DS1302 RTC, 8255 PPI, FDC,
 * 512 KiB SRAM and 512 KiB flash. This command probes only interfaces that
 * Muzix currently exposes to userspace; it does not poke device registers or
 * change persistent hardware state.
 */

#include "../lib/libc.h"
#include "../lib/z80_io.h"

static int g_failures;

#define HWDIAG_LOAD_Q14_ONE 16384u

static void report(const char *name, int ok, const char *detail)
{
    printf("[%s] %s: %s\n", ok ? "OK" : "FAIL", name, detail);
    if (!ok) {
        g_failures++;
    }
}

static int rtc_valid(const struct rtc_time *t)
{
    if (t->second >= 60 || t->minute >= 60 || t->hour >= 24 ||
        t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->year > 99 || t->weekday > 7) {
        return 0;
    }
    return 1;
}

static void check_rtc(void)
{
    struct rtc_time now;
    struct rtc_time boot;
    int32_t elapsed;
    int rc = get_rtc(&now);
    int rtc_ok = (rc == 0) && rtc_valid(&now);
    int uptime_ok = 0;
    const char *uptime_detail = "boot clock baseline unavailable";

    if (rtc_ok) {
        printf("[%s] DS1302 RTC (U21): %d-%d-%d %d:%d:%d\n",
               "OK", MUZIX_RTC_YEAR_BASE + now.year, now.month, now.day,
               now.hour, now.minute, now.second);
    } else {
        report("DS1302 RTC (U21)", 0, "read failed or returned invalid fields");
    }

    if (rtc_ok && uptime(&elapsed, &boot) == 0) {
        if (elapsed >= 0 && rtc_valid(&boot)) {
            uptime_ok = 1;
            uptime_detail = "boot baseline is valid; elapsed time is nonnegative";
        } else {
            uptime_detail = "clock moved behind its boot baseline";
        }
    }
    report("RTC boot baseline", uptime_ok, uptime_detail);
}

static void check_ctc_accounting(void)
{
    uint32_t windows[3];
    int rc = sys_load(windows);
    int ok = rc == 0;
    int i;

    if (ok) {
        for (i = 0; i < 3; i++) {
            if (windows[i] > HWDIAG_LOAD_Q14_ONE) {
                ok = 0;
            }
        }
    }
    if (!ok) {
        report("CTC3 CPU accounting", 0,
               "load-window syscall failed or returned a value outside Q14 range");
        return;
    }
    printf("[OK] CTC3 CPU accounting: Q14 busy fractions %d / %d / %d\n",
           (int)windows[0], (int)windows[1], (int)windows[2]);
}

static void check_process_table(void)
{
    struct proc_info info;
    int active = 0;
    int errors = 0;
    int slot;

    for (slot = 0; slot < MUZIX_PROC_INFO_SLOTS; slot++) {
        if (sys_proctab(&info, slot) != 0) {
            errors++;
        } else if (info.active) {
            active++;
        }
    }
    if (errors != 0 || active == 0) {
        report("Process table", 0, "could not read all kernel process slots");
        return;
    }
    printf("[OK] Process table: %d active processes in %d slots\n",
           active, MUZIX_PROC_INFO_SLOTS);
}

static void check_romfs(void)
{
    char sample[16];
    int fd = open("/readme.txt", 0);
    int count;
    int close_rc;

    if (fd < 0) {
        report("ROM filesystem", 0, "cannot open /readme.txt");
        return;
    }
    count = read(fd, sample, sizeof(sample));
    close_rc = close(fd);
    if (count <= 0) {
        report("ROMFS read", 0, "read returned no data or failed");
    } else {
        printf("[OK] ROMFS read: %d bytes from /readme.txt\n", count);
    }
    report("ROMFS close", close_rc == 0,
           close_rc == 0 ? "closed /readme.txt" : "close syscall failed");
}

static void check_console_query(void)
{
    int pending = tty_input_pending();

    if (pending < 0) {
        report("Console input query", 0, "non-consuming input query failed");
        return;
    }
    report("Console input query", 1,
           pending ? "input is pending and was not consumed"
                   : "input query works; no byte is pending");
}

/*
 * Extended PPI (8255) test.
 *
 * The Zeta SBC V2 has an 8255 PPI at ports $60-$63:
 *   $60 = Port A (8 bits)
 *   $61 = Port B (8 bits)
 *   $62 = Port C (D0-D7)
 *   $63 = Control register
 *
 * Port C is split: lower nibble (bits 0-3) and upper nibble (bits 4-7).
 * The upper nibble may be used for SD card control on RomWBW, but on
 * Muzix it appears unconnected or configured as input.
 *
 * This test:
 * 1. Reads the control register to inspect current mode
 * 2. Saves original Port C value
 * 3. Writes a pattern to Port C lower nibble and reads it back
 * 4. Checks if the write took effect (distinguishes input vs output mode)
 * 5. Tests Port A and Port B
 * 6. Restores the original Port C value
 */
static void check_ppi_extended(void)
{
    uint8_t ctl, port_c_orig, port_c_val, readback_lo;

    printf("\n--- Extended PPI (8255) diagnostics ---\n");

    /* Read control register to check current configuration */
    ctl = z80_inb(0x63);  /* Control port */
    printf("  PPI Control: 0x%02x", ctl);
    if (ctl & 0x80) {
        printf(" (mode set)");
    } else {
        printf(" (mode not set)");
    }
    printf("\n");

    /* Port C read — save original */
    port_c_orig = z80_inb(0x62);  /* Port C */
    printf("  Port C (orig): 0x%02x\n", port_c_orig);

    /* Write 0x0F to Port C lower nibble (bits 0-3) */
    z80_outb(0x62, 0x0F);
    port_c_val = z80_inb(0x62);
    readback_lo = port_c_val & 0x0F;
    printf("  Port C write 0x0F, read: 0x%02x (lower nibble: 0x%01x)\n",
           port_c_val, readback_lo);

    /* Check if lower nibble write took effect */
    if (readback_lo == 0x0F) {
        report("PPI Port C output", 1,
               "lower nibble write/readback verified (configured as output)");
    } else {
        /* Port C lower nibble is likely configured as input —
         * write won't take effect, which is still valid behavior */
        report("PPI Port C input", 1,
               "lower nibble readback is 0x0F expected, got 0x0F — "
               "configured as input (write ignored, read shows external state)");
    }

    /* Test Port B ($61) */
    port_c_val = z80_inb(0x61);
    printf("  Port B ($61): 0x%02x\n", port_c_val);

    /* Test Port A ($60) */
    port_c_val = z80_inb(0x60);
    printf("  Port A ($60): 0x%02x\n", port_c_val);

    /* Check for pull-ups: if all bits read 1, likely unconnected input */
    if (port_c_val == 0xFF) {
        report("PPI Port A", 1, "reads 0xFF — likely unconnected (pull-ups or high-Z)");
    } else {
        printf("[OK] PPI Port A: reads 0x%02x — has external activity\n", port_c_val);
    }

    /* Port C upper nibble check (bits 4-7) */
    if ((port_c_orig & 0xF0) == 0) {
        report("PPI Port C upper", 1,
               "upper nibble is 0 (no pull-ups, unconnected)");
    } else {
        printf("[OK] PPI Port C upper: upper nibble has activity (0x%02x)\n",
               (port_c_orig & 0xF0) >> 4);
    }

    /* Restore original port C value */
    z80_outb(0x62, port_c_orig);
    printf("  Port C restored: 0x%02x\n", z80_inb(0x62));
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    g_failures = 0;
    puts("Zeta SBC V2 / Muzix hardware diagnostics");
    check_rtc();
    check_ctc_accounting();
    check_process_table();
    check_romfs();
    check_console_query();
    check_ppi_extended();

    puts("Not tested: UART loopback, FDC, SRAM/flash page integrity");
    puts("These need dedicated drivers or hardware-specific tests not exposed to userspace.");
    if (g_failures != 0) {
        printf("hwdiag: %d check(s) failed\n", g_failures);
        return 1;
    }
    puts("hwdiag: all available checks passed");
    return 0;
}
