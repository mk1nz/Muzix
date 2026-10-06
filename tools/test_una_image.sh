#!/bin/sh
# Regression checks for the UNA stage-1/stage-2 raw-image ABI.

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 BOOT.bin IMAGE_BUILDER" >&2
    exit 2
fi

boot=$1
builder=$2
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/muzix-una.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

dd if=/dev/zero of="$tmpdir/payload.bin" bs=1 count=1000 status=none
sh "$builder" "$boot" "$tmpdir/payload.bin" "$tmpdir/image.img" >/dev/null

ruby - "$boot" "$tmpdir/payload.bin" "$tmpdir/image.img" <<'RUBY'
boot, payload, image = ARGV.map { |path| File.binread(path) }
abort "boot sector size" unless boot.bytesize == 512
abort "boot signature" unless boot.byteslice(510, 2).bytes == [0x55, 0xaa]
abort "image pre-filesystem size" unless image.bytesize == 2048 * 512
abort "boot placement" unless image.byteslice(0, 512) == boot
header = image.byteslice(2 * 512, 16)
abort "stage-2 magic" unless header.byteslice(0, 4) == "MUZ2"
abort "stage-2 ABI" unless header.byteslice(4, 2).bytes == [1, 16]
abort "stage-2 length" unless header.byteslice(6, 2).unpack1("v") == payload.bytesize
abort "stage-2 sector count" unless header.getbyte(8) == 2
abort "stage-2 entry" unless header.byteslice(10, 2).unpack1("v") == 16
abort "payload placement" unless image.byteslice(2 * 512 + 16, payload.bytesize) == payload
RUBY

dd if=/dev/zero of="$tmpdir/too-large.bin" bs=1 count=63473 status=none
if sh "$builder" "$boot" "$tmpdir/too-large.bin" "$tmpdir/oversize.img" >/dev/null 2>&1; then
    echo "oversize stage-2 was accepted" >&2
    exit 1
fi

echo "UNA raw image ABI: PASS"
