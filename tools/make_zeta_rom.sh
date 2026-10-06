#!/bin/sh
# Assemble the fixed Zeta V2 512 KiB Flash ROM layout.
# Usage: make_zeta_rom.sh BOOT.bin STAGE2.bin ROOTFS.bin OUTPUT.rom

set -eu
[ "$#" -eq 4 ] || { echo "usage: $0 BOOT.bin STAGE2.bin ROOTFS.bin OUTPUT.rom" >&2; exit 2; }

boot=$1 stage2=$2 rootfs=$3 output=$4
rom_size=$((512 * 1024))
# The kernel gets 128 KiB of the 512 KiB device. The Z80 addresses 64 KiB
# at a time and the boot stub still copies only pages 0-3, so the kernel is
# not using the upper 64 KiB yet - it is reserved so the image is not boxed
# in, and so the MM has somewhere to page from once context switching lands.
kernel_size=$((128 * 1024))
stage2_offset=152
rootfs_size=$((rom_size - kernel_size))

[ -f "$boot" ] && [ -f "$stage2" ] && [ -f "$rootfs" ] || { echo "ROM input missing" >&2; exit 1; }
[ "$(wc -c < "$boot")" -le "$stage2_offset" ] || { echo "ROM boot stub exceeds 0x98" >&2; exit 1; }
[ $(( $(wc -c < "$stage2") + stage2_offset )) -le "$kernel_size" ] || { echo "stage-2 exceeds the 128 KiB kernel ROM region" >&2; exit 1; }
[ "$(wc -c < "$rootfs")" -le "$rootfs_size" ] || { echo "ROMFS exceeds the 384 KiB ROM region" >&2; exit 1; }

dd if=/dev/zero of="$output" bs=1 count=$rom_size status=none
dd if="$boot" of="$output" bs=1 seek=0 conv=notrunc status=none
dd if="$stage2" of="$output" bs=1 seek=$stage2_offset conv=notrunc status=none
dd if="$rootfs" of="$output" bs=1 seek=$kernel_size conv=notrunc status=none
echo "Created $output (kernel ROM 128 KiB; ROMFS 384 KiB)"
