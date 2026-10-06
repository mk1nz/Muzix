#!/bin/sh
# Construct a UNA BIOS disk image from a 512-byte stage-1 and a flat stage-2.
# The payload is wrapped in the fixed MUZ2 stage-2 ABI header at LBA 2.
# Usage: make_una_image.sh BOOT.bin PAYLOAD.bin OUTPUT.img [FILESYSTEM.img]

set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 BOOT.bin PAYLOAD.bin OUTPUT.img [FILESYSTEM.img]" >&2
    exit 2
fi

boot=$1
payload=$2
output=$3
filesystem=${4-}
sector_size=512
payload_lba=2
filesystem_lba=2048
max_payload_sectors=124
header_size=16
max_payload_bytes=$((sector_size * max_payload_sectors - header_size))

[ "$(wc -c < "$boot")" -eq "$sector_size" ] || {
    echo "boot sector must be exactly $sector_size bytes" >&2
    exit 1
}
[ -f "$payload" ] || {
    echo "payload not found: $payload" >&2
    exit 1
}
payload_bytes=$(wc -c < "$payload")
[ "$payload_bytes" -gt 0 ] || {
    echo "stage-2 payload must not be empty" >&2
    exit 1
}
[ "$payload_bytes" -le "$max_payload_bytes" ] || {
    echo "payload exceeds ${max_payload_bytes}-byte stage-1 limit" >&2
    exit 1
}
[ -z "$filesystem" ] || [ -f "$filesystem" ] || {
    echo "filesystem not found: $filesystem" >&2
    exit 1
}

payload_sectors=$(((payload_bytes + header_size + sector_size - 1) / sector_size))
[ "$payload_sectors" -le "$max_payload_sectors" ] || {
    echo "payload needs too many sectors" >&2
    exit 1
}

# Reserve the pre-filesystem gap even without a filesystem, so a later
# filesystem write cannot overlap stage-2.
dd if=/dev/zero of="$output" bs=$sector_size count=$filesystem_lba status=none
dd if="$boot" of="$output" bs=$sector_size count=1 conv=notrunc status=none

# Header: magic, ABI version, header size, payload byte length, total sectors,
# reserved byte, then a little-endian entry offset of 16 bytes.
payload_lo=$((payload_bytes & 255))
payload_hi=$((payload_bytes >> 8))
{
    printf '\115\125\132\062\001\020'
    printf '%b' "$(printf '\\%03o\\%03o\\%03o\\000\\020\\000\\000\\000\\000\\000' \
        "$payload_lo" "$payload_hi" "$payload_sectors")"
} | dd of="$output" bs=1 seek=$((payload_lba * sector_size)) count=$header_size conv=notrunc status=none
dd if="$payload" of="$output" bs=1 seek=$((payload_lba * sector_size + header_size)) conv=notrunc status=none

if [ -n "$filesystem" ]; then
    dd if="$filesystem" of="$output" bs=$sector_size seek=$filesystem_lba \
        conv=notrunc status=none
fi

echo "Created $output (MUZ2 stage-2 at LBA $payload_lba; filesystem at LBA $filesystem_lba)"
