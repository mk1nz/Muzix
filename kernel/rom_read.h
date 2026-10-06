#ifndef MUZIX_ROM_READ_H
#define MUZIX_ROM_READ_H

#include <stdint.h>
#include "../fs/block_device.h"

/* The MINIX image occupies the ROM from here to the end of the 512 KiB part
 * (tools/make_zeta_rom.sh lays the kernel in 0x00000-0x1FFFF and the image
 * from 0x20000).  The superblock lives at image block 1, so image block 0 is
 * the 0x20200 magic at ROM offset 0x2020D.
 *
 * This was previously a private #define in rom_read.c, duplicated nowhere but
 * implied by a hand-written block count in kernel_main.c. */
#define MUZIX_ROMFS_BASE_OFFSET  0x20000u
#define MUZIX_ROMFS_TOTAL_BYTES  (512u * 1024u - MUZIX_ROMFS_BASE_OFFSET)
#define MUZIX_ROMFS_BLOCK_COUNT  (MUZIX_ROMFS_TOTAL_BYTES / MUZIX_FS_BLOCK_SIZE)

int rom_read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer);

#endif