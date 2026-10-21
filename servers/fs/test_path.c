#include "directory.h"
#include "path.h"

static muzix_fs_directory_t root;
static muzix_fs_directory_t etc;

static int lookup_component(void *context, uint16_t directory_inode,
                            const char *name, uint16_t *inode)
{
    (void)context;
    if (directory_inode == 1) {
        return muzix_fs_directory_lookup(&root, name, inode);
    }
    if (directory_inode == 2) {
        return muzix_fs_directory_lookup(&etc, name, inode);
    }
    return -1;
}

static int test_path(void)
{
    uint16_t inode;

    muzix_fs_directory_init(&root);
    muzix_fs_directory_init(&etc);
    muzix_fs_directory_add(&root, 2, "etc");
    muzix_fs_directory_add(&etc, 3, "motd");

    if (muzix_fs_path_lookup(0, 1, "/etc/motd", lookup_component, &inode) != 0 ||
        inode != 3) {
        return 1;
    }
    if (muzix_fs_path_lookup(0, 1, "/etc/missing", lookup_component, &inode) == 0) {
        return 2;
    }
    return 0;
}

int main(void)
{
    return test_path();
}
