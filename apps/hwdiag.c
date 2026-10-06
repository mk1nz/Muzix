/*
 * hwdiag - safe diagnostics for hardware interfaces exposed by the kernel.
 *
 * The Zeta SBC V2 has a 16550 UART, Z80 CTC, DS1302 RTC, 8255 PPI, FDC,
 * 512 KiB SRAM and 512 KiB flash. This command probes only interfaces that
 * Muzix currently exposes to userspace; it does not poke device registers or
 * change persistent hardware state.
 */

#include "../lib/libc.h"

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
    int fd = open("/readme", 0);
    int count;
    int close_rc;

    if (fd < 0) {
        report("ROM filesystem", 0, "cannot open /readme");
        return;
    }
    count = read(fd, sample, sizeof(sample));
    close_rc = close(fd);
    if (count <= 0) {
        report("ROMFS read", 0, "read returned no data or failed");
    } else {
        printf("[OK] ROMFS read: %d bytes from /readme\n", count);
    }
    report("ROMFS close", close_rc == 0,
           close_rc == 0 ? "closed /readme" : "close syscall failed");
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

    puts("Not tested: UART loopback, 8255 PPI, FDC, SRAM/flash page integrity");
    puts("These need drivers or hardware-specific tests not exposed to userspace.");
    if (g_failures != 0) {
        printf("hwdiag: %d check(s) failed\n", g_failures);
        return 1;
    }
    puts("hwdiag: all available checks passed");
    return 0;
}
