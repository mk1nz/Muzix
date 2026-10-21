#include "directory.h"

static int test_directory(void)
{
    muzix_fs_directory_t directory;
    uint16_t inode;

    muzix_fs_directory_init(&directory);
    if (muzix_fs_directory_add(&directory, 2, ".") != 0 ||
        muzix_fs_directory_add(&directory, 2, ".") == 0) {
        return 1;
    }
    if (muzix_fs_directory_add(&directory, 3, "etc") != 0 ||
        muzix_fs_directory_lookup(&directory, "etc", &inode) != 0 || inode != 3) {
        return 2;
    }
    if (muzix_fs_directory_remove(&directory, "etc") != 0 ||
        muzix_fs_directory_lookup(&directory, "etc", &inode) == 0) {
        return 3;
    }
    if (muzix_fs_directory_add(&directory, 4, "123456789012345") == 0) {
        return 4;
    }
    return 0;
}

int main(void)
{
    return test_directory();
}
