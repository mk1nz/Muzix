/*
 * host-test-not-portable: this test drives the REAL filesystem through the REAL
 * syscall dispatcher - 91 dispatcher calls across 22 system calls, the only
 * coverage of that combination in the tree - and every one of them hands the
 * dispatcher a "user address" that is a C pointer.
 *
 * muzix_copy_user_from_current() resolves the calling slot and then goes through
 * the MM to zeta_copy_user_to_kernel(), which on the target programs the bank
 * registers and copies through a 16 KB window at a REAL 16-bit address.  A host
 * pointer is in no window, and it is not even a 16-bit quantity: the dispatcher
 * takes uint16_t.  test_host/zeta_host.c stands in for the primitive and models a
 * 64 KB user space indexed BY ADDRESS, so the test could only work by placing
 * every path, stat buffer, pipe fd pair and ioctl argument at a chosen offset in
 * that space and copying the results back out afterwards.  That is a rewrite of
 * 60-odd address arguments and of every assertion that reads what came back, not
 * a fix to a broken include.
 *
 * It did not compile at all until the missing kernel/syscalls.h include was added
 * on 2026-10-04, so it had never run.  Two real faults were found on the way to
 * that point and are fixed in the file below: the syscall context was initialised
 * field by field with umask missing, so SYS_UMASK reported a stack value as the
 * previous mask; and the file was listed as expected-to-build while having never
 * been able to build.
 *
 * platform/zeta-v2/test_syscall_entry.c covers the userspace-number-to-kernel
 * seam from the host today, and kernel/test_syscalls.c covers the dispatcher with
 * its own harness.  Neither covers the real filesystem behind the dispatcher, and
 * that remains the gap this file is written for.
 */
#include <string.h>

#include "fs_service.h"
#include "../kernel/syscalls.h"
#include "../kernel/kernel_loop.h"
#include "../kernel/proc_table.h"
#include "../mm/mm_service.h"

static uint8_t disk[MUZIX_FS_BLOCK_SIZE * 32];

static int read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        buffer[i] = disk[block * MUZIX_FS_BLOCK_SIZE + i];
    }
    return 0;
}

static int write_block(struct muzix_block_device *device, uint16_t block, const uint8_t *buffer)
{
    (void)device;
    for (uint16_t i = 0; i < MUZIX_FS_BLOCK_SIZE; i++) {
        disk[block * MUZIX_FS_BLOCK_SIZE + i] = buffer[i];
    }
    return 0;
}

static int test_fs_service_io(void)
{
    muzix_block_device_t device = {0, read_block, write_block, 32};
    muzix_fs_volume_t volume;
    muzix_fs_volume_t remounted;
    muzix_fs_service_t fs;
    muzix_fs_service_t fs_after_mount;
    uint16_t inode;
    uint16_t hello_inode;
    uint16_t etc_inode;
    muzix_fs_inode_t stat;
    muzix_fs_dirent_t dirent;
    uint8_t input[5] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t output[5] = {0, 0, 0, 0, 0};
    uint8_t syscall_output[5] = {0, 0, 0, 0, 0};
    char cwd_output[MUZIX_FS_PATH_MAX];
    muzix_userspace_syscall_context_t syscall_ctx;
    int writer;
    int reader;
    int pipe_fds[2];
    muzix_tty_device_t tty;
    muzix_tty_params_t tty_params;
    size_t done;

    muzix_fs_volume_init(&volume, &device);
    if (muzix_fs_volume_format(&volume, 8, 32, 5) != 0) {
        return 1;
    }
    muzix_fs_service_init(&fs, 0, 4);
    muzix_fs_service_attach_volume(&fs, &volume);
    muzix_tty_device_init(&tty);
    muzix_fs_service_attach_tty(&fs, &tty);
    if (muzix_fs_service_create_root(&fs, "hello", 0x8000u, &inode) != 0 ||
        muzix_fs_service_lookup_root(&fs, "hello", &inode) != 0) {
        return 2;
    }
    hello_inode = inode;
    /* Zeroed, not assigned field by field.  muzix_handle_userspace_syscall()
     * only refreshes ctx->umask from the process table when it can resolve the
     * running slot, and this context has no kernel loop behind it - so whatever
     * was in that field on the stack is what UMASK reports as the PREVIOUS mask.
     * The field was left out of the initialiser, the stack happened to hold 022,
     * and the first UMASK call answered 022 where 0 was expected, which is how
     * this test failed on the one line that had nothing to do with the filesystem.
     *
     * SYS_UMASK returns the old mask, which is what POSIX umask(2) does and what
     * the assertions below rely on: the first call must answer 0 and the second
     * must answer 022. */
    memset(&syscall_ctx, 0, sizeof(syscall_ctx));
    syscall_ctx.fs = &fs;
    syscall_ctx.pid = 7;
    syscall_ctx.ppid = 1;
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_GETPID, 0, 0, 0) != 7 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_GETPPID, 0, 0, 0) != 1) {
        return 10;
    }
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_UMASK, 022, 0, 0);
    if (writer != 0 ||
        (writer = muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CREAT,
            (int32_t)(uintptr_t)"/masked", 0666, 0)) < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_STAT,
            (int32_t)(uintptr_t)"/masked", (int32_t)(uintptr_t)&stat, 0) != 0 ||
        (stat.mode & 0777u) != 0644u ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_UMASK, 0, 0, 0) != 022) {
        return 10;
    }
    reader = muzix_fs_service_open_tty(&fs);
        tty_params.erase = 9;
        tty_params.kill = 10;
        tty_params.flags = 0x55u;
    if (reader < 0 || muzix_fs_service_ioctl(
            &fs, reader, MUZIX_TTY_IOCTL_SETP,
            &tty_params) != 0 ||
        muzix_fs_service_ioctl(&fs, reader, MUZIX_TTY_IOCTL_GETP,
                               &tty_params) != 0 || tty_params.erase != 9 ||
        tty_params.kill != 10 || tty_params.flags != 0x55u ||
        muzix_fs_service_close(&fs, reader) != 0) {
        return 10;
    }
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_MKNOD,
            (int32_t)(uintptr_t)"/tty", MUZIX_FS_MODE_CHAR | 0600,
            MUZIX_FS_TTY_DEVICE) != 0) {
        return 10;
    }
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/tty", MUZIX_FS_OPEN_RDWR, 0);
    if (writer < 0 || muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0) {
        return 10;
    }
    reader = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/tty", MUZIX_FS_OPEN_RDWR, 0);
    if (reader < 0 || muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_IOCTL, reader,
            MUZIX_TTY_IOCTL_GETP, (int32_t)(uintptr_t)&tty_params) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, reader, 0, 0) != 0) {
        return 10;
    }
    reader = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/tty", MUZIX_FS_OPEN_RDWR, 0);
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_DUP, reader, 0, 0);
    if (reader < 0 || writer < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, reader, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_IOCTL, writer,
            MUZIX_TTY_IOCTL_GETP, (int32_t)(uintptr_t)&tty_params) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0) {
        return 10;
    }
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_PIPE,
            (int32_t)(uintptr_t)pipe_fds, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_WRITE, pipe_fds[1],
            (int32_t)(uintptr_t)input, sizeof(input)) != (int32_t)sizeof(input) ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, pipe_fds[0],
            (int32_t)(uintptr_t)output, 2) != 2 ||
        output[0] != 'h' || output[1] != 'e' ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, pipe_fds[0],
            (int32_t)(uintptr_t)output, sizeof(output)) != 3 ||
        output[0] != 'l' || output[1] != 'l' || output[2] != 'o' ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, pipe_fds[0],
            (int32_t)(uintptr_t)output, 1) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, pipe_fds[0],
            (int32_t)(uintptr_t)output, -1) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_WRITE, pipe_fds[1],
            (int32_t)(uintptr_t)input, -1) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_SEEK, pipe_fds[0], 0, 0) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, pipe_fds[1], 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, pipe_fds[0],
            (int32_t)(uintptr_t)output, 1) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, pipe_fds[0], 0, 0) != 0) {
        return 10;
    }
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_PIPE,
            (int32_t)(uintptr_t)pipe_fds, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, pipe_fds[0], 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_WRITE, pipe_fds[1],
            (int32_t)(uintptr_t)input, 1) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, pipe_fds[1], 0, 0) != 0) {
        return 10;
    }
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_CREAT,
        (int32_t)(uintptr_t)"/created", 0x0000u, 0);
    if (writer < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_WRITE, writer,
            (int32_t)(uintptr_t)input, sizeof(input)) != (int32_t)sizeof(input) ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0) {
        return 10;
    }
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/hello", MUZIX_FS_OPEN_RDWR, 0);
    reader = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_DUP, writer, 0, 0);
    if (writer < 0 || reader < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_SEEK, writer, -1, 0) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_SEEK, writer, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, writer,
            (int32_t)(uintptr_t)output, 2) != 2 ||
        output[0] != 'h' || output[1] != 'e' ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, reader,
            (int32_t)(uintptr_t)output, 3) != 3 ||
        output[0] != 'l' || output[1] != 'l' || output[2] != 'o' ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_FSTAT, reader,
            (int32_t)(uintptr_t)&stat, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, reader, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_DUP, -1, 0, 0) >= 0) {
        return 10;
    }
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_CREAT,
        (int32_t)(uintptr_t)"/created", 0x0000u, 0);
    if (writer < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_FSTAT, writer,
            (int32_t)(uintptr_t)&stat, 0) != 0 || stat.size != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CREAT,
            (int32_t)(uintptr_t)"/", 0x0000u, 0) >= 0) {
        return 10;
    }
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CHMOD,
            (int32_t)(uintptr_t)"/created", 0640, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_STAT,
            (int32_t)(uintptr_t)"/created", (int32_t)(uintptr_t)&stat, 0) != 0 ||
        (stat.mode & 0777u) != 0640u || (stat.mode & 0x8000u) == 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_OPEN,
            (int32_t)(uintptr_t)"/created/child", MUZIX_FS_OPEN_RDONLY, 0) >= 0 ||
        muzix_fs_service_unlink_path(&fs, "/created/child") == 0 ||
        muzix_fs_service_rmdir_path(&fs, "/created/child") == 0 ||
        muzix_fs_service_rename_path(&fs, "/created/child",
                                     "/created/other") == 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_ACCESS,
            (int32_t)(uintptr_t)"/created", MUZIX_FS_ACCESS_READ, 0) != 0) {
        return 10;
    }
    syscall_ctx.uid = 1;
    syscall_ctx.gid = 1;
    syscall_ctx.euid = 1;
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_ACCESS,
            (int32_t)(uintptr_t)"/created", MUZIX_FS_ACCESS_READ, 0) == 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_OPEN,
            (int32_t)(uintptr_t)"/created", MUZIX_FS_OPEN_RDONLY, 0) >= 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CHMOD,
            (int32_t)(uintptr_t)"/created", 0600, 0) == 0) {
        return 10;
    }
    syscall_ctx.gid = 0;
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_ACCESS,
            (int32_t)(uintptr_t)"/created", MUZIX_FS_ACCESS_READ, 0) != 0) {
        return 10;
    }
    syscall_ctx.uid = 0;
    syscall_ctx.gid = 0;
    syscall_ctx.euid = 0;
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CHMOD,
            (int32_t)(uintptr_t)"/created", 0600, 0) != 0) {
        return 10;
    }
    reader = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/created", MUZIX_FS_OPEN_RDONLY, 0);
    if (reader < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_WRITE, reader,
            (int32_t)(uintptr_t)input, 1) >= 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, reader, 0, 0) != 0) {
        return 10;
    }
    writer = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/hello", MUZIX_FS_OPEN_RDWR, 0);
    if (writer < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_WRITE, writer,
            (int32_t)(uintptr_t)input, sizeof(input)) != (int32_t)sizeof(input) ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_SEEK, writer, 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READ, writer,
            (int32_t)(uintptr_t)syscall_output, sizeof(syscall_output)) !=
            (int32_t)sizeof(syscall_output) ||
        syscall_output[4] != 'o' ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_FSTAT, writer,
            (int32_t)(uintptr_t)&stat, 0) != 0 ||
        stat.size != sizeof(input) ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_FSTAT, -1,
            (int32_t)(uintptr_t)&stat, 0) == 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, writer, 0, 0) != 0) {
        return 10;
    }
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_MKDIR,
            (int32_t)(uintptr_t)"tmp", 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CHDIR,
            (int32_t)(uintptr_t)"tmp", 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_MKDIR,
            (int32_t)(uintptr_t)"child", 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_RENAME,
            (int32_t)(uintptr_t)"child", (int32_t)(uintptr_t)"child2", 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_RMDIR,
            (int32_t)(uintptr_t)"child2", 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_GETCWD,
            (int32_t)(uintptr_t)cwd_output, sizeof(cwd_output), 0) != 0 ||
        strcmp(cwd_output, "/tmp") != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_GETCWD,
            (int32_t)(uintptr_t)cwd_output, -1, 0) != -1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CHDIR,
            (int32_t)(uintptr_t)"..", 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_GETCWD,
            (int32_t)(uintptr_t)cwd_output, sizeof(cwd_output), 0) != 0 ||
        strcmp(cwd_output, "/") != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CHDIR,
            (int32_t)(uintptr_t)"/", 0, 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_RENAME,
            (int32_t)(uintptr_t)"tmp", (int32_t)(uintptr_t)"tmp2", 0) != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_STAT,
            (int32_t)(uintptr_t)"hello", (int32_t)(uintptr_t)&stat, 0) != 0 ||
        stat.size != sizeof(input) ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_RMDIR,
            (int32_t)(uintptr_t)"tmp2", 0, 0) != 0) {
        return 11;
    }
    if (muzix_fs_service_mkdir_root(&fs, "etc", 0, 0, &etc_inode) != 0 ||
        muzix_fs_service_lookup_root(&fs, "etc", &inode) != 0 ||
        inode != etc_inode || fs.volume->inodes.entries[etc_inode].mode != 0x4000u) {
        return 3;
    }
    writer = muzix_fs_service_creat_path(&fs, "/etc/first", 0, 0);
    reader = muzix_fs_service_creat_path(&fs, "/etc/second", 0, 0);
    if (writer < 0 || reader < 0 ||
        muzix_fs_service_close(&fs, writer) != 0 ||
        muzix_fs_service_close(&fs, reader) != 0 ||
        muzix_fs_service_unlink_path(&fs, "/etc/first") != 0) {
        return 3;
    }
    reader = muzix_handle_userspace_syscall(
        &syscall_ctx, MUZIX_USER_SYS_OPEN,
        (int32_t)(uintptr_t)"/etc", 0, 0);
    if (reader < 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_STAT,
            (int32_t)(uintptr_t)"/etc", (int32_t)(uintptr_t)&stat, 0) != 0 ||
        (stat.mode & 0x4000u) == 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READDIR, reader, 0,
            (int32_t)(uintptr_t)&dirent) != 0 ||
        strcmp(dirent.name, ".") != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READDIR, reader, 1,
            (int32_t)(uintptr_t)&dirent) != 0 ||
        strcmp(dirent.name, "..") != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READDIR, reader, 2,
            (int32_t)(uintptr_t)&dirent) != 0 ||
        strcmp(dirent.name, "second") != 0 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_READDIR, reader, 3,
            (int32_t)(uintptr_t)&dirent) != 1 ||
        muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_CLOSE, reader, 0, 0) != 0) {
        return 12;
    }
    if (muzix_handle_userspace_syscall(
            &syscall_ctx, MUZIX_USER_SYS_UNLINK,
            (int32_t)(uintptr_t)"etc", 0, 0) == 0) {
        return 13;
    }
    reader = muzix_fs_service_open_path(&fs, "/etc");
    if (reader < 0 || muzix_fs_service_rmdir_path(&fs, "/etc") == 0 ||
        muzix_fs_service_close(&fs, reader) != 0 ||
        muzix_fs_service_rmdir_path(&fs, "/etc") != 0) {
        return 14;
    }
    if (muzix_fs_service_mkdir_path(&fs, "/parent", 0, 0, &inode) != 0 ||
        muzix_fs_service_mkdir_path(&fs, "/parent/child", 0, 0, &inode) != 0 ||
        muzix_fs_service_chdir(&fs, "/parent/child") != 0 ||
        muzix_fs_service_rmdir_path(&fs, "/parent") == 0 ||
        muzix_fs_service_chdir(&fs, "/") != 0 ||
        muzix_fs_service_rmdir_path(&fs, "/parent/child") != 0 ||
        muzix_fs_service_rmdir_path(&fs, "/parent") != 0) {
        return 15;
    }
    if (muzix_fs_service_mkdir_path(&fs, "/a", 0, 0, &inode) != 0 ||
        muzix_fs_service_mkdir_path(&fs, "/a/b", 0, 0, &inode) != 0 ||
        muzix_fs_service_rename_path(&fs, "/a", "/a/b/moved") == 0 ||
        muzix_fs_service_stat_path(&fs, "/a", &stat) != 0 ||
        muzix_fs_service_stat_path(&fs, "/a/b", &stat) != 0) {
        return 16;
    }
    if (muzix_fs_service_creat_path(&fs, "/same", 0, 0) < 0 ||
        muzix_fs_service_rename_path(&fs, "/same", "/same") != 0 ||
        muzix_fs_service_stat_path(&fs, "/same", &stat) != 0) {
        return 17;
    }
    if (muzix_fs_service_creat_path(&fs, "/victim", 0, 0) < 0 ||
        muzix_fs_service_lookup_root(&fs, "victim", &inode) != 0) {
        return 17;
    }
    fs.volume->inodes.entries[inode].links = 0;
    if (muzix_fs_service_unlink_path(&fs, "/victim") == 0 ||
        muzix_fs_service_stat_path(&fs, "/victim", &stat) != 0) {
        return 17;
    }
    if (muzix_fs_service_chdir(&fs, "/a/b") != 0 ||
        muzix_fs_service_rename_path(&fs, "/a", "/renamed") != 0 ||
        muzix_fs_service_getcwd(&fs, cwd_output, sizeof(cwd_output)) != 0 ||
        strcmp(cwd_output, "/renamed/b") != 0 ||
        muzix_fs_service_creat_path(&fs, "file", 0, 0) < 0 ||
        muzix_fs_service_chdir(&fs, "/") != 0) {
        return 18;
    }
    if (muzix_fs_service_write_inode(&fs, hello_inode, 0, input, sizeof(input), &done) != 0 ||
        done != sizeof(input) || muzix_fs_service_sync(&fs) != 0 ||
        muzix_fs_service_stat_root(&fs, "hello", &stat) != 0 ||
        stat.size != sizeof(input)) {
        return 5;
    }
    if (muzix_fs_service_read_inode(&fs, hello_inode, 0, output, sizeof(output), &done) != 0 ||
        done != sizeof(output) || output[4] != 'o') {
        return 6;
    }
    muzix_fs_volume_init(&remounted, &device);
    if (muzix_fs_volume_mount(&remounted) != 0) {
        return 5;
    }
    muzix_fs_service_init(&fs_after_mount, 0, 4);
    muzix_fs_service_attach_volume(&fs_after_mount, &remounted);
    reader = muzix_fs_service_open_root(&fs_after_mount, "hello");
    if (reader < 0 || muzix_fs_service_read(&fs_after_mount, reader,
                                             output, sizeof(output), &done) != 0 ||
        done != sizeof(output) || output[0] != 'h' ||
        muzix_fs_service_close(&fs_after_mount, reader) != 0) {
        return 8;
    }
    writer = muzix_fs_service_open_root(&fs, "hello");
    reader = muzix_fs_service_open_root(&fs, "hello");
    if (writer < 0 || reader < 0 ||
        muzix_fs_service_read(&fs, reader, output, sizeof(output), &done) != 0 ||
        done != sizeof(output) || output[0] != 'h' ||
        muzix_fs_service_close(&fs, writer) != 0 ||
        muzix_fs_service_close(&fs, reader) != 0) {
        return 8;
    }
    if (muzix_fs_service_link_path(&fs, "/hello", "/hello_alias") != 0 ||
        muzix_fs_service_stat_path(&fs, "/hello_alias", &stat) != 0 ||
        stat.links != 2 ||
        muzix_fs_service_unlink_path(&fs, "/hello") != 0 ||
        muzix_fs_service_lookup_root(&fs, "hello", &inode) == 0 ||
        muzix_fs_service_stat_path(&fs, "/hello_alias", &stat) != 0 ||
        stat.links != 1) {
        return 8;
    }
    reader = muzix_fs_service_open_root(&fs, "hello");
    if (reader >= 0) {
        return 8;
    }
    reader = muzix_fs_service_open_path(&fs, "/hello_alias");
    if (reader < 0 || muzix_fs_service_unlink_path(&fs, "/hello_alias") != 0 ||
        muzix_fs_service_seek(&fs, reader, 4) != 0 ||
        muzix_fs_service_read(&fs, reader, output, 1, &done) != 0 ||
        done != 1 || output[0] != 'o' ||
        muzix_fs_service_close(&fs, reader) != 0 ||
        muzix_fs_service_creat_path(&fs, "/hello", 0, 0) < 0 ||
        muzix_fs_service_rename_root(&fs, "hello", "renamed") != 0 ||
        muzix_fs_service_lookup_root(&fs, "renamed", &inode) != 0) {
        return 8;
    }
    if (muzix_fs_service_unlink_root(&fs, "renamed") != 0) {
        return 9;
    }
    if (muzix_fs_service_close(&fs, writer) != 0) {
        return 9;
    }
    if (muzix_fs_service_unmount(&fs) != 0 ||
        muzix_fs_service_sync(&fs) == 0) {
        return 8;
    }
    return 0;
}

int main(void)
{
    return test_fs_service_io();
}
