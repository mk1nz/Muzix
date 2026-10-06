#include "superblock.h"

#include <string.h>
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/trace.h"

#define MUZIX_FS_SUPERBLOCK_BLOCK 1u

#if MUZIX_ENABLE_FORMAT
static void muzix_fs_put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

#endif /* MUZIX_ENABLE_FORMAT */

static uint16_t muzix_fs_get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

#if MUZIX_ENABLE_FORMAT
static void muzix_fs_put32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

#endif /* MUZIX_ENABLE_FORMAT */

static uint32_t muzix_fs_get32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

void muzix_fs_superblock_init(muzix_fs_superblock_state_t *state,
                              muzix_fs_block_cache_t *cache)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->cache = cache;
}

#if MUZIX_ENABLE_FORMAT
int muzix_fs_superblock_format(muzix_fs_superblock_state_t *state,
                               uint16_t inode_count,
                               uint16_t zone_count,
                               uint16_t first_data_zone)
{
    uint8_t *data;

    if (!state || !state->cache || inode_count == 0 || zone_count == 0 ||
        first_data_zone >= zone_count ||
        muzix_fs_cache_get(state->cache, MUZIX_FS_SUPERBLOCK_BLOCK, &data) != 0) {
        return -1;
    }

    memset(data, 0, MUZIX_FS_BLOCK_SIZE);
    state->value.ninodes = inode_count;
    state->value.nzones = zone_count;
    state->value.imap_blocks = 1;
    state->value.zmap_blocks = 1;
    state->value.first_data_zone = first_data_zone;
    state->value.log_zone_size = 0;
    state->value.max_size = 0xffffffffUL;
    state->value.magic = MUZIX_FS_MAGIC;

    muzix_fs_put16(&data[0], state->value.ninodes);
    muzix_fs_put16(&data[2], state->value.nzones);
    data[4] = state->value.imap_blocks;
    data[5] = state->value.zmap_blocks;
    muzix_fs_put16(&data[6], state->value.first_data_zone);
    data[8] = state->value.log_zone_size;
    muzix_fs_put32(&data[9], state->value.max_size);
    muzix_fs_put16(&data[13], state->value.magic);
    if (muzix_fs_cache_mark_dirty(state->cache, MUZIX_FS_SUPERBLOCK_BLOCK) != 0) {
        return -1;
    }
    state->loaded = 1;
    return 0;
}
#endif /* MUZIX_ENABLE_FORMAT */

int muzix_fs_superblock_load(muzix_fs_superblock_state_t *state)
{
    uint8_t *data;

    if (!state || !state->cache) {
        return -1;
    }

    if (muzix_fs_cache_get(state->cache, MUZIX_FS_SUPERBLOCK_BLOCK, &data) != 0) {
        return -1;
    }

    state->value.ninodes = muzix_fs_get16(&data[0]);
    state->value.nzones = muzix_fs_get16(&data[2]);
    state->value.imap_blocks = data[4];
    state->value.zmap_blocks = data[5];
    state->value.first_data_zone = muzix_fs_get16(&data[6]);
    state->value.log_zone_size = data[8];
    state->value.max_size = muzix_fs_get32(&data[9]);
    state->value.magic = muzix_fs_get16(&data[13]);
    state->loaded = 1;
    return muzix_fs_superblock_valid(state) ? 0 : -1;
}

int muzix_fs_superblock_valid(const muzix_fs_superblock_state_t *state)
{
    if (!state || !state->loaded || state->value.magic != MUZIX_FS_MAGIC ||
        state->value.ninodes == 0 || state->value.nzones == 0 ||
        state->value.first_data_zone >= state->value.nzones) {
        return 0;
    }
    return 1;
}
