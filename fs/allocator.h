#ifndef MUZIX_FS_ALLOCATOR_H
#define MUZIX_FS_ALLOCATOR_H

#include <stdint.h>

#define MUZIX_FS_MAX_ZONES 256u

typedef struct {
    uint16_t first_data_zone;
    uint16_t zone_count;
    uint8_t used[MUZIX_FS_MAX_ZONES / 8u];
} muzix_fs_zone_allocator_t;

void muzix_fs_zone_allocator_init(muzix_fs_zone_allocator_t *allocator,
                                  uint16_t zone_count,
                                  uint16_t first_data_zone);
int muzix_fs_zone_alloc(muzix_fs_zone_allocator_t *allocator,
                        uint16_t *zone);
int muzix_fs_zone_free(muzix_fs_zone_allocator_t *allocator,
                       uint16_t zone);
int muzix_fs_zone_is_used(const muzix_fs_zone_allocator_t *allocator,
                          uint16_t zone);

#endif
