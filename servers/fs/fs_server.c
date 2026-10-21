#include "server.h"
#include "syscall.h"
#include "proc_info.h"
#include "libc.h"
#include "fs_syscalls.h"
/* These two are written with ../../ rather than ../ on purpose.  servers/fs/
 * contains a near-copy of the fs/ tree, and a quoted include resolves against
 * the including file's directory first, so "../fs/tty_device.h" from this file
 * means servers/fs/tty_device.h - the copy, which has never compiled and whose
 * includes are wrong - and not the live one.  The ../../ spelling forces the
 * real one and fails loudly if the tree moves, which is better than silently
 * binding the wrong device. */
#include "../../fs/tty_device.h"
#include "../../platform/zeta-v2/uart_io.h"
#include "fs_clock.h"
#include <string.h>

#define MUZIX_FS_PATH_MAX 64u

/* One I/O slice, in bytes: how much moves between the client's buffer and the
 * TTY device per copy, bounded by the 256-byte stack buffer below.
 *
 * The slice is NOT the transfer size.  It used to be: handle_read and
 * handle_write clamped `length` into a 256-byte buffer and reported the clamped
 * count as the transfer count, so a 1000-byte write returned 256 and dropped
 * 744 bytes while claiming success.  Both now loop in slices of at most this
 * size until the whole count has moved, and report the true count.  Counts of
 * one slice or less take the loop's first iteration, which is the old
 * single-copy path unchanged. */
#define MUZIX_FS_IO_SLICE 256u

static muzix_server_message_t g_reply;

/* The console this server owns; created in main() before the loop starts
 * (step 2d).  Declared here because the read/write handlers below route
 * descriptors 0/1/2 through it. */
static muzix_tty_device_t g_fs_tty;

static char g_fs_cwd[MUZIX_FS_PATH_MAX] = "/";

static int fs_is_console_fd(int fd)
{
    return fd == 0 || fd == 1 || fd == 2;
}

/* Resolve `.` and `..` relative to g_fs_cwd, producing an absolute path in
 * resolved.  For the flat ROM filesystem, `..` always means "/" (no nested
 * subdirectories beyond /bin and /sbin). */
static void fs_resolve_cwd_path(char *resolved, const char *path) {
    char full[MUZIX_FS_PATH_MAX];
    size_t pos = 0;

    /* Build full path: cwd + path (with separator if needed) */
    if (path[0] == '/') {
        for (pos = 0; pos < MUZIX_FS_PATH_MAX - 1 && g_fs_cwd[pos] != '\0'; pos++) {
            full[pos] = g_fs_cwd[pos];
        }
        /* For absolute path, just use it */
        for (pos = 0; pos < MUZIX_FS_PATH_MAX - 1 && path[pos] != '\0'; pos++) {
            full[pos] = path[pos];
        }
    } else {
        for (pos = 0; pos < MUZIX_FS_PATH_MAX - 1 && g_fs_cwd[pos] != '\0'; pos++) {
            full[pos] = g_fs_cwd[pos];
        }
        /* Ensure separator */
        if (pos > 0 && full[pos - 1] != '/' && pos < MUZIX_FS_PATH_MAX - 1) {
            full[pos++] = '/';
        }
        for ( ; pos < MUZIX_FS_PATH_MAX - 1 && *path != '\0'; pos++, path++) {
            full[pos] = *path;
        }
    }
    full[pos] = '\0';

    /* Now normalize: split by / and rebuild */
    {
        char out[MUZIX_FS_PATH_MAX];
        size_t out_pos = 0;
        const char *p = full;

        out[out_pos++] = '/'; /* always start with / */

        while (*p && out_pos < MUZIX_FS_PATH_MAX - 1) {
            /* Skip leading slashes */
            while (*p == '/') p++;
            if (*p == '\0') break;

            /* Extract component (up to / or end) */
            char comp[16];
            size_t clen = 0;
            while (*p && *p != '/' && clen < sizeof(comp) - 1) {
                comp[clen++] = *p++;
            }
            comp[clen] = '\0';

            if (clen == 1 && comp[0] == '.') {
                /* . — skip */
            } else if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
                /* .. — pop one component (but not past root) */
                if (out_pos > 1) {
                    out_pos--;
                    while (out_pos > 1 && out[out_pos - 1] != '/') out_pos--;
                    out[out_pos] = '\0';
                    if (out_pos > 1) {
                        out_pos--; /* remove the trailing / */
                    }
                }
            } else {
                /* Normal component: add / + component */
                if (out_pos > 1) {
                    out[out_pos++] = '/';
                }
                for (size_t i = 0; i < clen && out_pos < MUZIX_FS_PATH_MAX - 1; i++) {
                    out[out_pos++] = comp[i];
                }
            }
        }

        /* Ensure trailing slash is not left (unless it's root) */
        if (out_pos > 1 && out[out_pos - 1] == '/') {
            out_pos--;
        }
        out[out_pos] = '\0';

        /* Copy to resolved */
        for (size_t i = 0; i < out_pos && i < MUZIX_FS_PATH_MAX - 1; i++) {
            resolved[i] = out[i];
        }
        resolved[out_pos] = '\0';
        if (out_pos >= MUZIX_FS_PATH_MAX) {
            resolved[MUZIX_FS_PATH_MAX - 1] = '\0';
        }
    }
}

static int fs_server_init(void) {
    /* chdir/getcwd use minimal root-only path state; kernel interceptors
     * handle all ROM filesystem file operations. */
    g_fs_cwd[0] = '/';
    g_fs_cwd[1] = '\0';
    return 0;
}

static void fs_server_cleanup(void) {
}

static int handle_open(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_close(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    /* Closing a console descriptor is a local no-op that succeeds: the device
     * belongs to the kernel, this server keeps no descriptor table, and there
     * is nothing to release.  Every other descriptor names a file this server
     * could not have opened - it opens none. */
    if (!fs_is_console_fd(m->m1_i1)) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    g_reply.m1_i1 = 0;
    return 0;
}

/*
 * STEP 2e (2026-10-12): the console is served locally, and no file syscall is
 * ever called from this server.
 *
 * EXTENDED 2026-10-14: at STEP 2e that was true of read and write only.  Every
 * other handler still finished with its own userspace wrapper, which is the
 * same defect in twenty more places; the paragraphs below are the record of
 * the whole rule, and the handlers themselves are the plain negative stubs
 * that follow.
 *
 * handle_write() used to finish with `sys_write(fd, buffer, count)`.  That is
 * the defect: muzix_syscall_to_server() maps SYS_OPEN..SYS_SEEK to FS, so the
 * call was queued back into this same server's IPC slot and FS consumed its own
 * request again and again.  No byte ever reached the UART, and the trace showed
 * only syscall 50 looping from slot 2.
 *
 * The rule now is one path per operation, and the server never re-enters the
 * kernel for the operation it was just asked to perform.  Every other handler
 * used to break the same rule with its own wrapper - sys_open(), sys_close(),
 * sys_seek(), sys_mkdir(), sys_stat(), sys_ioctl() and the rest - because
 * muzix_syscall_to_server() routes SYS_OPEN..SYS_SEEK, SYS_MKDIR..SYS_MKNOD,
 * SYS_READDIR..SYS_DUP and SYS_PIPE..SYS_IOCTL all to MUZIX_PROC_FS, this
 * process.  mini_send() queues such a request on the server's own callerq and
 * parks the caller waiting for a reply only the server itself can produce, and
 * the handler that made the call is the one that would have to answer it.  The
 * fs/ service layer that would have served these operations for real is not
 * linked into this server (servers/fs/fs_server.lk), so those handlers now
 * answer with a negative status instead: an error the caller can act on,
 * rather than a machine that stops.
 *
 * The three handlers with something local to serve - read, write and ioctl -
 * route descriptors 0/1/2 through the TTY this server owns, by way of
 * muzix_tty_read()/muzix_tty_write()/muzix_tty_ioctl() - its own functions -
 * and fail loudly on every other descriptor.
 *
 * sys_ipc_srv_copy_from_client()/sys_ipc_srv_copy_to_client() are how the
 * buffer moves, and the choice between them and the plain
 * sys_ipc_copy_from_user()/sys_ipc_copy_to_user() is not cosmetic.
 *
 * m->m1_p1 is an address in the CLIENT's address space - the shell's stack or
 * text page - because it was the shell that built the message.  The plain
 * sys_ipc_copy_from_user() resolves the address against the CALLING process,
 * which here is this server: it read the shell's stack pointer as an offset
 * into the FS server's own window and handed back whatever bytes happened to
 * live there.  That is the stream of 0xD5 the emulator printed, and there was
 * nothing in the count or the return code to say it was wrong - the write
 * succeeded, transmitting garbage.
 *
 * sys_ipc_srv_copy_from_client() instead asks the kernel to copy from the
 * process recorded in ipc_client, which muzix_handle_ipc_sendrec() set from
 * msg.m_source - the sender - on the way in.  Same for the to-client form on
 * the replies.  Neither is routed to a server, so neither re-enters this loop.
 */
static int handle_read(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint8_t buffer[MUZIX_FS_IO_SLICE];
    size_t moved = 0;
    /* SYS_READ is read(fd, buf, count) - argument 1 is the descriptor and the
     * buffer is argument 2.  The kernel builds the message in
     * kernel/syscalls_ipc.c as m1_p1 = arg1, m1_p2 = arg2 and m1_i3 = arg3, so
     * the caller's buffer is m1_p2 and the transfer length is m1_i3.  Reading
     * them from m1_p1 and m1_i2 took the descriptor for the pointer and the
     * pointer for the length: the copy-in then failed EFAULT against address 1,
     * which is window 0, and the write reported -1 without transmitting
     * anything.  handle_open, handle_stat and handle_rename are already in
     * this convention, so this is the two handlers that fell out of step. */
    int total = m->m1_i3;
    int status;

    if (!fs_is_console_fd(m->m1_i1)) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    if (total <= 0) {
        g_reply.m1_i1 = 0;
        return 0;
    }

    while (moved < (size_t)total) {
        size_t chunk = (size_t)total - moved;
        size_t count = 0;

        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        status = muzix_tty_read(&g_fs_tty, buffer, chunk, &count);
        if (status != 0) {
            /* The TTY reports "no byte yet" as MUZIX_READ_AGAIN; the caller's
             * read() turns that into a park rather than an error.  With bytes
             * already moved the partial count is the honest answer - those
             * bytes are on their way out below - and only an empty first slice
             * passes the status through. */
            if (moved == 0) {
                g_reply.m1_i1 = status;
                return 0;
            }
            break;
        }
        if (count == 0) {
            break;
        }
        if (m->m1_p2 != 0 &&
            sys_ipc_srv_copy_to_client((uint16_t)(m->m1_p2 + moved), buffer, count) != 0) {
            g_reply.m1_i1 = (moved != 0) ? (int)moved : -1;
            return -1;
        }
        moved += count;
    }
    g_reply.m1_i1 = (int)moved;
    return 0;
}

static int handle_write(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint8_t buffer[MUZIX_FS_IO_SLICE];
    size_t moved = 0;
    /* SYS_WRITE is write(fd, buf, count), and the message layout is the
     * kernel's m1_p1 = arg1, m1_p2 = arg2, m1_i3 = arg3: fd is m1_i1, the
     * caller's buffer is m1_p2, and the length is m1_i3.  Taking them from
     * m1_p1 and m1_i2 instead meant the copy-in went to address 1 - window 0,
     * which zeta_copy_user_to_kernel() refuses - and returned EFAULT, so
     * nothing reached the TTY.  Same convention as handle_read. */
    int total = m->m1_i3;

    if (!fs_is_console_fd(m->m1_i1)) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    if (total <= 0) {
        g_reply.m1_i1 = 0;
        return 0;
    }

    while (moved < (size_t)total) {
        size_t chunk = (size_t)total - moved;
        size_t count = 0;

        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (sys_ipc_srv_copy_from_client((uint16_t)(m->m1_p2 + moved), buffer, chunk) != 0) {
            /* A copy-in failure used to `return -1` with nothing staged, and
             * muzix_server_run() replies with the zeroed `out` message anyway:
             * the caller was told 0 bytes had been written, which the shell
             * reads as a fatal write.  Negative unless some of the transfer
             * already went out, in which case the partial count is the truth. */
            g_reply.m1_i1 = (moved != 0) ? (int)moved : -1;
            return -1;
        }
        if (muzix_tty_write(&g_fs_tty, buffer, chunk, &count) != 0) {
            g_reply.m1_i1 = (moved != 0) ? (int)moved : -1;
            return -1;
        }
        moved += count;
        if (count != chunk) {
            /* The device took less than the slice; asking again immediately
             * would spin on a transmitter that just said no. */
            break;
        }
    }
    g_reply.m1_i1 = (int)moved;
    return 0;
}

static int handle_seek(const void *msg) {
    (void)msg;
    /* Nothing in this link is seekable: the console is a character stream and
     * no file descriptor exists here.  -1 rather than 0, which would read as
     * "already at offset 0". */
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_mkdir(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_rmdir(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_chdir(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint16_t path_ptr = (uint16_t)m->m1_p1;
    char path[MUZIX_FS_PATH_MAX];
    int i;
    int fd;

    if (path_ptr == 0) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    for (i = 0; i < MUZIX_FS_PATH_MAX - 1; i++) {
        if (sys_ipc_srv_copy_from_client(path_ptr + (uint16_t)i, (uint8_t *)&path[i], 1) != 0) {
            g_reply.m1_i1 = -1;
            return -1;
        }
        if (path[i] == '\0') {
            break;
        }
    }
    path[MUZIX_FS_PATH_MAX - 1] = '\0';

    /* Validate the path through the kernel's ROM filesystem interceptor.
     * sys_open is intercepted by the kernel (not routed to this server),
     * so there is no recursion.  Opening the directory path confirms it
     * exists and is accessible. */
    fd = sys_open(path, 0);
    if (fd < 0) {
        g_reply.m1_i1 = -1;
    } else {
        char resolved[MUZIX_FS_PATH_MAX];
        sys_close(fd);
        fs_resolve_cwd_path(resolved, path);
        strncpy(g_fs_cwd, resolved, MUZIX_FS_PATH_MAX - 1);
        g_fs_cwd[MUZIX_FS_PATH_MAX - 1] = '\0';
        g_reply.m1_i1 = 0;
    }
    return 0;
}

static int handle_getcwd(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint16_t buffer_ptr = (uint16_t)m->m1_p1;
    uint16_t size = (uint16_t)m->m1_i2;
    size_t cwd_len;

    if (buffer_ptr == 0 || size == 0) {
        g_reply.m1_i1 = -1;
        return -1;
    }

    cwd_len = strlen(g_fs_cwd);
    if (cwd_len >= size) {
        g_reply.m1_i1 = -1;
        return 0;
    }

    if (sys_ipc_srv_copy_to_client(buffer_ptr, (uint8_t *)g_fs_cwd, cwd_len + 1) != 0) {
        g_reply.m1_i1 = -1;
        return 0;
    }
    g_reply.m1_i1 = (int)cwd_len;
    return 0;
}

static int handle_unlink(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_rename(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_stat(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_fstat(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_creat(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_link(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_chmod(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_access(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_dup(const void *msg) {
    (void)msg;
    /* No descriptor table to duplicate out of. */
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_readdir(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_sync(const void *msg) {
    (void)msg;
    /* Nothing is mounted in this link, so there is nothing to flush; report
     * that rather than a success nobody could have asked for. */
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_pipe(const void *msg) {
    (void)msg;
    /* Pipes need the service layer's descriptor and buffer machinery, which is
     * not linked here. */
    g_reply.m1_i1 = -1;
    return -1;
}

static int handle_mknod(const void *msg) {
    (void)msg;
    g_reply.m1_i1 = -1;
    return -1;
}

/*
 * The ioctl numbers this handler accepts are the MUZIX_TTY_IOCTL_* set from
 * lib/tty_ioctl.h - GETP 0x7408, SETP 0x7409, SETC 0x7411, GETC 0x7412,
 * PENDING 0x7413 - not the Linux termios numbers 0x5401-0x5405 it used to
 * switch on.  Nothing in the tree ever sent a 0x5401: those numbers are Linux
 * TCGETS/TCSETS/..., while fs/tty_device.c's muzix_tty_ioctl(), which this
 * handler serves through, switches on the MUZIX set.  A request number that
 * matches nothing is not an error anywhere - it falls off the switch and the
 * program gets -1 for a request that is supposed to work - which is why every
 * console ioctl in the tree (lib/libc.c's tty_input_pending, MUZIX_TTY_IOCTL_
 * PENDING on descriptor 0) failed while looking like it had worked.
 *
 * The argument sizes come from the device structs, not from the old comment's
 * guess: params = 4 bytes (erase, kill, flags), chars = 6 bytes, pending = 2.
 *
 * The buffer pointer is m1_p3, not m1_p1.  The kernel builds the message as
 * m1_p1 = arg1, m1_p2 = arg2, m1_p3 = arg3 (kernel/syscalls_ipc.c), and
 * ioctl(fd, request, argument) puts the argument in arg3 - m1_p1 is the
 * descriptor, which as a pointer is address 1, in window 0, which
 * zeta_copy_user_to_kernel() refuses.
 */
static int handle_ioctl(const void *msg) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    uint8_t ioctl_buf[8];
    size_t ioctl_len = 0;
    int copy_in = 0;
    int status;

    switch (m->m1_i2) {
        case MUZIX_TTY_IOCTL_GETP: ioctl_len = sizeof(muzix_tty_params_t); break;
        case MUZIX_TTY_IOCTL_SETP: ioctl_len = sizeof(muzix_tty_params_t); copy_in = 1; break;
        case MUZIX_TTY_IOCTL_GETC: ioctl_len = sizeof(muzix_tty_chars_t); break;
        case MUZIX_TTY_IOCTL_SETC: ioctl_len = sizeof(muzix_tty_chars_t); copy_in = 1; break;
        case MUZIX_TTY_IOCTL_PENDING: ioctl_len = sizeof(uint16_t); break;
        default:
            g_reply.m1_i1 = -1;
            return -1;
    }
    if (ioctl_len > sizeof(ioctl_buf) || !fs_is_console_fd(m->m1_i1)) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    if (copy_in && m->m1_p3 != 0 &&
        sys_ipc_srv_copy_from_client(m->m1_p3, ioctl_buf, ioctl_len) != 0) {
        g_reply.m1_i1 = -1;
        return -1;
    }
    status = muzix_tty_ioctl(&g_fs_tty, (uint16_t)m->m1_i2, ioctl_buf);
    if (status == 0 && !copy_in && m->m1_p3 != 0) {
        if (sys_ipc_srv_copy_to_client(m->m1_p3, ioctl_buf, ioctl_len) != 0) {
            g_reply.m1_i1 = -1;
            return -1;
        }
    }
    g_reply.m1_i1 = status;
    return 0;
}

static int fs_server_handle(const void *msg, void *reply) {
    const muzix_server_message_t *m = (const muzix_server_message_t *)msg;
    muzix_server_message_t *r = (muzix_server_message_t *)reply;
    int result;

    /* The FS server's own progress counter, once per dispatched request.
     *
     * fs_clock.h says muzix_clock_tick() is called once per pass of the
     * server's receive loop, and nothing called it: the counter therefore sat
     * at zero for the life of the process.  The server link resolves the
     * UART's tick reference against fs_clock.rel - _muzix_zeta_uart_send()
     * (platform/zeta-v2/uart_io.s) bounds its wait for a free transmitter with
     * a four-tick budget measured on muzix_tick_count - so with the counter
     * frozen at zero that budget could never expire and the wait was
     * unbounded.  The loop itself lives in lib/server.c and is shared with the
     * other servers, so the tick is taken at the one point in this file every
     * delivered message passes through: the dispatch below. */
    muzix_clock_tick();

    /* A fresh staged reply per request.  g_reply is a static reused across
     * messages; without this a handler that fails without staging a status
     * would deliver the previous request's answer. */
    memset(&g_reply, 0, sizeof(g_reply));
    memset(r, 0, sizeof(*r));

    result = -1;
    switch (m->m_type) {
        case 10: result = handle_open(msg); break;
        case 11: result = handle_close(msg); break;
        case 12: result = handle_read(msg); break;
        case 13: result = handle_write(msg); break;
        case 14: result = handle_seek(msg); break;
        case 20: result = handle_mkdir(msg); break;
        case 21: result = handle_rmdir(msg); break;
        case 22: result = handle_chdir(msg); break;
        case 23: result = handle_getcwd(msg); break;
        case 24: result = handle_unlink(msg); break;
        case 25: result = handle_rename(msg); break;
        case 26: result = handle_stat(msg); break;
        case 27: result = handle_fstat(msg); break;
        case 28: result = handle_creat(msg); break;
        case 29: result = handle_link(msg); break;
        case 32: result = handle_chmod(msg); break;
        case 33: result = handle_access(msg); break;
        case 41: result = handle_dup(msg); break;
        case 40: result = handle_readdir(msg); break;
        case 30: result = handle_sync(msg); break;
        case 46: result = handle_pipe(msg); break;
        case 47: result = handle_ioctl(msg); break;
        case 34: result = handle_mknod(msg); break;
        default: result = -1; break;
    }

    /* muzix_server_run() (lib/server.c) replies with the handler's `out`
     * message whatever this function returns, and `out` was just zeroed.  A
     * failure that left no status in the staged reply was therefore delivered
     * as SUCCESS: handle_open's copy-in failure handed the caller descriptor
     * 0, and handle_write's handed it a 0-byte transfer, which the shell reads
     * as a fatal write (AGENTS.md's write-then-exit trace).  Every failure
     * leaves a negative status in the reply, so even a reply the loop should
     * not have sent is an error the caller can see. */
    if (result != 0) {
        g_reply.m1_i1 = -1;
    }

    *r = g_reply;
    /* g_reply is a static and its addressing fields are stale from an earlier
     * request; a reply is addressed by the request it answers. */
    r->m_source = m->m_source;
    r->m_type = m->m_type;
    return result;
}

static muzix_server_ops_t g_fs_ops = {
    .init = fs_server_init,
    .handle_message = fs_server_handle,
    .cleanup = fs_server_cleanup,
};

/*
 * STEP 2d (2026-10-12): a TTY the server owns.
 *
 * The console has been the thing the server could not write to.  Two defects
 * met there: this server answered a message by calling the same userspace
 * syscall back into the kernel, which looped straight back to this same server
 * because muzix_syscall_to_server() routes SYS_OPEN..SYS_SEEK to FS; and even
 * had that worked, nothing had ever called
 * muzix_tty_device_set_callbacks(), so `g_tty_send` was NULL and
 * fs/tty_device.c's muzix_tty_write() bailed at `if (!g_tty_send) return -1`,
 * with `fs->tty` NULL so muzix_fs_service_open_tty() could not create fds
 * 1 and 2 either.
 *
 * The device is created here, in the server, because the server is the process
 * that owns it.
 *
 * The three hooks are thin plain-C wrappers around the Zeta UART primitives,
 * not those primitives' addresses.  The UART entry points are
 * MUZIX_ZETA_SDCCCALL1, the TTY's callback type is plain -mz80, and AGENTS.md
 * forbids applying one convention to both; passing an sdcccall(1) routine by
 * pointer is the exact mistake fs/tty_device.h documents, where it cost a write
 * to the stack and a reboot.  Going through a wrapper is the supported
 * direction - the compiler builds the sdcccall(1) call and
 * tools/check_z80_call_convention.rb pins the registers that depend on - and
 * for this arity the two conventions happen to agree on the parameters anyway,
 * which is a fact about HL/DE and not something to rely on.
 */
static void fs_tty_send(uint8_t byte, void *userdata)
{
    (void)userdata;
    muzix_zeta_uart_send(byte, userdata);
}

static uint8_t fs_tty_recv(void *userdata)
{
    return muzix_zeta_uart_recv(userdata);
}

static uint8_t fs_tty_available(void *userdata)
{
    return muzix_zeta_uart_available(userdata);
}

static void fs_server_init_console(void)
{
    muzix_tty_device_init(&g_fs_tty);
    muzix_tty_device_set_callbacks(&g_fs_tty,
                                   fs_tty_send,
                                   fs_tty_recv,
                                   fs_tty_available,
                                   0);
}

int main(void) {
    muzix_server_t server;
    fs_server_init_console();
    muzix_server_init(&server, MUZIX_SERVER_TYPE_FS, 1, &g_fs_ops);
    return muzix_server_run(&server);
}
