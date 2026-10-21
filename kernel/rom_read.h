#ifndef MUZIX_ROM_READ_H
#define MUZIX_ROM_READ_H

#include <stdint.h>
#include "../fs/block_device.h"

/* Forward declaration of MM service struct (defined in mm/mm_service.h) */
struct muzix_mm_service;

/* The MINIX image occupies the ROM from here to the end of the 512 KiB part
 * (tools/make_zeta_rom.sh lays the kernel in 0x00000-0x1FFFF and the image
 * from 0x20000).  The superblock lives at image block 1, so image block 0 is
 * the 0x20200 magic at ROM offset 0x2020D.
 *
 * This was previously a private #define in rom_read.c, duplicated nowhere but
 * implied by a hand-written block count in kernel_main.c. */
#define MUZIX_ROMFS_BASE_OFFSET  0x20000u
#define MUZIX_ROMFS_TOTAL_BYTES  (512u * 1024u - MUZIX_ROMFS_BASE_OFFSET)
#define MUZIX_ROMFS_BLOCK_COUNT  (768u)

int rom_read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer);

/* Window-0 helper for boot-time kernel-to-user copy (see rom_read.c).
 * dst_map is the destination process map (opaque pointer to process_map_t). */
int muzix_boot_copy_kernel_to_user(struct muzix_mm_service *mm,
                                     const void *dst_map,
                                     uint16_t user_addr,
                                     const uint8_t *src,
                                     size_t len);

/* The kernel-side ROM filesystem interceptors (see rom_read.c for why the
 * FS server cannot answer these itself).  A synthetic descriptor returned by
 * muzix_rom_open_path() is answered by muzix_rom_readdir() with the same
 * 0/1/-1 the fs/ service layer's readdir gives; muzix_rom_fd_is_synthetic()
 * is the range test the syscall layer uses to keep everything else routed. */
int muzix_rom_open_path(const char *path);
int muzix_rom_readdir(int fd, uint16_t index, uint8_t *entry_out);
int muzix_rom_fd_is_synthetic(int fd);
uint16_t muzix_rom_resolve_inode(const char *path);
int muzix_rom_lookup_inode(const char *path, uint8_t *inode_out);
int muzix_rom_fstat(int fd, uint8_t *inode_out);
/* The read side of the same synthetic descriptors.  muzix_rom_read() fills a
 * KERNEL buffer - the copy out to the caller's is the syscall layer's. */
int muzix_rom_seek(int fd, uint32_t offset, int whence);
int muzix_rom_read(int fd, uint8_t *buffer, uint16_t count);

#endif