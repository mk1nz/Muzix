#include "fs_service.h"

#include <string.h>
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/trace.h"

#define MUZIX_FS_OPEN_KIND_INODE 0u
#define MUZIX_FS_OPEN_KIND_PIPE 1u
#define MUZIX_FS_OPEN_KIND_TTY 2u

/* The largest file this inode layout can describe: 7 direct zones plus one
 * indirect block of 256 entries.  Positions past it have no zone to point at,
 * so they are rejected instead of being truncated (see file_io.h). */
#define MUZIX_FS_MAX_FILE_SIZE \
    ((uint32_t)MUZIX_FS_ZONE_SLOTS * (uint32_t)MUZIX_FS_BLOCK_SIZE)

static int muzix_fs_service_lookup_path(muzix_fs_service_t *fs,
                                        const char *path,
                                        uint16_t *inode);
static int muzix_fs_service_normalize_path(const muzix_fs_service_t *fs,
                                           const char *path,
                                           char *output,
                                           size_t size);
static int muzix_fs_service_lookup_parent(muzix_fs_service_t *fs,
                                          char *normalized,
                                          const char **leaf,
                                          uint16_t *parent_inode);
static muzix_fs_directory_t *muzix_fs_service_dir_load(muzix_fs_service_t *fs,
                                                       uint16_t inode,
                                                       uint16_t *zone);
static int muzix_fs_service_dir_store(muzix_fs_service_t *fs,
                                      uint16_t inode,
                                      uint16_t zone,
                                      const muzix_fs_directory_t *directory);
static int muzix_fs_service_scratch_acquire(muzix_fs_service_t *fs);
static void muzix_fs_service_scratch_release(muzix_fs_service_t *fs);

/* Decide what a rename of old_path to new_path does to the current directory.
 * 0 = cwd_path has to follow the move, 1 = it is unaffected, -1 = the result
 * would not fit in cwd_path.  Kept separate from the rewrite itself so the
 * decision can be made before anything is modified: rename_path applies the
 * rewrite only after the rename is durable, and holding its 64-byte staging
 * buffer for the whole operation does not fit on the stack. */
static int muzix_fs_service_cwd_rewrite(const muzix_fs_service_t *fs,
                                        const char *old_path,
                                        const char *new_path)
{
    size_t old_length;
    size_t new_length;
    size_t suffix_length;

    if (!fs || !old_path || !new_path) {
        return -1;
    }
    old_length = strlen(old_path);
    if (strcmp(fs->cwd_path, old_path) != 0 &&
        (strncmp(fs->cwd_path, old_path, old_length) != 0 ||
         fs->cwd_path[old_length] != '/')) {
        return 1;
    }
    new_length = strlen(new_path);
    suffix_length = strlen(fs->cwd_path) - old_length;
    if (new_length + suffix_length >= MUZIX_FS_PATH_MAX) {
        return -1;
    }
    return 0;
}

static int muzix_fs_service_rewrite_cwd(const muzix_fs_service_t *fs,
                                        const char *old_path,
                                        const char *new_path,
                                        char *output,
                                        size_t size)
{
    size_t new_length;
    int decision;

    if (!fs || !old_path || !new_path || !output || size == 0) {
        return -1;
    }
    /* The fit test is against cwd_path's own capacity, which every caller
     * passes as size, so a caller cannot be talked into a too-long rewrite. */
    decision = muzix_fs_service_cwd_rewrite(fs, old_path, new_path);
    if (decision != 0) {
        return decision;
    }
    new_length = strlen(new_path);
    memcpy(output, new_path, new_length);
    memcpy(output + new_length, fs->cwd_path + strlen(old_path),
           strlen(fs->cwd_path) - strlen(old_path) + 1);
    return 0;
}

/* Move the current directory along with a completed rename.  The staging
 * buffer lives in this frame, which is only entered once nothing else is on the
 * stack. */
static void muzix_fs_service_apply_cwd(muzix_fs_service_t *fs,
                                       const char *old_path,
                                       const char *new_path)
{
    char buffer[MUZIX_FS_PATH_MAX];

    if (muzix_fs_service_rewrite_cwd(fs, old_path, new_path, buffer,
                                     sizeof(buffer)) == 0) {
        memcpy(fs->cwd_path, buffer, sizeof(buffer));
    }
}

static int muzix_fs_service_lookup_component(void *context,
                                             uint16_t directory_inode,
                                             const char *name,
                                             uint16_t *inode)
{
    muzix_fs_service_t *fs = (muzix_fs_service_t *)context;
    muzix_fs_inode_t *directory;
    muzix_fs_directory_t entries;

    if (!fs || !fs->volume || !fs->volume->mounted || !fs->inodes ||
        !fs->cache || !inode) {
        return -1;
    }
    directory = muzix_fs_inode_get(fs->inodes, directory_inode);
    if (!directory || (directory->mode & 0x4000u) == 0 ||
        directory->zone[0] == 0 ||
        muzix_fs_directory_load(fs->cache, directory->zone[0], &entries) != 0) {
        return -1;
    }
    return muzix_fs_directory_lookup(&entries, name, inode);
}

void muzix_fs_service_init(muzix_fs_service_t *fs,
                           muzix_system_task_t *system_task,
                           uint8_t source)
{
    if (!fs) {
        return;
    }

    memset(fs, 0, sizeof(*fs));
    fs->system_task = system_task;
    fs->cwd_inode = 1;
    fs->cwd_path[0] = '/';
    fs->source = source;
}



void muzix_fs_service_attach_volume(muzix_fs_service_t *fs,
                                    muzix_fs_volume_t *volume)
{
    if (!fs) {
        return;
    }

    fs->volume = volume;
    if (volume) {
        fs->device = volume->device;
        fs->cache = &volume->cache;
        fs->inodes = &volume->inodes;
    }
}

void muzix_fs_service_attach_tty(muzix_fs_service_t *fs,
                                 muzix_tty_device_t *tty)
{
    if (fs) {
        fs->tty = tty;
    }
}




int muzix_fs_service_mkdir_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t umask,
                                uint16_t *inode)
{
    char normalized[MUZIX_FS_PATH_MAX];
    const char *leaf;
    muzix_fs_directory_t *directory;
    muzix_fs_inode_t *entry;
    uint16_t parent_inode;
    uint16_t parent_zone;
    uint16_t zone;
    int status = -1;

    if (!fs || !path || !inode || !fs->volume || !fs->volume->mounted ||
        muzix_fs_service_normalize_path(fs, path, normalized,
                                        sizeof(normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, normalized, &leaf,
                                       &parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }

    /* The child directory is written to its own zone before the parent is
     * touched, so the single shared image is used once for the parent lookup,
     * once for the child and once for the parent update.  Keeping two live
     * images here instead cost 512 bytes of frame, on top of the 143 bytes of
     * path buffers the operation still needs. */
    directory = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!directory || muzix_fs_directory_lookup(directory, leaf, 0) == 0 ||
        muzix_fs_inode_alloc(&fs->volume->inodes, inode) != 0) {
        goto done;
    }
    if (muzix_fs_zone_alloc(&fs->volume->allocator, &zone) != 0) {
        muzix_fs_inode_release(&fs->volume->inodes, *inode);
        goto done;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, *inode);
    entry->mode = (uint16_t)(MUZIX_FS_MODE_DIR |
                             (mode & 0777u & (uint16_t)~umask));
    entry->links = 2;
    entry->zone[0] = zone;
    muzix_fs_directory_init(&fs->scratch);
    if (muzix_fs_directory_add(&fs->scratch, *inode, ".") != 0 ||
        muzix_fs_directory_add(&fs->scratch, parent_inode, "..") != 0 ||
        muzix_fs_directory_save(&fs->volume->cache, zone, &fs->scratch) != 0) {
        muzix_fs_zone_free(&fs->volume->allocator, zone);
        muzix_fs_inode_release(&fs->volume->inodes, *inode);
        goto done;
    }

    directory = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!directory ||
        muzix_fs_directory_add(directory, *inode, leaf) != 0 ||
        muzix_fs_service_dir_store(fs, parent_inode, parent_zone,
                                   directory) != 0) {
        muzix_fs_zone_free(&fs->volume->allocator, zone);
        muzix_fs_inode_release(&fs->volume->inodes, *inode);
        goto done;
    }
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        goto done;
    }
    status = 0;

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}

int muzix_fs_service_lookup_root(muzix_fs_service_t *fs,
                                 const char *name,
                                 uint16_t *inode)
{
    if (!fs || !fs->volume || !fs->volume->mounted) {
        return -1;
    }
    return muzix_fs_directory_lookup(&fs->volume->root_directory, name, inode);
}

static int muzix_fs_service_lookup_path(muzix_fs_service_t *fs,
                                        const char *path,
                                        uint16_t *inode)
{
    uint16_t base;

    if (!fs || !path || !inode || !fs->volume || !fs->volume->mounted) {
        return -1;
    }
    base = path[0] == '/' ? 1 : fs->cwd_inode;
    return muzix_fs_path_lookup(fs, base, path,
                                muzix_fs_service_lookup_component, inode);
}

static int muzix_fs_service_normalize_path(const muzix_fs_service_t *fs,
                                           const char *path,
                                           char *output,
                                           size_t size)
{
    char combined[MUZIX_FS_PATH_MAX * 2];
    char component[MUZIX_FS_NAME_MAX + 1];
    size_t combined_length;
    size_t output_length = 1;
    size_t component_length;
    const char *cursor;

    if (!fs || !path || !output || size < 2) {
        return -1;
    }
    if (path[0] == '/') {
        combined_length = strlen(path);
        if (combined_length >= sizeof(combined)) {
            return -1;
        }
        memcpy(combined, path, combined_length + 1);
    } else {
        combined_length = strlen(fs->cwd_path) + 1 + strlen(path);
        if (combined_length >= sizeof(combined)) {
            return -1;
        }
        strcpy(combined, fs->cwd_path);
        if (strcmp(combined, "/") != 0) {
            strcat(combined, "/");
        }
        strcat(combined, path);
    }

    output[0] = '/';
    output[1] = '\0';
    cursor = combined;
    while (*cursor) {
        while (*cursor == '/') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        component_length = 0;
        while (cursor[component_length] && cursor[component_length] != '/') {
            /* MUZIX_FS_NAME_MAX characters are storable: a directory entry
             * holds 14 name bytes, muzix_fs_name_valid accepts 14, and the
             * image tool writes 14.  Rejecting at 13 made a 14-character name
             * visible to ls/open yet unreachable for creat, unlink, rename and
             * mkdir, so the name could never be created or removed again. */
            if (component_length > MUZIX_FS_NAME_MAX) {
                return -1;
            }
            component[component_length] = cursor[component_length];
            component_length++;
        }
        component[component_length] = '\0';
        cursor += component_length;
        if (strcmp(component, ".") == 0) {
            continue;
        }
        if (strcmp(component, "..") == 0) {
            if (output_length > 1) {
                output_length--;
                while (output_length > 1 && output[output_length - 1] != '/') {
                    output_length--;
                }
                output[output_length] = '\0';
            }
            continue;
        }
        if (output_length > 1) {
            if (output_length + component_length + 1 >= size) {
                return -1;
            }
            output[output_length++] = '/';
        } else if (output_length + component_length >= size) {
            return -1;
        }
        memcpy(&output[output_length], component, component_length);
        output_length += component_length;
        output[output_length] = '\0';
    }
    return 0;
}

/* Resolve the directory that holds an already-normalized absolute path and the
 * name inside it.  The path is cut at its last '/' for the duration of the
 * lookup and restored before returning, which removes the separate 64-byte
 * parent buffer and the 15-byte name copy every path operation used to carry:
 * with the 455-byte user-syscall dispatcher and the 260-byte
 * muzix_fs_service_lookup_component frame already live below, those two
 * buffers alone pushed the chain past the 1317-byte stack. */
static int muzix_fs_service_lookup_parent(muzix_fs_service_t *fs,
                                          char *normalized,
                                          const char **leaf,
                                          uint16_t *parent_inode)
{
    char *slash;
    char saved;
    int status;

    if (!fs || !normalized || !leaf || !parent_inode ||
        normalized[0] != '/' || normalized[1] == '\0') {
        return -1;
    }
    slash = strrchr(normalized, '/');
    if (!slash || slash[1] == '\0' || strlen(slash + 1) > MUZIX_FS_NAME_MAX) {
        return -1;
    }
    *leaf = slash + 1;
    if (slash == normalized) {
        /* Directly under the root: nothing to look up. */
        *parent_inode = 1;
        return 0;
    }
    saved = *slash;
    *slash = '\0';
    status = muzix_fs_service_lookup_path(fs, normalized, parent_inode);
    *slash = saved;
    return status;
}

/* Take exclusive use of the shared directory image. */
static int muzix_fs_service_scratch_acquire(muzix_fs_service_t *fs)
{
    if (!fs || fs->scratch_busy) {
        return -1;
    }
    fs->scratch_busy = 1;
    return 0;
}

static void muzix_fs_service_scratch_release(muzix_fs_service_t *fs)
{
    if (fs) {
        fs->scratch_busy = 0;
    }
}

/* Directory image for `inode`, ready to be modified.  The root is the live
 * volume object (volume_flush writes it out from there); every other directory
 * is read into the shared scratch image, which stays valid until the next
 * load.  *zone receives the backing zone, 0 for the root. */
static muzix_fs_directory_t *muzix_fs_service_dir_load(muzix_fs_service_t *fs,
                                                       uint16_t inode,
                                                       uint16_t *zone)
{
    muzix_fs_inode_t *entry;

    if (!fs || !fs->volume || !zone) {
        return 0;
    }
    if (inode == 1) {
        *zone = 0;
        return &fs->volume->root_directory;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    if (!entry || (entry->mode & MUZIX_FS_MODE_DIR) == 0 ||
        entry->zone[0] == 0) {
        return 0;
    }
    *zone = entry->zone[0];
    if (muzix_fs_directory_load(&fs->volume->cache, *zone,
                                &fs->scratch) != 0) {
        return 0;
    }
    return &fs->scratch;
}

/* Write a modified directory image back.  The root is flushed from the live
 * volume object by muzix_fs_volume_flush, so there is nothing to store. */
static int muzix_fs_service_dir_store(muzix_fs_service_t *fs,
                                      uint16_t inode,
                                      uint16_t zone,
                                      const muzix_fs_directory_t *directory)
{
    if (!fs || !directory) {
        return -1;
    }
    if (inode == 1) {
        return 0;
    }
    return muzix_fs_directory_save(&fs->volume->cache, zone, directory);
}

/* Put `name` back into a directory that a failed operation had already removed
 * it from.  Undo is done with inverse directory operations rather than a saved
 * copy of the whole image: a second 256-byte snapshot does not fit on the
 * stack, and the entry's inode number is all that is needed to restore it. */
static int muzix_fs_service_dir_restore_entry(muzix_fs_service_t *fs,
                                              uint16_t parent_inode,
                                              const char *name,
                                              uint16_t child_inode)
{
    uint16_t zone;
    muzix_fs_directory_t *directory =
        muzix_fs_service_dir_load(fs, parent_inode, &zone);

    if (!directory || muzix_fs_directory_add(directory, child_inode, name) != 0) {
        return -1;
    }
    return muzix_fs_service_dir_store(fs, parent_inode, zone, directory);
}

/* Undo a directory_add: take `name` back out.  An entry that is already absent
 * is not a failure -- the wanted end state is "not present", and on a
 * read-only image the change may never have reached the cache. */
static int muzix_fs_service_dir_drop_entry(muzix_fs_service_t *fs,
                                           uint16_t parent_inode,
                                           const char *name)
{
    uint16_t zone;
    muzix_fs_directory_t *directory =
        muzix_fs_service_dir_load(fs, parent_inode, &zone);

    if (!directory) {
        return -1;
    }
    if (muzix_fs_directory_lookup(directory, name, 0) != 0) {
        return 0;
    }
    if (muzix_fs_directory_remove(directory, name) != 0) {
        return -1;
    }
    return muzix_fs_service_dir_store(fs, parent_inode, zone, directory);
}

static muzix_fs_open_description_t *muzix_fs_service_description(
    muzix_fs_service_t *fs, int handle)
{
    if (!fs || handle < 0 || handle >= MUZIX_FS_OPEN_FILES ||
        !fs->open_files[handle].used ||
        fs->open_files[handle].description >= MUZIX_FS_OPEN_DESCRIPTIONS) {
        return 0;
    }
    if (!fs->open_descriptions[fs->open_files[handle].description].used) {
        return 0;
    }
    return &fs->open_descriptions[fs->open_files[handle].description];
}

static int muzix_fs_service_check_inode_access(const muzix_fs_inode_t *inode,
                                               uint16_t requested,
                                               uint16_t uid,
                                               uint16_t gid)
{
    uint16_t available;
    uint16_t permissions;

    if (!inode || requested == 0) {
        return inode ? 0 : -1;
    }
    permissions = inode->mode & 0777u;
    if (permissions == 0) {
        return 0;
    }
    if (uid == 0) {
        if ((requested & MUZIX_FS_ACCESS_EXEC) == 0) {
            return 0;
        }
        available = (uint16_t)(permissions & 7u);
        return (available & MUZIX_FS_ACCESS_EXEC) != 0 ||
                       (permissions & 0700u) != 0
                   ? 0
                   : -1;
    }
    if (uid == inode->uid) {
        available = (uint16_t)((permissions >> 6) & 7u);
    } else if (gid == inode->gid) {
        available = (uint16_t)((permissions >> 3) & 7u);
    } else {
        available = (uint16_t)(permissions & 7u);
    }
    return (available & requested) == requested ? 0 : -1;
}

static int muzix_fs_service_open_inode(muzix_fs_service_t *fs,
                                       uint16_t inode,
                                       uint8_t access,
                                       uint16_t uid,
                                       uint16_t gid)
{
    muzix_fs_inode_t *entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    uint8_t slot;
    uint8_t description;

    if (!entry || (access & (MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE)) == 0 ||
        muzix_fs_service_check_inode_access(entry, access, uid, gid) != 0) {
        return -1;
    }

    for (slot = 0; slot < MUZIX_FS_OPEN_FILES; slot++) {
        if (!fs->open_files[slot].used) {
            break;
        }
    }
    if (slot == MUZIX_FS_OPEN_FILES) {
        return -1;
    }
    for (description = 0; description < MUZIX_FS_OPEN_DESCRIPTIONS; description++) {
        if (!fs->open_descriptions[description].used) {
            fs->open_descriptions[description].used = 1;
            fs->open_descriptions[description].refs = 1;
            fs->open_descriptions[description].kind =
                (entry->mode & MUZIX_FS_MODE_TYPE) == MUZIX_FS_MODE_CHAR &&
                        entry->zone[0] == MUZIX_FS_TTY_DEVICE && fs->tty
                    ? MUZIX_FS_OPEN_KIND_TTY
                    : MUZIX_FS_OPEN_KIND_INODE;
            fs->open_descriptions[description].pipe = 0;
            fs->open_descriptions[description].inode = inode;
            fs->open_descriptions[description].position = 0;
            fs->open_descriptions[description].access = access;
            fs->open_files[slot].used = 1;
            fs->open_files[slot].description = description;
            return slot;
        }
    }
    return -1;
}

int muzix_fs_service_dup(muzix_fs_service_t *fs, int handle)
{
    muzix_fs_open_description_t *description;
    uint8_t slot;

    description = muzix_fs_service_description(fs, handle);
    if (!description || description->refs == 0xffu) {
        return -1;
    }
    for (slot = 0; slot < MUZIX_FS_OPEN_FILES; slot++) {
        if (!fs->open_files[slot].used) {
            fs->open_files[slot].used = 1;
            fs->open_files[slot].description =
                fs->open_files[handle].description;
            description->refs++;
            return slot;
        }
    }
    return -1;
}

int muzix_fs_service_open_tty(muzix_fs_service_t *fs)
{
    uint8_t slot;
    uint8_t description;

    if (!fs || !fs->tty) {
        return -1;
    }
    for (slot = 0; slot < MUZIX_FS_OPEN_FILES; slot++) {
        if (!fs->open_files[slot].used) {
            break;
        }
    }
    if (slot == MUZIX_FS_OPEN_FILES) {
        return -1;
    }
    for (description = 0; description < MUZIX_FS_OPEN_DESCRIPTIONS; description++) {
        if (!fs->open_descriptions[description].used) {
            fs->open_descriptions[description].used = 1;
            fs->open_descriptions[description].refs = 1;
            fs->open_descriptions[description].kind = MUZIX_FS_OPEN_KIND_TTY;
            fs->open_descriptions[description].access =
                MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE;
            fs->open_files[slot].used = 1;
            fs->open_files[slot].description = description;
            return slot;
        }
    }
    return -1;
}

int muzix_fs_service_ioctl(muzix_fs_service_t *fs,
                           int handle,
                           uint16_t request,
                           void *argument)
{
    muzix_fs_open_description_t *description =
        muzix_fs_service_description(fs, handle);

    if (!description || description->kind != MUZIX_FS_OPEN_KIND_TTY ||
        !fs->tty) {
        return -1;
    }
    return muzix_tty_ioctl(fs->tty, request, argument);
}

int muzix_fs_service_pipe(muzix_fs_service_t *fs, int fds[2])
{
    uint8_t pipe_slot;
    uint8_t description_a = MUZIX_FS_OPEN_DESCRIPTIONS;
    uint8_t description_b = MUZIX_FS_OPEN_DESCRIPTIONS;
    uint8_t file_a = MUZIX_FS_OPEN_FILES;
    uint8_t file_b = MUZIX_FS_OPEN_FILES;
    uint8_t index;

    if (!fs || !fds) {
        return -1;
    }
    for (pipe_slot = 0; pipe_slot < MUZIX_FS_PIPE_SLOTS; pipe_slot++) {
        if (!fs->pipes[pipe_slot].used) {
            break;
        }
    }
    if (pipe_slot == MUZIX_FS_PIPE_SLOTS) {
        return -1;
    }
    /* The two tables have separate capacities, so scan each one against its
     * own limit.  The old shared loop walked one index across both tables up
     * to the descriptor count, which is only correct while the two counts
     * happen to be equal -- and it needed two free slots in *each* table. */
    for (index = 0; index < MUZIX_FS_OPEN_FILES; index++) {
        if (!fs->open_files[index].used) {
            if (file_a == MUZIX_FS_OPEN_FILES) {
                file_a = index;
            } else if (file_b == MUZIX_FS_OPEN_FILES) {
                file_b = index;
            }
        }
    }
    for (index = 0; index < MUZIX_FS_OPEN_DESCRIPTIONS; index++) {
        if (!fs->open_descriptions[index].used) {
            if (description_a == MUZIX_FS_OPEN_DESCRIPTIONS) {
                description_a = index;
            } else if (description_b == MUZIX_FS_OPEN_DESCRIPTIONS) {
                description_b = index;
            }
        }
    }
    if (file_b == MUZIX_FS_OPEN_FILES ||
        description_b == MUZIX_FS_OPEN_DESCRIPTIONS) {
        return -1;
    }
    memset(&fs->pipes[pipe_slot], 0, sizeof(fs->pipes[pipe_slot]));
    fs->pipes[pipe_slot].used = 1;
    fs->pipes[pipe_slot].readers = 1;
    fs->pipes[pipe_slot].writers = 1;
    fs->open_descriptions[description_a].used = 1;
    fs->open_descriptions[description_a].refs = 1;
    fs->open_descriptions[description_a].kind = MUZIX_FS_OPEN_KIND_PIPE;
    fs->open_descriptions[description_a].pipe = pipe_slot;
    fs->open_descriptions[description_a].access = MUZIX_FS_ACCESS_READ;
    fs->open_descriptions[description_b] = fs->open_descriptions[description_a];
    fs->open_descriptions[description_b].access = MUZIX_FS_ACCESS_WRITE;
    fs->open_files[file_a].used = 1;
    fs->open_files[file_a].description = description_a;
    fs->open_files[file_b].used = 1;
    fs->open_files[file_b].description = description_b;
    fds[0] = file_a;
    fds[1] = file_b;
    return 0;
}

int muzix_fs_service_open_root(muzix_fs_service_t *fs,
                               const char *name)
{
    uint16_t inode;

    if (!fs || muzix_fs_service_lookup_root(fs, name, &inode) != 0) {
        return -1;
    }
    return muzix_fs_service_open_inode(
        fs, inode, MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE, 0, 0);
}

int muzix_fs_service_open_path(muzix_fs_service_t *fs,
                               const char *path)
{
    return muzix_fs_service_open_path_flags(
        fs, path, MUZIX_FS_OPEN_RDWR, 0, 0);
}

int muzix_fs_service_open_path_flags(muzix_fs_service_t *fs,
                                     const char *path,
                                     uint16_t flags,
                                     uint16_t uid,
                                     uint16_t gid)
{
    uint16_t inode;
    uint8_t access;

    if (muzix_fs_service_lookup_path(fs, path, &inode) != 0) {
        return -1;
    }
    if (flags == MUZIX_FS_OPEN_RDONLY) {
        access = MUZIX_FS_ACCESS_READ;
    } else if (flags == MUZIX_FS_OPEN_WRONLY) {
        access = MUZIX_FS_ACCESS_WRITE;
    } else if (flags == MUZIX_FS_OPEN_RDWR) {
        access = MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE;
    } else {
        return -1;
    }
    return muzix_fs_service_open_inode(fs, inode, access, uid, gid);
}

int muzix_fs_service_mknod_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t device,
                                uint16_t euid,
                                uint16_t umask)
{
    char normalized[MUZIX_FS_PATH_MAX];
    const char *leaf;
    muzix_fs_directory_t *directory;
    muzix_fs_inode_t *entry;
    uint16_t parent_inode;
    uint16_t parent_zone;
    uint16_t inode;
    int status = -1;

    if (!fs || !path || euid != 0 || !fs->volume || !fs->volume->mounted ||
        !fs->tty || (mode & MUZIX_FS_MODE_TYPE) != MUZIX_FS_MODE_CHAR ||
        device != MUZIX_FS_TTY_DEVICE ||
        muzix_fs_service_normalize_path(fs, path, normalized,
                                        sizeof(normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, normalized, &leaf,
                                       &parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }

    directory = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!directory || muzix_fs_directory_lookup(directory, leaf, 0) == 0 ||
        muzix_fs_inode_alloc(&fs->volume->inodes, &inode) != 0) {
        goto done;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    entry->mode = (uint16_t)(MUZIX_FS_MODE_CHAR |
                             (mode & 0777u & (uint16_t)~umask));
    entry->zone[0] = device;
    entry->links = 1;
    if (muzix_fs_directory_add(directory, inode, leaf) != 0 ||
        muzix_fs_service_dir_store(fs, parent_inode, parent_zone,
                                   directory) != 0) {
        muzix_fs_inode_release(&fs->volume->inodes, inode);
        goto done;
    }
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        goto done;
    }
    status = 0;

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}

static int muzix_fs_service_truncate_inode(muzix_fs_service_t *fs,
                                           muzix_fs_inode_t *inode)
{
    /* Both the inode and the zone bitmap are modified as the loop runs, and a
     * failure used to be reported only after zones 0..i-1 had already been
     * freed and their pointers zeroed.  RAM then claimed the blocks were free
     * while the image still listed them, and the next allocation handed the
     * same zones to a different file.  Restore both structures so the caller
     * either sees the whole truncate or none of it; the 68 bytes of snapshot
     * are the only way to do that without a second 288-byte inode table copy
     * on a 1317-byte stack. */
    muzix_fs_zone_allocator_t allocator_snapshot = fs->volume->allocator;
    muzix_fs_inode_t inode_snapshot = *inode;
    uint8_t *data;
    uint16_t zone;
    uint16_t offset;

    for (uint8_t index = 0; index < MUZIX_FS_DIRECT_ZONES; index++) {
        zone = inode->zone[index];
        if (zone != 0 && muzix_fs_zone_free(&fs->volume->allocator, zone) != 0) {
            fs->volume->allocator = allocator_snapshot;
            *inode = inode_snapshot;
            return -1;
        }
        inode->zone[index] = 0;
    }
    if (inode->indirect_zone != 0) {
        if (muzix_fs_cache_get(&fs->volume->cache, inode->indirect_zone,
                               &data) != 0) {
            fs->volume->allocator = allocator_snapshot;
            *inode = inode_snapshot;
            return -1;
        }
        for (offset = 0; offset + 1 < MUZIX_FS_BLOCK_SIZE; offset += 2) {
            zone = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
            if (zone != 0 && muzix_fs_zone_free(&fs->volume->allocator, zone) != 0) {
                fs->volume->allocator = allocator_snapshot;
                *inode = inode_snapshot;
                return -1;
            }
        }
        if (muzix_fs_zone_free(&fs->volume->allocator, inode->indirect_zone) != 0) {
            fs->volume->allocator = allocator_snapshot;
            *inode = inode_snapshot;
            return -1;
        }
        inode->indirect_zone = 0;
    }
    inode->size = 0;
    return 0;
}

int muzix_fs_service_creat_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t umask)
{
    char normalized[MUZIX_FS_PATH_MAX];
    const char *leaf;
    muzix_fs_directory_t *parent;
    muzix_fs_inode_t *entry;
    muzix_fs_inode_t entry_snapshot;
    muzix_fs_zone_allocator_t allocator_snapshot;
    uint16_t parent_inode;
    uint16_t parent_zone;
    uint16_t inode = 0;
    int handle;
    int existing;
    int created = 0;
    int status = -1;

    if (!fs || !path || !fs->volume || !fs->volume->mounted ||
        muzix_fs_service_normalize_path(fs, path, normalized,
                                        sizeof(normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, normalized, &leaf,
                                       &parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }
    handle = muzix_fs_service_open_inode(
        fs, 1, MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE, 0, 0);
    if (handle < 0) {
        goto done;
    }
    /* Scratch handle on the root: it only exists to reserve a file number, so
     * release it again before the work proper. */
    fs->open_descriptions[fs->open_files[handle].description].used = 0;
    fs->open_descriptions[fs->open_files[handle].description].refs = 0;
    fs->open_files[handle].used = 0;

    parent = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!parent) {
        goto done;
    }

    /* Snapshot what creat is about to change, so a failure to flush leaves no
     * half-truncated file behind: without it the zones were released in RAM
     * while the image kept the old pointers, and a later file could be handed
     * the same zones. */
    existing = muzix_fs_directory_lookup(parent, leaf, &inode) == 0;
    if (existing) {
        entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
        if (!entry || (entry->mode & 0x4000u) != 0) {
            goto done;
        }
        entry_snapshot = *entry;
        allocator_snapshot = fs->volume->allocator;
        if (muzix_fs_service_truncate_inode(fs, entry) != 0) {
            goto done;
        }
    } else {
        if (muzix_fs_inode_alloc(&fs->volume->inodes, &inode) != 0) {
            goto done;
        }
        entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
        entry_snapshot = *entry;
        allocator_snapshot = fs->volume->allocator;
        entry->mode = (uint16_t)(MUZIX_FS_MODE_REGULAR |
                     (mode & 0777u & (uint16_t)~umask));
        entry->links = 1;
        if (muzix_fs_directory_add(parent, inode, leaf) != 0) {
            muzix_fs_inode_release(&fs->volume->inodes, inode);
            goto done;
        }
        if (muzix_fs_service_dir_store(fs, parent_inode, parent_zone,
                                       parent) != 0) {
            muzix_fs_service_dir_drop_entry(fs, parent_inode, leaf);
            muzix_fs_inode_release(&fs->volume->inodes, inode);
            goto done;
        }
        created = 1;
    }
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        if (created) {
            muzix_fs_service_dir_drop_entry(fs, parent_inode, leaf);
            muzix_fs_inode_release(&fs->volume->inodes, inode);
        } else {
            fs->volume->allocator = allocator_snapshot;
            fs->volume->inodes.entries[inode] = entry_snapshot;
        }
        goto done;
    }
    status = muzix_fs_service_open_inode(
        fs, inode, MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE, 0, 0);

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}

int muzix_fs_service_chdir(muzix_fs_service_t *fs, const char *path)
{
    muzix_fs_inode_t *entry;
    uint16_t inode;
    char normalized[MUZIX_FS_PATH_MAX];

    if (muzix_fs_service_lookup_path(fs, path, &inode) != 0) {
        return -1;
    }
    entry = muzix_fs_inode_get(fs->inodes, inode);
    if (!entry || (entry->mode & 0x4000u) == 0) {
        return -1;
    }
    if (muzix_fs_service_normalize_path(fs, path, normalized,
                                         sizeof(normalized)) != 0) {
        return -1;
    }
    memcpy(fs->cwd_path, normalized, sizeof(normalized));
    fs->cwd_inode = inode;
    return 0;
}

int muzix_fs_service_getcwd(muzix_fs_service_t *fs,
                            char *buffer,
                            size_t size)
{
    size_t length;

    if (!fs || !buffer || size == 0) {
        return -1;
    }
    length = strlen(fs->cwd_path);
    if (length + 1 > size) {
        return -1;
    }
    memcpy(buffer, fs->cwd_path, length + 1);
    return 0;
}

static int muzix_fs_service_reclaim_inode(muzix_fs_service_t *fs,
                                           uint16_t inode)
{
    muzix_fs_inode_t *entry;
    uint8_t *indirect;

    for (uint8_t description = 0; description < MUZIX_FS_OPEN_DESCRIPTIONS;
         description++) {
        if (fs->open_descriptions[description].used &&
            fs->open_descriptions[description].kind == MUZIX_FS_OPEN_KIND_INODE &&
            fs->open_descriptions[description].inode == inode) {
            return 0;
        }
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    if (!entry || entry->links != 0) {
        return 0;
    }
    /* A device inode stores the device id in zone[0] (MUZIX_FS_TTY_DEVICE for
     * the only kind mknod can create), not a zone number.  Treating it as a
     * zone made `unlink /dev/tty` hand 0x0100 to the bitmap -- past
     * MUZIX_FS_MAX_ZONES, so the free silently failed -- and then release the
     * inode, destroying the only device node in the tree.  Device inodes own
     * no zones, so there is nothing to reclaim; only the inode goes. */
    if ((entry->mode & MUZIX_FS_MODE_TYPE) == MUZIX_FS_MODE_CHAR ||
        (entry->mode & MUZIX_FS_MODE_TYPE) == MUZIX_FS_MODE_BLOCK) {
        return muzix_fs_inode_release(&fs->volume->inodes, inode);
    }
    for (uint8_t index = 0; index < MUZIX_FS_DIRECT_ZONES; index++) {
        if (entry->zone[index] != 0) {
            muzix_fs_zone_free(&fs->volume->allocator, entry->zone[index]);
            entry->zone[index] = 0;
        }
    }
    if (entry->indirect_zone != 0) {
        if (muzix_fs_cache_get(&fs->volume->cache, entry->indirect_zone,
                               &indirect) == 0) {
            for (uint16_t offset = 0; offset + 1 < MUZIX_FS_BLOCK_SIZE;
                 offset += 2) {
                uint16_t zone = (uint16_t)indirect[offset] |
                                ((uint16_t)indirect[offset + 1] << 8);
                if (zone != 0) {
                    muzix_fs_zone_free(&fs->volume->allocator, zone);
                }
            }
        }
        muzix_fs_zone_free(&fs->volume->allocator, entry->indirect_zone);
        entry->indirect_zone = 0;
    }
    return muzix_fs_inode_release(&fs->volume->inodes, inode);
}

int muzix_fs_service_close(muzix_fs_service_t *fs, int handle)
{
    muzix_fs_open_description_t *description;
    uint16_t inode;
    int flush_needed;

    description = muzix_fs_service_description(fs, handle);
    if (!description) {
        return -1;
    }
    inode = description->inode;
    flush_needed = (description->access & MUZIX_FS_ACCESS_WRITE) != 0;
    fs->open_files[handle].used = 0;
    if (description->kind == MUZIX_FS_OPEN_KIND_PIPE) {
        muzix_fs_pipe_t *pipe = &fs->pipes[description->pipe];
        if (description->refs > 0) {
            description->refs--;
        }
        if (description->refs != 0) {
            return 0;
        }
        if ((description->access & MUZIX_FS_ACCESS_READ) != 0 && pipe->readers > 0) {
            pipe->readers--;
        }
        if ((description->access & MUZIX_FS_ACCESS_WRITE) != 0 && pipe->writers > 0) {
            pipe->writers--;
        }
        description->used = 0;
        description->refs = 0;
        if (pipe->readers == 0 && pipe->writers == 0) {
            pipe->used = 0;
        }
        return 0;
    }
    if (description->kind == MUZIX_FS_OPEN_KIND_TTY) {
        if (description->refs > 0) {
            description->refs--;
        }
        if (description->refs == 0) {
            description->used = 0;
        }
        return 0;
    }
    if (description->refs > 0) {
        description->refs--;
    }
    if (description->refs == 0) {
        description->used = 0;
    }
    if (description->refs == 0) {
        muzix_fs_inode_t *entry =
            muzix_fs_inode_get(&fs->volume->inodes, inode);
        if (!entry) {
            return -1;
        }
        if (entry->links == 0) {
            flush_needed = 1;
        }
        if (muzix_fs_service_reclaim_inode(fs, inode) != 0) {
            return -1;
        }
    }
    /* Closing an unchanged read-only file must work on ROM-backed volumes. */
    return flush_needed ? muzix_fs_volume_flush(fs->volume) : 0;
}

int muzix_fs_service_readdir(muzix_fs_service_t *fs,
                             int handle,
                             uint16_t index,
                             muzix_fs_dirent_t *entry)
{
    muzix_fs_inode_t *inode;
    muzix_fs_directory_t directory;
    muzix_fs_open_description_t *file;
    uint16_t logical_index = 0;
    uint16_t slot;

    if (!fs || !entry || handle < 0 || handle >= MUZIX_FS_OPEN_FILES ||
        !fs->open_files[handle].used || !fs->volume ||
        !fs->volume->mounted) {
        return -1;
    }
    file = muzix_fs_service_description(fs, handle);
    if (!file || file->kind != MUZIX_FS_OPEN_KIND_INODE ||
        (file->access & MUZIX_FS_ACCESS_READ) == 0) {
        return -1;
    }
    /* An index past the last entry is the end of the directory, not an error:
     * returning -1 here made a caller that walked upward (ls, the kernel
     * shell) report a failure on the first index beyond the entry count, while
     * running off the end of a short directory returned 1.  Both are EOF. */
    if (index >= MUZIX_FS_DIR_ENTRIES) {
        return 1;
    }
    inode = muzix_fs_inode_get(&fs->volume->inodes, file->inode);
    if (!inode || (inode->mode & 0x4000u) == 0 || inode->zone[0] == 0 ||
        muzix_fs_directory_load(&fs->volume->cache, inode->zone[0], &directory) != 0) {
        return -1;
    }
    for (slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        if (directory.entries[slot].inode == 0) {
            continue;
        }
        if (logical_index == index) {
            entry->inode = directory.entries[slot].inode;
            memcpy(entry->name, directory.entries[slot].name,
                   MUZIX_FS_NAME_MAX);
            entry->name[MUZIX_FS_NAME_MAX] = '\0';
            return 0;
        }
        logical_index++;
    }
    return 1;
}

int muzix_fs_service_seek(muzix_fs_service_t *fs,
                          int handle,
                          uint32_t position)
{
    muzix_fs_open_description_t *file = muzix_fs_service_description(fs, handle);

    if (!file || file->kind != MUZIX_FS_OPEN_KIND_INODE) {
        return -1;
    }
    /* An unbounded position was accepted and then truncated to 16 bits when
     * the block index was computed: lseek(fd, 0x2000000) followed by a write
     * wrapped the index back to 0, overwrote zone[0] with a freshly allocated
     * zone and published a 512 MiB file size.  The inode layout addresses
     * MUZIX_FS_ZONE_SLOTS blocks, so anything past that is not a position this
     * filesystem can hold. */
    if (position > MUZIX_FS_MAX_FILE_SIZE) {
        return -1;
    }
    file->position = position;
    return 0;
}


int muzix_fs_service_unlink_path(muzix_fs_service_t *fs,
                                 const char *path)
{
    char normalized[MUZIX_FS_PATH_MAX];
    const char *leaf;
    muzix_fs_directory_t *parent_directory;
    muzix_fs_inode_t *entry;
    uint16_t parent_inode;
    uint16_t parent_zone;
    uint16_t inode;
    int status = -1;

    /* unlink was the one path operation that split and resolved its argument
     * by hand instead of normalising it first, so "." and ".." were passed to
     * the directory as ordinary names and `unlink ../file` never resolved. */
    if (!fs || !path || !fs->volume || !fs->volume->mounted ||
        muzix_fs_service_normalize_path(fs, path, normalized,
                                        sizeof(normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, normalized, &leaf,
                                       &parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }

    parent_directory = muzix_fs_service_dir_load(fs, parent_inode,
                                                 &parent_zone);
    if (!parent_directory ||
        muzix_fs_directory_lookup(parent_directory, leaf, &inode) != 0 ||
        inode == 1) {
        goto done;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    if (!entry || (entry->mode & 0x4000u) != 0 || entry->links == 0 ||
        muzix_fs_directory_remove(parent_directory, leaf) != 0) {
        goto done;
    }
    if (muzix_fs_service_dir_store(fs, parent_inode, parent_zone,
                                   parent_directory) != 0) {
        muzix_fs_service_dir_restore_entry(fs, parent_inode, leaf, inode);
        goto done;
    }
    entry->links--;
    if (muzix_fs_service_reclaim_inode(fs, inode) != 0) {
        goto done;
    }
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        goto done;
    }
    status = 0;

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}

int muzix_fs_service_link_path(muzix_fs_service_t *fs,
                               const char *old_path,
                               const char *new_path)
{
    char normalized[MUZIX_FS_PATH_MAX];
    const char *leaf;
    muzix_fs_directory_t *parent;
    muzix_fs_inode_t *source;
    uint16_t source_inode;
    uint16_t parent_inode;
    uint16_t parent_zone;
    int status = -1;

    if (!fs || !old_path || !new_path || !fs->volume ||
        !fs->volume->mounted ||
        muzix_fs_service_lookup_path(fs, old_path, &source_inode) != 0 ||
        muzix_fs_service_normalize_path(fs, new_path, normalized,
                                        sizeof(normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, normalized, &leaf,
                                       &parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }
    source = muzix_fs_inode_get(&fs->volume->inodes, source_inode);
    if (!source || (source->mode & 0x4000u) != 0 || source->links == 0 ||
        source->links == 0xffu) {
        goto done;
    }
    parent = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!parent || muzix_fs_directory_lookup(parent, leaf, 0) == 0 ||
        muzix_fs_directory_add(parent, source_inode, leaf) != 0) {
        goto done;
    }
    if (muzix_fs_service_dir_store(fs, parent_inode, parent_zone,
                                   parent) != 0) {
        muzix_fs_service_dir_drop_entry(fs, parent_inode, leaf);
        goto done;
    }
    source->links++;
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        source->links--;
        muzix_fs_service_dir_drop_entry(fs, parent_inode, leaf);
        goto done;
    }
    status = 0;

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}


int muzix_fs_service_rmdir_path(muzix_fs_service_t *fs,
                                const char *path)
{
    char normalized[MUZIX_FS_PATH_MAX];
    const char *leaf;
    muzix_fs_directory_t *parent;
    muzix_fs_directory_t *target;
    muzix_fs_inode_t *target_inode;
    muzix_fs_zone_allocator_t allocator_snapshot;
    muzix_fs_inode_t entry_snapshot;
    uint16_t parent_inode;
    uint16_t parent_zone;
    uint16_t target_zone;
    uint16_t inode;
    uint8_t used = 0;
    uint8_t description;
    size_t target_length;
    int status = -1;

    if (!fs || !path || !fs->volume || !fs->volume->mounted ||
        muzix_fs_service_normalize_path(fs, path, normalized,
                                        sizeof(normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, normalized, &leaf,
                                       &parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }
    target_length = strlen(normalized);
    if (strcmp(fs->cwd_path, normalized) == 0 ||
        (strncmp(fs->cwd_path, normalized, target_length) == 0 &&
         fs->cwd_path[target_length] == '/')) {
        goto done;
    }
    parent = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!parent || muzix_fs_directory_lookup(parent, leaf, &inode) != 0 ||
        inode == 1 || inode == fs->cwd_inode) {
        goto done;
    }
    for (description = 0; description < MUZIX_FS_OPEN_DESCRIPTIONS;
         description++) {
        if (fs->open_descriptions[description].used &&
            fs->open_descriptions[description].inode == inode) {
            goto done;
        }
    }
    /* Only one directory image is available, so the target is examined with a
     * load into the shared buffer and the parent image is reloaded after it. */
    target_inode = muzix_fs_inode_get(&fs->volume->inodes, inode);
    if (!target_inode) {
        goto done;
    }
    target = muzix_fs_service_dir_load(fs, inode, &target_zone);
    if (!target) {
        goto done;
    }
    for (uint8_t slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        if (target->entries[slot].inode != 0) {
            used++;
        }
    }
    if (used > 2) {
        goto done;
    }
    /* Snapshot what is about to be released.  The zone bitmap and the single
     * inode are all rmdir touches -- 68 bytes, where the old code copied the
     * whole 288-byte inode table and two more 256-byte directory images -- and
     * without a restore a failed store or flush left the directory entry gone
     * while the inode and its zone stayed allocated. */
    allocator_snapshot = fs->volume->allocator;
    entry_snapshot = *target_inode;

    parent = muzix_fs_service_dir_load(fs, parent_inode, &parent_zone);
    if (!parent || muzix_fs_directory_remove(parent, leaf) != 0 ||
        muzix_fs_service_dir_store(fs, parent_inode, parent_zone,
                                   parent) != 0) {
        muzix_fs_service_dir_restore_entry(fs, parent_inode, leaf, inode);
        goto done;
    }
    muzix_fs_zone_free(&fs->volume->allocator, target_zone);
    if (muzix_fs_inode_release(&fs->volume->inodes, inode) != 0) {
        goto rollback;
    }
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        goto rollback;
    }
    status = 0;
    goto done;

rollback:
    muzix_fs_service_dir_restore_entry(fs, parent_inode, leaf, inode);
    fs->volume->allocator = allocator_snapshot;
    /* Written through a pointer: SDCC rejects a dereference of a struct-array
     * element reached by a chain of -> selectors.  A failed release leaves the
     * entry untouched, so this is a no-op in that case. */
    target_inode = &fs->volume->inodes.entries[inode];
    *target_inode = entry_snapshot;

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}


/* Rename a directory across parents needs the moved directory's own image too
 * (its ".." entry has to follow).  Split out so that image is loaded, patched
 * and stored while the two parent images are not on the stack, and so the
 * caller only has to remember one uint16_t to undo it. */
static int muzix_fs_service_rename_relink(muzix_fs_service_t *fs,
                                          uint16_t inode,
                                          uint16_t new_parent_inode,
                                          uint16_t *old_parent_inode)
{
    muzix_fs_directory_t *target;
    uint16_t target_zone;
    uint16_t previous = 0;
    uint8_t found = 0;

    target = muzix_fs_service_dir_load(fs, inode, &target_zone);
    if (!target) {
        return -1;
    }
    for (uint8_t slot = 0; slot < MUZIX_FS_DIR_ENTRIES; slot++) {
        if (strcmp(target->entries[slot].name, "..") == 0) {
            previous = target->entries[slot].inode;
            target->entries[slot].inode = new_parent_inode;
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    if (muzix_fs_service_dir_store(fs, inode, target_zone, target) != 0) {
        return -1;
    }
    /* Optional: the undo path passes NULL because it already knows the old
     * value, and a caller that only wants the ".." repointed does not care. */
    if (old_parent_inode) {
        *old_parent_inode = previous;
    }
    return 0;
}

int muzix_fs_service_rename_path(muzix_fs_service_t *fs,
                                 const char *old_path,
                                 const char *new_path)
{
    /* Two 64-byte path buffers and pointers into them are all that stay here:
     * the function used to declare five 256-byte directory images (two parents,
     * three snapshots) plus four more path buffers, a 1988-byte frame against
     * a 1317-byte stack.  The images live in fs->scratch, one at a time, and
     * each step below either stores its image or undoes its own change, so no
     * snapshot has to be carried. */
    char old_normalized[MUZIX_FS_PATH_MAX];
    char new_normalized[MUZIX_FS_PATH_MAX];
    const char *old_leaf;
    const char *new_leaf;
    muzix_fs_directory_t *directory;
    muzix_fs_inode_t *entry;
    uint16_t old_parent_inode;
    uint16_t new_parent_inode;
    uint16_t old_parent_zone;
    uint16_t new_parent_zone;
    uint16_t inode = 0;
    uint16_t previous_parent = 0;
    size_t old_length;
    int linked = 0;
    int renamed = 0;
    int status = -1;

    if (!fs || !old_path || !new_path || !fs->volume ||
        !fs->volume->mounted ||
        muzix_fs_service_normalize_path(fs, old_path, old_normalized,
                                        sizeof(old_normalized)) != 0 ||
        muzix_fs_service_normalize_path(fs, new_path, new_normalized,
                                        sizeof(new_normalized)) != 0 ||
        muzix_fs_service_lookup_parent(fs, old_normalized, &old_leaf,
                                       &old_parent_inode) != 0 ||
        muzix_fs_service_lookup_parent(fs, new_normalized, &new_leaf,
                                       &new_parent_inode) != 0 ||
        muzix_fs_service_scratch_acquire(fs) != 0) {
        return -1;
    }
    if (muzix_fs_service_lookup_path(fs, old_normalized, &inode) != 0) {
        goto done;
    }
    if (strcmp(old_normalized, new_normalized) == 0) {
        status = 0;
        goto done;
    }
    /* Check up front that the current directory can follow the move.  The
     * rewrite itself happens after the rename is durable, so the 64-byte
     * staging buffer for it is not live across the operation. */
    if (muzix_fs_service_cwd_rewrite(fs, old_normalized, new_normalized) < 0) {
        goto done;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    old_length = strlen(old_normalized);
    if (entry && (entry->mode & 0x4000u) != 0 &&
        (strcmp(new_normalized, old_normalized) == 0 ||
         (strncmp(new_normalized, old_normalized, old_length) == 0 &&
          new_normalized[old_length] == '/'))) {
        /* A directory cannot be moved inside itself. */
        goto done;
    }

    /* Step 1: learn the inode number from the source directory. */
    directory = muzix_fs_service_dir_load(fs, old_parent_inode,
                                          &old_parent_zone);
    if (!directory || muzix_fs_directory_lookup(directory, old_leaf, &inode) != 0) {
        goto done;
    }

    /* Step 2: publish the entry in the destination directory. */
    directory = muzix_fs_service_dir_load(fs, new_parent_inode,
                                          &new_parent_zone);
    if (!directory || muzix_fs_directory_lookup(directory, new_leaf, 0) == 0 ||
        muzix_fs_directory_add(directory, inode, new_leaf) != 0) {
        goto done;
    }
    /* The image is modified from here on, so every failure past this point has
     * to take the entry back out again -- including a failed store, which
     * leaves the change sitting in the cache. */
    linked = 1;
    if (muzix_fs_service_dir_store(fs, new_parent_inode, new_parent_zone,
                                   directory) != 0) {
        goto rollback;
    }

    /* Step 3: take the entry out of the source directory.  When both parents
     * are the same block this reloads the image written just above, so the two
     * steps behave as one; when they differ, a failure here is undone by
     * dropping the entry from the destination again. */
    directory = muzix_fs_service_dir_load(fs, old_parent_inode,
                                          &old_parent_zone);
    if (!directory || muzix_fs_directory_remove(directory, old_leaf) != 0 ||
        muzix_fs_service_dir_store(fs, old_parent_inode, old_parent_zone,
                                   directory) != 0) {
        goto rollback;
    }
    renamed = 1;

    /* Step 4: repoint the moved directory's ".." when it changed parent. */
    if (old_parent_inode != new_parent_inode) {
        entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
        if (entry && (entry->mode & 0x4000u) != 0) {
            if (muzix_fs_service_rename_relink(fs, inode, new_parent_inode,
                                               &previous_parent) != 0) {
                goto rollback;
            }
            linked |= 2;
        }
    }
    if (muzix_fs_volume_flush(fs->volume) != 0) {
        goto rollback;
    }
    if (muzix_fs_service_cwd_rewrite(fs, old_normalized, new_normalized) == 0) {
        muzix_fs_service_apply_cwd(fs, old_normalized, new_normalized);
    }
    status = 0;
    goto done;

    /* Undo in reverse order, each step guarded by the flag that says its
     * change actually landed.  The old code instead carried two full directory
     * snapshots (512 bytes of frame) to restore from. */
rollback:
    if ((linked & 2u) != 0) {
        muzix_fs_service_rename_relink(fs, inode, previous_parent, 0);
    }
    if (linked) {
        muzix_fs_service_dir_drop_entry(fs, new_parent_inode, new_leaf);
    }
    if (renamed) {
        muzix_fs_service_dir_restore_entry(fs, old_parent_inode, old_leaf,
                                           inode);
    }

done:
    muzix_fs_service_scratch_release(fs);
    return status;
}


int muzix_fs_service_stat_path(muzix_fs_service_t *fs,
                               const char *path,
                               muzix_fs_inode_t *stat)
{
    uint16_t inode;
    muzix_fs_inode_t *entry;

    if (!stat || muzix_fs_service_lookup_path(fs, path, &inode) != 0) {
        return -1;
    }
    entry = muzix_fs_inode_get(fs->inodes, inode);
    if (!entry) {
        return -1;
    }
    *stat = *entry;
    return 0;
}

int muzix_fs_service_fstat(muzix_fs_service_t *fs,
                           int handle,
                           muzix_fs_inode_t *stat)
{
    muzix_fs_inode_t *entry;
    muzix_fs_open_description_t *file;

    file = muzix_fs_service_description(fs, handle);
    if (!file || file->kind != MUZIX_FS_OPEN_KIND_INODE || !stat ||
        !fs->volume || !fs->volume->mounted) {
        return -1;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, file->inode);
    if (!entry) {
        return -1;
    }
    *stat = *entry;
    return 0;
}

int muzix_fs_service_chmod_path(muzix_fs_service_t *fs,
                                const char *path,
                                uint16_t mode,
                                uint16_t uid)
{
    uint16_t inode;
    muzix_fs_inode_t *entry;

    if (!fs || !path || !fs->volume || !fs->volume->mounted ||
        muzix_fs_service_lookup_path(fs, path, &inode) != 0) {
        return -1;
    }
    entry = muzix_fs_inode_get(&fs->volume->inodes, inode);
    if (!entry || (uid != 0 && entry->uid != uid)) {
        return -1;
    }
    entry->mode = (uint16_t)((entry->mode & (uint16_t)~0777u) |
                             (mode & 0777u));
    return muzix_fs_volume_flush(fs->volume);
}

int muzix_fs_service_access_path(muzix_fs_service_t *fs,
                                 const char *path,
                                 uint16_t requested,
                                 uint16_t uid,
                                 uint16_t gid)
{
    uint16_t inode;
    uint16_t available;
    muzix_fs_inode_t *entry;

    if (!fs || !path || requested > 7u ||
        muzix_fs_service_lookup_path(fs, path, &inode) != 0) {
        return -1;
    }
    if (requested == 0) {
        return 0;
    }
    entry = muzix_fs_inode_get(fs->inodes, inode);
    if (!entry) {
        return -1;
    }
    if (uid == 0) {
        available = (uint16_t)(entry->mode & 0007u);
        if ((requested & MUZIX_FS_ACCESS_EXEC) != 0 &&
            (available & MUZIX_FS_ACCESS_EXEC) == 0 &&
            (entry->mode & 0700u) == 0) {
            return -1;
        }
        if ((requested & (MUZIX_FS_ACCESS_READ | MUZIX_FS_ACCESS_WRITE)) != 0) {
            return 0;
        }
        return (available & requested) == requested ? 0 : -1;
    }
    if (uid == entry->uid) {
        available = (uint16_t)((entry->mode >> 6) & 7u);
    } else if (gid == entry->gid) {
        available = (uint16_t)((entry->mode >> 3) & 7u);
    } else {
        available = (uint16_t)(entry->mode & 7u);
    }
    return (available & requested) == requested ? 0 : -1;
}

int muzix_fs_service_sync(muzix_fs_service_t *fs)
{
    if (!fs || !fs->volume || !fs->volume->mounted) {
        return -1;
    }
    return muzix_fs_volume_flush(fs->volume);
}

int muzix_fs_service_read(muzix_fs_service_t *fs,
                          int handle,
                          uint8_t *buffer,
                          size_t length,
                          size_t *result)
{
    muzix_fs_open_description_t *file;
    int status;

;

    file = muzix_fs_service_description(fs, handle);
    if (!file || (file->access & MUZIX_FS_ACCESS_READ) == 0) {
        return -1;
    }
;
    if (file->kind == MUZIX_FS_OPEN_KIND_PIPE) {
        muzix_fs_pipe_t *pipe = &fs->pipes[file->pipe];
        size_t amount = length;
        if (!pipe->used || !buffer || !result) {
            return -1;
        }
        if (pipe->count == 0) {
            *result = 0;
            return pipe->writers == 0 ? 0 : -1;
        }
        if (amount > pipe->count) {
            amount = pipe->count;
        }
        for (size_t index = 0; index < amount; index++) {
            buffer[index] = pipe->data[pipe->read_pos];
            pipe->read_pos = (uint16_t)((pipe->read_pos + 1) % MUZIX_FS_BLOCK_SIZE);
        }
        pipe->count = (uint16_t)(pipe->count - amount);
        *result = amount;
        return 0;
    }
    if (file->kind == MUZIX_FS_OPEN_KIND_TTY) {
        return muzix_tty_read(fs->tty, buffer, length, result);
    }
    if (file->kind != MUZIX_FS_OPEN_KIND_INODE) {
        return -1;
    }
;
    status = muzix_fs_service_read_inode(fs, file->inode, file->position,
                                         buffer, length, result);
    if (status == 0) {
        file->position += *result;
    }
    return status;
}

int muzix_fs_service_write(muzix_fs_service_t *fs,
                           int handle,
                           const uint8_t *buffer,
                           size_t length,
                           size_t *result)
{
    muzix_fs_open_description_t *file;
    int status;

    file = muzix_fs_service_description(fs, handle);
    if (!file || (file->access & MUZIX_FS_ACCESS_WRITE) == 0) {
        return -1;
    }
    if (file->kind == MUZIX_FS_OPEN_KIND_PIPE) {
        muzix_fs_pipe_t *pipe = &fs->pipes[file->pipe];
        size_t amount = length;
        size_t free_space;
        if (!pipe->used || !buffer || !result || pipe->readers == 0) {
            return -1;
        }
        free_space = MUZIX_FS_BLOCK_SIZE - pipe->count;
        if (free_space == 0) {
            return -1;
        }
        if (amount > free_space) {
            amount = free_space;
        }
        for (size_t index = 0; index < amount; index++) {
            pipe->data[pipe->write_pos] = buffer[index];
            pipe->write_pos = (uint16_t)((pipe->write_pos + 1) % MUZIX_FS_BLOCK_SIZE);
        }
        pipe->count = (uint16_t)(pipe->count + amount);
        *result = amount;
        return 0;
    }
    if (file->kind == MUZIX_FS_OPEN_KIND_TTY) {
        return muzix_tty_write(fs->tty, buffer, length, result);
    }
    if (file->kind != MUZIX_FS_OPEN_KIND_INODE) {
        return -1;
    }
    status = muzix_fs_service_write_inode(fs, file->inode, file->position,
                                          buffer, length, result);
    if (status == 0) {
        file->position += *result;
    }
    return status;
}

int muzix_fs_service_read_inode(muzix_fs_service_t *fs,
                                uint16_t inode,
                                uint32_t position,
                                uint8_t *buffer,
                                size_t length,
                                size_t *result)
{
    muzix_fs_inode_t *entry;

;

    if (!fs || !fs->cache || !fs->inodes || !fs->volume ||
        !fs->volume->mounted || !result) {
        return -1;
    }
    entry = muzix_fs_inode_get(fs->inodes, inode);
    if (!entry) {
        return -1;
    }
    int rc = muzix_fs_file_read(fs->cache, entry, position, buffer, length, result);
    return rc;
}

int muzix_fs_service_write_inode(muzix_fs_service_t *fs,
                                 uint16_t inode,
                                 uint32_t position,
                                 const uint8_t *buffer,
                                 size_t length,
                                 size_t *result)
{
    muzix_fs_inode_t *entry;

    if (!fs || !fs->cache || !fs->inodes || !fs->volume ||
        !fs->volume->mounted || !result) {
        return -1;
    }
    entry = muzix_fs_inode_get(fs->inodes, inode);
    if (!entry) {
        return -1;
    }
    return muzix_fs_file_write_alloc(fs->cache, &fs->volume->allocator,
                                     entry, position, buffer, length, result);
}


