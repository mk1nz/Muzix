#include "../fs/fs_service.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/trace.h"

#include <stdint.h>

/* How often the fallback shell checks its deadline while polling the UART.
 * This is a busy wait in kernel context, not a sleep or scheduler yield; unlike
 * the userspace libc read path, it can keep the kernel loop from dispatching
 * other work while no input is available. */
#define MUZIX_SHELL_KEY_TICKS 2

static void sh_putc(char c)
{
    if (c == '\n') {
        muzix_zeta_uart_send((uint8_t)'\r', 0);
    }
    muzix_zeta_uart_send((uint8_t)c, 0);
}

static void sh_puts(const char *s)
{
    while (*s) {
        sh_putc(*s);
        s++;
    }
}

void shell_task(muzix_fs_service_t *fs)
{
    char buf[48];
    uint8_t i;

    sh_puts("\nMUZIX Shell v1.0\n");
    sh_puts("Type 'help' for commands\n\n");

    for (;;) {
        sh_puts("[FALLBACK] $ ");
        i = 0;
        while (i < sizeof(buf) - 1) {
            /* Keep polling until input arrives. The tick deadline only causes
             * the polling deadline to be refreshed; it does not sleep, yield,
             * or make this kernel-context wait preemptible. */
            uint16_t deadline = MUZIX_TICK_DEADLINE(MUZIX_SHELL_KEY_TICKS);

            while (!muzix_zeta_uart_available(0)) {
                if (muzix_tick_expired(deadline)) {
                    deadline = MUZIX_TICK_DEADLINE(MUZIX_SHELL_KEY_TICKS);
                }
            }
            {
                int c = muzix_zeta_uart_recv(0);
                sh_putc((char)c);
                if (c == '\r' || c == '\n') {
                    break;
                }
                buf[i++] = (char)c;
            }
        }
        buf[i] = '\0';
        sh_putc('\n');
        sh_putc('\r');

        if (buf[0] == '\0') {
            continue;
        }

        /* No reboot command.
         *
         * It used to be an endless paging-disable loop, which turned paging
         * off and spun - it never reached a reset, and the bank selector is
         * write-only so nothing could read the state back to recover.  (The
         * primitive it called, muzix_zeta_enable_paging(), had no callers at all
         * and was removed on 2026-10-02; see platform/zeta-v2/bank_io.h.) Rewriting it as a plain `return` only made it exit to a
         * caller that itself returns, so it still did nothing while still
         * advertising a capability the platform cannot deliver. There is no
         * reset path in the Zeta V2 support here; do not reintroduce one as a
         * spin on the paging port. */
        if (buf[0] == 'h') {
            sh_puts("Commands: help echo ls trace version\n");
        } else if (buf[0] == 'v') {
            sh_puts("MUZIX kernel v1.0 (Z80/Zeta-V2)\n");
        } else if (buf[0] == 't') {
            muzix_trace_dump();
        } else if (buf[0] == 'l') {
            muzix_fs_dirent_t entry;
            uint16_t index = 0;
            int handle;
            handle = muzix_fs_service_open_root(fs, ".");
            if (handle < 0) {
                sh_puts("ls: error\n");
                continue;
            }
            while (muzix_fs_service_readdir(fs, handle, index, &entry) == 0) {
                if (entry.inode != 0 && entry.name[0] != '\0') {
                    sh_puts("  ");
                    sh_puts(entry.name);
                    sh_puts("\n");
                }
                index++;
            }
            muzix_fs_service_close(fs, handle);
        } else if (buf[0] == 'e' && buf[1] == 'c') {
            for (i = 5; buf[i] != '\0'; i++) {
                sh_putc(buf[i]);
            }
            sh_putc('\n');
            sh_putc('\r');
        } else {
            sh_puts("Unknown command. Type 'help'\n");
        }
    }
}
