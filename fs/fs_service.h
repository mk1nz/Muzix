#ifndef MUZIX_FS_SERVICE_H
#define MUZIX_FS_SERVICE_H

#include "../kernel/system_task.h"
#include "block_device.h"
#include "block_cache.h"
#include "inode_table.h"
#include "file_io.h"
#include "path.h"
#include "volume.h"
#include "tty_device.h"

/* Descriptor table (file numbers) and description table (open file objects) are
 * separate capacities.  Sharing one count of 4 made pipe() impossible: the
 * kernel holds three TTY handles (stdin/stdout/stderr) open for the whole
 * session, so only one slot was left in each table and pipe() needs two.  A
 * process that had opened one file could not create a pipe at all.
 *
 * Descriptions are the larger of the two because each one costs a file
 * position (uint32_t) while a file number is a single byte. */
#define MUZIX_FS_OPEN_FILES 8u
#define MUZIX_FS_OPEN_DESCRIPTIONS 8u
#define MUZIX_FS_PATH_MAX 64u
#define MUZIX_FS_ACCESS_EXEC 1u
#define MUZIX_FS_ACCESS_WRITE 2u
#define MUZIX_FS_ACCESS_READ 4u
#define MUZIX_FS_OPEN_RDONLY 0u
#define MUZIX_FS_OPEN_WRONLY 1u
#define MUZIX_FS_OPEN_RDWR 2u
#define MUZIX_FS_PIPE_SLOTS 1u

typedef struct {
    uint8_t data[MUZIX_FS_BLOCK_SIZE];
    uint16_t read_pos;
    uint16_t write_pos;
    uint16_t count;
    uint8_t readers;
    uint8_t writers;
    uint8_t used;
} muzix_fs_pipe_t;

typedef struct {
    uint8_t used;
    uint8_t description;
} muzix_fs_open_file_t;

typedef struct {
    uint16_t inode;
    uint8_t kind;
    uint8_t pipe;
    uint32_t position;
    uint8_t access;
    uint8_t refs;
    uint8_t used;
} muzix_fs_open_description_t;

typedef struct {
    uint16_t inode;
    char name[MUZIX_FS_NAME_MAX + 1];
} muzix_fs_dirent_t;

typedef struct muzix_fs_service {
    muzix_system_task_t *system_task;
    muzix_block_device_t *device;
    muzix_fs_block_cache_t *cache;
    muzix_fs_inode_table_t *inodes;
    muzix_fs_volume_t *volume;
    muzix_fs_open_file_t open_files[MUZIX_FS_OPEN_FILES];
    muzix_fs_open_description_t open_descriptions[MUZIX_FS_OPEN_DESCRIPTIONS];
    muzix_fs_pipe_t pipes[MUZIX_FS_PIPE_SLOTS];
    muzix_tty_device_t *tty;
    uint16_t cwd_inode;
    char cwd_path[MUZIX_FS_PATH_MAX];
    /* Shared directory image for the path operations (mkdir, mknod, creat,
     * unlink, link, rmdir, rename).  One muzix_fs_directory_t is 256 bytes
     * and the kernel stack is only 1317 bytes (SP = 0xFFFE, _DATA ends at
     * 0xFAD9), so the five image buffers rename_path used to hold needed a
     * 1988-byte frame and drove SP 673 bytes past the end of _DATA into
     * _INITIALIZED/_INITIALIZER.  A rename across two non-root directories
     * needs two images live at once, so the operations are staged through
     * this one buffer instead and the second directory is loaded, stored and
     * released before the first is reloaded.
     *
     * scratch_busy makes the sharing safe: every user takes it for the whole
     * operation and fails rather than trampling a live image.  No user calls
     * another user while holding it (muzix_fs_service_lookup_component keeps
     * its own local image precisely so a path lookup may run while the buffer
     * is held), so the guard only ever fires on a future regression. */
    muzix_fs_directory_t scratch;
    uint8_t scratch_busy;
    uint8_t source;
} muzix_fs_service_t;

void muzix_fs_service_init(muzix_fs_service_t *fs,
                           muzix_system_task_t *system_task,
                           uint8_t source);
void muzix_fs_service_set_device(muzix_fs_service_t *fs,
                                                                 muzix_block_device_t *device);
void muzix_fs_service_attach_storage(muzix_fs_service_t *fs,
                                     muzix_fs_block_cache_t *cache,
                                     muzix_fs_inode_table_t *inodes);
void muzix_fs_service_attach_volume(muzix_fs_service_t *fs,
                                    muzix_fs_volume_t *volume);
int muzix_fs_service_create_root(muzix_fs_service_t *fs,
                                 const char *name,
                                 uint16_t mode,
                                 uint16_t *inode);
int muzix_fs_service_mkdir_root(muzix_fs_service_t *fs,
                                const char *name,
                                uint16_t mode,
                                uint16_t umask,
                                uint16_t *inode);
int muzix_fs_service_mkdir_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t umask,
                                uint16_t *inode);
int muzix_fs_service_lookup_root(muzix_fs_service_t *fs,
                                 const char *name,
                                 uint16_t *inode);
int muzix_fs_service_open_root(muzix_fs_service_t *fs,
                               const char *name);
int muzix_fs_service_open_path(muzix_fs_service_t *fs,
                               const char *path);
int muzix_fs_service_open_path_flags(muzix_fs_service_t *fs,
                                     const char *path,
                                     uint16_t flags,
                                     uint16_t uid,
                                     uint16_t gid);
int muzix_fs_service_creat_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t umask);
int muzix_fs_service_mknod_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t device,
                                uint16_t euid,
                                uint16_t umask);
int muzix_fs_service_chdir(muzix_fs_service_t *fs, const char *path);
int muzix_fs_service_getcwd(muzix_fs_service_t *fs,
                            char *buffer,
                            size_t size);
int muzix_fs_service_close(muzix_fs_service_t *fs, int handle);
int muzix_fs_service_dup(muzix_fs_service_t *fs, int handle);
int muzix_fs_service_pipe(muzix_fs_service_t *fs, int fds[2]);
void muzix_fs_service_attach_tty(muzix_fs_service_t *fs,
                                 muzix_tty_device_t *tty);
int muzix_fs_service_open_tty(muzix_fs_service_t *fs);
int muzix_fs_service_ioctl(muzix_fs_service_t *fs,
                           int handle,
                           uint16_t request,
                           void *argument);
int muzix_fs_service_readdir(muzix_fs_service_t *fs,
                             int handle,
                             uint16_t index,
                             muzix_fs_dirent_t *entry);
int muzix_fs_service_seek(muzix_fs_service_t *fs,
                          int handle,
                          uint32_t position);
int muzix_fs_service_unlink_root(muzix_fs_service_t *fs,
                                 const char *name);
int muzix_fs_service_unlink_path(muzix_fs_service_t *fs,
                                 const char *path);
int muzix_fs_service_link_path(muzix_fs_service_t *fs,
                               const char *old_path,
                               const char *new_path);
int muzix_fs_service_rmdir_root(muzix_fs_service_t *fs,
                                const char *name);
int muzix_fs_service_rmdir_path(muzix_fs_service_t *fs,
                                const char *path);
int muzix_fs_service_rename_root(muzix_fs_service_t *fs,
                                 const char *old_name,
                                 const char *new_name);
int muzix_fs_service_rename_path(muzix_fs_service_t *fs,
                                 const char *old_path,
                                 const char *new_path);
int muzix_fs_service_stat_root(muzix_fs_service_t *fs,
                               const char *name,
                               muzix_fs_inode_t *stat);
int muzix_fs_service_stat_path(muzix_fs_service_t *fs,
                               const char *path,
                               muzix_fs_inode_t *stat);
int muzix_fs_service_fstat(muzix_fs_service_t *fs,
                           int handle,
                           muzix_fs_inode_t *stat);
int muzix_fs_service_chmod_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t uid);
int muzix_fs_service_access_path(muzix_fs_service_t *fs,
                                 const char *path,
                                 uint16_t requested,
                                 uint16_t uid,
                                 uint16_t gid);
int muzix_fs_service_sync(muzix_fs_service_t *fs);
int muzix_fs_service_read(muzix_fs_service_t *fs,
                          int handle,
                          uint8_t *buffer,
                          size_t length,
                          size_t *result);
int muzix_fs_service_write(muzix_fs_service_t *fs,
                           int handle,
                           const uint8_t *buffer,
                           size_t length,
                           size_t *result);
int muzix_fs_service_read_inode(muzix_fs_service_t *fs,
                                uint16_t inode,
                                uint32_t position,
                                uint8_t *buffer,
                                size_t length,
                                size_t *result);
int muzix_fs_service_write_inode(muzix_fs_service_t *fs,
                                 uint16_t inode,
                                 uint32_t position,
                                 const uint8_t *buffer,
                                 size_t length,
                                 size_t *result);


#endif
