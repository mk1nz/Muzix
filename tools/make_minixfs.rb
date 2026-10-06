#!/usr/bin/env ruby
# Generate a MINIX v1-compatible filesystem image for Muzix.
# Layout: block 0 = reserved, block 1 = superblock,
# block 2 = zone allocator, block 3 = inode table,
# block 4 = root directory, blocks 5+ = file data.
#
# Usage: make_minixfs.rb OUTPUT.bin FILE1 NAME1 FILE2 NAME2 ...

abort "usage: #{$PROGRAM_NAME} OUTPUT.bin FILE NAME ..." if ARGV.length < 3

output = ARGV.shift
files = ARGV.each_slice(2).to_a
abort "files must come in FILE NAME pairs" if files.any? { |f| f.length != 2 }

BLOCK_SIZE = 512
# Must match MUZIX_FS_INODE_SLOTS in fs/inode_table.h. Slot 0 is never handed
# out while slot 1 is the root, so inode capacity must include those two slots.
# The kernel has its own matching ceiling: each slot is a 32-byte record inside
# g_volume, the 512-byte inode block would hold sixteen, and the stack-headroom
# gate is what stops short of it - see MUZIX_FS_INODE_SLOTS for the measurement.
MAX_INODES = 14
MAX_ZONES = 256
FIRST_DATA_ZONE = 5
MAGIC = 0x137f
INODE_DISK_SIZE = 32
DIR_ENTRY_SIZE = 16
ROOT_INODE = 1
ROOT_DIR_BLOCK = 4

# Must match MUZIX_FS_DIRECT_ZONES in fs/fs_types.h: the kernel reads exactly
# this many direct zone pointers from the inode and then follows indirect_zone.
# A file larger than DIRECT_ZONES * 512 bytes needs a real indirect block.
DIRECT_ZONES = 7
ZONES_PER_INDIRECT_BLOCK = BLOCK_SIZE / 2

# The inode table supports twelve files in addition to reserved slot 0 and root
# inode 1. The current ROM image uses twelve file entries.
MAX_FILES = MAX_INODES - 2
abort "too many files (max #{MAX_FILES})" if files.length > MAX_FILES

total_blocks = FIRST_DATA_ZONE
data_blocks = []

files.each do |path, name|
  abort "name too long (max 14)" if name.bytesize > 14
  abort "file not found: #{path}" unless File.file?(path)
  data = File.binread(path)
  blocks_needed = (data.bytesize + BLOCK_SIZE - 1) / BLOCK_SIZE
  blocks_needed = 1 if blocks_needed == 0

  direct = blocks_needed < DIRECT_ZONES ? blocks_needed : DIRECT_ZONES
  indirect_count = blocks_needed - direct
  if indirect_count > ZONES_PER_INDIRECT_BLOCK
    abort "#{name}: too large (#{blocks_needed} blocks, max #{DIRECT_ZONES + ZONES_PER_INDIRECT_BLOCK})"
  end

  entry = { name: name, data: data, blocks: blocks_needed, start_block: total_blocks,
            direct: direct, indirect_count: indirect_count, indirect_block: 0 }
  total_blocks += blocks_needed
  if indirect_count > 0
    # One extra block holds the remaining zone numbers, 16 bits each.
    entry[:indirect_block] = total_blocks
    total_blocks += 1
  end
  data_blocks << entry
end

abort "filesystem exceeds 65535 blocks" if total_blocks > MAX_ZONES
zone_count = total_blocks

image = Array.new(total_blocks * BLOCK_SIZE, 0)

def write16(buf, offset, val)
  buf[offset] = val & 0xff
  buf[offset + 1] = (val >> 8) & 0xff
end

def write32(buf, offset, val)
  buf[offset] = val & 0xff
  buf[offset + 1] = (val >> 8) & 0xff
  buf[offset + 2] = (val >> 16) & 0xff
  buf[offset + 3] = (val >> 24) & 0xff
end

# Block 1: superblock
sb = 1 * BLOCK_SIZE
write16(image, sb + 0, MAX_INODES)
write16(image, sb + 2, zone_count)
image[sb + 4] = 1
image[sb + 5] = 1
write16(image, sb + 6, FIRST_DATA_ZONE)
image[sb + 8] = 0
write32(image, sb + 9, 0xffffffff)
write16(image, sb + 13, MAGIC)

# Block 2: zone allocator
alloc = 2 * BLOCK_SIZE
write16(image, alloc + 0, FIRST_DATA_ZONE)
write16(image, alloc + 2, zone_count)
# Blocks 0..FIRST_DATA_ZONE-1 hold the metadata (reserved, superblock, zone
# allocator, inode table, root directory) and can never be file data.  They used
# to be left clear, so the kernel's muzix_fs_zone_is_used(4) reported the root
# directory as free; nothing broke only because muzix_fs_zone_alloc starts at
# first_data_zone.  fs/allocator_store.c now rejects a bitmap that does not
# reserve them, so the image has to agree.
(0...FIRST_DATA_ZONE).each do |zone|
  image[alloc + 4 + zone / 8] |= (1 << (zone % 8))
end
data_blocks.each do |fb|
  fb[:blocks].times do |i|
    zone = fb[:start_block] + i
    byte_idx = zone / 8
    bit = zone % 8
    image[alloc + 4 + byte_idx] |= (1 << bit)
  end
  next if fb[:indirect_count] == 0
  zone = fb[:indirect_block]
  image[alloc + 4 + zone / 8] |= (1 << (zone % 8))
end

# Block 3: inode table
inode_table = 3 * BLOCK_SIZE
image.fill(0, inode_table, BLOCK_SIZE)

inode_num = 2
data_blocks.each do |fb|
  offset = inode_table + inode_num * INODE_DISK_SIZE
  write16(image, offset + 0, 0x8000 | 0777)
  write32(image, offset + 4, fb[:data].bytesize)
  image[offset + 10] = 1
  fb[:direct].times do |i|
    write16(image, offset + 11 + i * 2, fb[:start_block] + i)
  end
  # Remaining zone numbers go in the indirect block; the inode just points at it.
  if fb[:indirect_count] > 0
    ind = fb[:indirect_block] * BLOCK_SIZE
    fb[:indirect_count].times do |i|
      write16(image, ind + i * 2, fb[:start_block] + fb[:direct] + i)
    end
    write16(image, offset + 25, fb[:indirect_block])
  else
    write16(image, offset + 25, 0)
  end
  image[offset + 27] = 1
  inode_num += 1
end

# Root inode (inode 1)
root_off = inode_table + ROOT_INODE * INODE_DISK_SIZE
write16(image, root_off + 0, 0x4000)
image[root_off + 10] = 2
write16(image, root_off + 11, ROOT_DIR_BLOCK)
image[root_off + 27] = 1

# Block 4: root directory
dir = 4 * BLOCK_SIZE
write16(image, dir + 0, ROOT_INODE)
".".ljust(14, "\0").bytes.each_with_index { |b, i| image[dir + 2 + i] = b }
write16(image, dir + DIR_ENTRY_SIZE, ROOT_INODE)
"..".ljust(14, "\0").bytes.each_with_index { |b, i| image[dir + DIR_ENTRY_SIZE + 2 + i] = b }

data_blocks.each_with_index do |fb, idx|
  entry_off = dir + (idx + 2) * DIR_ENTRY_SIZE
  write16(image, entry_off, idx + 2)
  fb[:name].ljust(14, "\0").bytes.each_with_index { |b, i| image[entry_off + 2 + i] = b }
end

# Data blocks
data_blocks.each do |fb|
  data = fb[:data]
  fb[:blocks].times do |i|
    block_idx = fb[:start_block] + i
    chunk = data.bytes[i * BLOCK_SIZE, BLOCK_SIZE] || []
    chunk.each_with_index { |byte, j| image[block_idx * BLOCK_SIZE + j] = byte }
  end
end

File.binwrite(output, image.pack("C*"))
puts "Created #{output} (#{total_blocks} blocks, #{total_blocks * BLOCK_SIZE} bytes)"
