#include <stdint.h>

#include "../platform/zeta-v2/test_kernel/bank_model.h"
#include "../fs/block_device.h"
#include "../mm/mm_service.h"
#include "../platform/zeta-v2/uart_io.h"
#include "rom_read.h"

/*
 * ROMFS layout in the Zeta V2 512 KiB Flash ROM:
 *   0x00000-0x00FFF : boot stub + stage-2 kernel (copied to RAM by rom_boot.s)
 *   0x010000-...    : ROMFS (device->block_count blocks of 512 bytes)
 *
 * The ROMFS region is only reachable through banked memory.  Window 1
 * The bank model temporarily maps window 1 to the ROM page, copies out the
 * requested bytes, then restores the complete caller map.  Drivers never
 * manipulate bank registers directly.
 *
 * Context is preserved through the bank switching helpers so the caller's
 * bank registers are never corrupted.
 */

/* The MINIX-v1 filesystem image region is only reachable through banked memory.  Window 1
 * The bank model temporarily maps window 1 to the ROM page, copies out the
 * requested bytes, then restores the complete caller map.  Drivers never
 * manipulate bank registers directly.
 *
 * MUZIX_ROMFS_BASE_OFFSET and MUZIX_ROMFS_BLOCK_COUNT now live in rom_read.h so
 * the mount's block count and the read offset cannot drift apart again.
 *
 * Context is preserved through the bank switching helpers so the caller's
 * bank registers are never corrupted.
 */

#define MUZIX_ROMFS_WINDOW_ADDR 0x4000u
#define MUZIX_ROMFS_BANK_SIZE   0x4000u

int rom_read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer)
{
    uint32_t rom_offset;
    uint8_t page;
    uint16_t offset;
    muzix_mm_service_t *mm;

    if (!device || !buffer) {
        return -1;
    }

    /* The block count is the device's, not a constant: hardcoding 66 here
     * silently limited a differently sized ROMFS image while claiming to read
     * every block the volume described, so a read past the real end returned
     * image bytes from beyond the filesystem. */
    if (device->block_count == 0 || block >= device->block_count) {
        return -1;
    }

    mm = device->context ? (muzix_mm_service_t *)device->context : 0;

    rom_offset = (uint32_t)MUZIX_ROMFS_BASE_OFFSET
               + (uint32_t)block * (uint32_t)MUZIX_FS_BLOCK_SIZE;
    page = (uint8_t)(rom_offset / MUZIX_ROMFS_BANK_SIZE);
    offset = (uint16_t)(rom_offset % MUZIX_ROMFS_BANK_SIZE);

    if (!mm) {
        return -1;
    }
    int result = muzix_mm_copy_rom_page_to_kernel(mm, page,
                                                  offset, buffer,
                                                  MUZIX_FS_BLOCK_SIZE);
    if (result != 0) {
        return -1;
    }
    return 0;
}
