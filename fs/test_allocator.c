#include "allocator.h"

static int test_allocator(void)
{
    muzix_fs_zone_allocator_t allocator;
    uint16_t first;
    uint16_t second;

    muzix_fs_zone_allocator_init(&allocator, 16, 4);
    if (muzix_fs_zone_alloc(&allocator, &first) != 0 || first != 4 ||
        muzix_fs_zone_alloc(&allocator, &second) != 0 || second != 5 ||
        !muzix_fs_zone_is_used(&allocator, first)) {
        return 1;
    }
    if (muzix_fs_zone_free(&allocator, first) != 0 ||
        muzix_fs_zone_alloc(&allocator, &first) != 0 || first != 4 ||
        muzix_fs_zone_free(&allocator, 2) == 0) {
        return 2;
    }
    return 0;
}

int main(void)
{
    return test_allocator();
}
