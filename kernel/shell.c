#include "../include/muzix_ipc.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../fs/fs_service.h"

#include <stdint.h>
#include <string.h>

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

static int sh_ipc_sendrec(int type, int16_t i1, int16_t i2, int16_t i3,
                          uint16_t p1, uint16_t p2, uint16_t p3,
                          int16_t *result_i1)
{
    muzix_ipc_table_t *ipc = muzix_ipc_get_global();
    muzix_ipc_message_t msg;
    int result;

    if (!ipc) {
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    msg.m_source = MUZIX_SYSTEM_TASK;
    msg.m_type = type;
    msg.m1_i1 = i1;
    msg.m1_i2 = i2;
    msg.m1_i3 = i3;
    msg.m1_p1 = p1;
    msg.m1_p2 = p2;
    msg.m1_p3 = p3;

    result = muzix_ipc_sendrec(ipc, MUZIX_SYSTEM_TASK, 2, &msg);
    if (result != MUZIX_IPC_OK) {
        return -1;
    }

    if (result_i1) {
        *result_i1 = msg.m1_i1;
    }
    return 0;
}

/* Wait for one byte, and give the processor back while waiting.
 *
 * WHY THIS IS NOT A POLL, WHICH IS WHAT IT WAS
 * --------------------------------------------
 * The loop this replaces polled muzix_zeta_uart_available(0) at full speed under
 * a deadline it re-armed the moment it expired, so the deadline bounded nothing:
 * the CPU was held, at 100% utilisation, for as long as it took somebody to
 * press a key.  On a machine with no preemption that is the whole system - and
 * it is exactly the shape of the fault the load windows exist to expose, except
 * that it was invisible here because this shell runs in kernel context where
 * nothing charges the time to anybody.
 *
 * The kernel already has the mechanism for a kernel-context wait that does not
 * hold the processor: the halt loop in kernel/kernel_loop.c's kernel_halt(),
 * which is what the scheduler itself uses.  This is the same thing one loop
 * down, and it uses the same bounded, tick-based shape:
 *
 *   - z80_idle_halt() stops the processor until an interrupt, and a stopped
 *     Z80 issues no bus cycles at all.  Both of the machine's interrupts end
 *     it: the 120 Hz tick re-arms the deadline, and the UART's own vectored
 *     interrupt - the one that was added precisely so a keystroke does not have
 *     to wait out a deadline - returns immediately.
 *   - the deadline is still re-armed rather than acted on, and that is
 *     deliberate: a shell that stops waiting for a key is not a shell.  What
 *     changed is that the machine is stopped while it waits, not that it stops
 *     waiting.
 *
 * SAFE HERE, WHICH IS THE SAME ARGUMENT kernel_halt() MAKES
 * --------------------------------------------------------
 * This runs from ordinary kernel context with the kernel map installed, never
 * from an interrupt handler; it touches no bank register and reads no port
 * except the UART's own status; and the tick handler's constraint - data cells
 * in window 0, pushes wherever it was interrupted - is untouched, because
 * nothing here runs inside it.  The UART handler raises a _DATA flag and this
 * loop notices the byte on its next pass, exactly as platform/zeta-v2/tick.s
 * describes.
 *
 * Kept responsive by construction rather than by polling: the wait ends on the
 * first interrupt that follows a byte arriving, which is the UART's own. */
static void sh_wait_key(void)
{
    uint16_t deadline = MUZIX_TICK_DEADLINE(MUZIX_SHELL_KEY_TICKS);

    while (!muzix_zeta_uart_available(0)) {
        if (muzix_tick_expired(deadline)) {
            /* Re-arm, do not give up: see the comment above. */
            deadline = MUZIX_TICK_DEADLINE(MUZIX_SHELL_KEY_TICKS);
        }
        z80_idle_halt();
    }
}

void shell_task(void)
{
    char buf[48];
    uint8_t i;
    uint8_t start;

    sh_puts("\nMUZIX Shell v1.0\n");
    sh_puts("Type 'help' for commands\n\n");

    for (;;) {
        sh_puts("[FALLBACK] $ ");
        i = 0;
        while (i < sizeof(buf) - 1) {
            sh_wait_key();
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

        if (buf[0] == 'h') {
            sh_puts("Commands: help echo ls trace version\n");
        } else if (buf[0] == 'v') {
            sh_puts("MUZIX kernel v1.0 (Z80/Zeta-V2)\n");
        } else if (buf[0] == 't') {
            muzix_trace_dump();
        } else if (buf[0] == 'l') {
            int16_t handle;
            int16_t result;
            muzix_fs_dirent_t entry;
            uint16_t index = 0;

            if (sh_ipc_sendrec(10, 0, 0, 0, (uint16_t)".", 0, 0, &handle) != 0 || handle < 0) {
                sh_puts("ls: error\n");
                continue;
            }

            while (1) {
                if (sh_ipc_sendrec(40, handle, index, 0, 0, (uint16_t)&entry, 0, &result) != 0 || result != 0) {
                    break;
                }
                if (entry.inode != 0 && entry.name[0] != '\0') {
                    sh_puts("  ");
                    sh_puts(entry.name);
                    sh_puts("\n");
                }
                index++;
            }
            sh_ipc_sendrec(11, handle, 0, 0, 0, 0, 0, &result);
        } else if (buf[0] == 'e' && buf[1] == 'c') {
            /* The argument starts after the command word, not at a fixed index.
             *
             * The old form was `for (i = 5; buf[i] != '\0'; i++)`, and index 5
             * is where the argument sits in "echo hello" - and only there.  A
             * command shorter than five characters ("ec", "echo") started the
             * loop past the NUL, reading uninitialised stack until it happened
             * to find a zero byte, and printed it.  So the start is derived from
             * the buffer itself: skip to the end of the first word, then skip
             * the spaces after it, and stop there.  Every step is bounded by
             * the terminator, so no read can leave the buffer that holds it.
             *
             * The command match above is deliberately crude - first letter, then
             * 'c' - because this is a fallback shell with about a dozen bytes of
             * _CODE to spend.  Matching on "ec" and then taking everything after
             * the first WORD is the same crudeness applied consistently. */
            start = 2;
            while (buf[start] != '\0' && buf[start] != ' ') {
                start++;
            }
            while (buf[start] == ' ') {
                start++;
            }
            for (i = start; buf[i] != '\0'; i++) {
                sh_putc(buf[i]);
            }
            sh_putc('\n');
            sh_putc('\r');
        } else {
            sh_puts("Unknown command. Type 'help'\n");
        }
    }
}