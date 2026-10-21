#ifndef MUZIX_FS_TYPES_H
#define MUZIX_FS_TYPES_H

#include <stdint.h>

#define MUZIX_FS_MAGIC 0x137fu
#define MUZIX_FS_MAX_INODES 32u
#define MUZIX_FS_DIRECT_ZONES 7u
#define MUZIX_FS_MODE_CHAR 0x2000u
#define MUZIX_FS_MODE_DIR 0x4000u
#define MUZIX_FS_MODE_BLOCK 0x6000u
#define MUZIX_FS_MODE_REGULAR 0x8000u
#define MUZIX_FS_MODE_TYPE 0xf000u
#define MUZIX_FS_MODE_SETUID 04000u
#define MUZIX_FS_MODE_SETGID 02000u
#define MUZIX_FS_MODE_PERMS 07777u
#define MUZIX_FS_TTY_DEVICE 0x0100u

typedef struct {
    uint16_t ninodes;
    uint16_t nzones;
    uint8_t imap_blocks;
    uint8_t zmap_blocks;
    uint16_t first_data_zone;
    uint8_t log_zone_size;
    uint32_t max_size;
    uint16_t magic;
} muzix_fs_superblock_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint16_t gid;
    uint8_t links;
    uint16_t zone[MUZIX_FS_DIRECT_ZONES];
    uint16_t indirect_zone;
    uint8_t used;
    uint8_t pad[3];
} muzix_fs_inode_t;

#endif
