#include "allocator.h"

#include <string.h>

static void muzix_fs_zone_mark(muzix_fs_zone_allocator_t *allocator,
                               uint16_t zone,
                               uint8_t used)
{
     uint16_t byte = zone >> 3;
    uint8_t mask = (uint8_t)(1u << (zone & 7u));

    if (used) {
        allocator->used[byte] |= mask;
    } else {
        allocator->used[byte] &= (uint8_t)~mask;
    }
}

void muzix_fs_zone_allocator_init(muzix_fs_zone_allocator_t *allocator,
                                  uint16_t zone_count,
                                  uint16_t first_data_zone)
{
    if (!allocator) {
        return;
    }

    memset(allocator, 0, sizeof(*allocator));
    if (zone_count > MUZIX_FS_MAX_ZONES) {
        zone_count = MUZIX_FS_MAX_ZONES;
    }
    allocator->zone_count = zone_count;
    allocator->first_data_zone = first_data_zone;
}

int muzix_fs_zone_alloc(muzix_fs_zone_allocator_t *allocator,
                        uint16_t *zone)
{
    uint16_t candidate;

    if (!allocator || !zone || allocator->first_data_zone >= allocator->zone_count) {
        return -1;
    }

    for (candidate = allocator->first_data_zone;
         candidate < allocator->zone_count; candidate++) {
        if (!muzix_fs_zone_is_used(allocator, candidate)) {
            muzix_fs_zone_mark(allocator, candidate, 1);
            *zone = candidate;
            return 0;
        }
    }
    return -1;
}

int muzix_fs_zone_free(muzix_fs_zone_allocator_t *allocator,
                       uint16_t zone)
{
    if (!allocator || zone < allocator->first_data_zone ||
        zone >= allocator->zone_count ||
        !muzix_fs_zone_is_used(allocator, zone)) {
        return -1;
    }

    muzix_fs_zone_mark(allocator, zone, 0);
    return 0;
}

int muzix_fs_zone_is_used(const muzix_fs_zone_allocator_t *allocator,
                          uint16_t zone)
{
    if (!allocator || zone >= allocator->zone_count ||
        zone >= MUZIX_FS_MAX_ZONES) {
        return 0;
    }

     return (allocator->used[zone >> 3] & (uint8_t)(1u << (zone & 7u))) != 0;
}
