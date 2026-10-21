#!/usr/bin/env ruby
# Generate a MINIX v1-compatible filesystem image for Muzix.
# Layout: block 0 = reserved, block 1 = superblock,
# block 2 = zone allocator, blocks 3-4 = inode table (2 blocks),
# block 5 = root directory, blocks 6+ = file data.
#
# Usage: make_minixfs.rb OUTPUT.bin FILE1 NAME1 FILE2 NAME2 ...
#
# NAMES support a /path/name form. Directories are created automatically:
# make_minixfs.rb out.bin myapp /bin/myapp doc /bin/doc
# creates /bin if needed and places the files inside it.

abort "usage: #{$PROGRAM_NAME} OUTPUT.bin FILE NAME ..." if ARGV.length < 3

output = ARGV.shift
files = ARGV.each_slice(2).to_a
abort "files must come in FILE NAME pairs" if files.any? { |f| f.length != 2 }

build_time = Time.now.to_i
$build_time = build_time

BLOCK_SIZE = 512
# Must match MUZIX_FS_INODE_SLOTS in fs/inode_table.h. Slot 0 is never handed
# out while slot 1 is the root, so inode capacity must include those two slots.
# The kernel has its own matching ceiling: each slot is a 32-byte record inside
# g_volume, the 512-byte inode block would hold sixteen, and the stack-headroom
# gate is what stops short of it - see MUZIX_FS_INODE_SLOTS for the measurement.
MAX_INODES = 32
# 384 KiB of ROMFS, which is what the device actually has: the kernel configures
# its ROM block device for 768 blocks (kernel/rom_read.h MUZIX_ROMFS_BLOCK_COUNT),
# and tools/make_zeta_rom.sh lays the image at 0x20000 in a 512 KiB part.  This
# was 256 - one eighth of the space - and the image was full: with stdout
# buffered in libc the sixteen binaries plus the docs took 254 blocks against a
# 256 ceiling, which was not a margin, just a wall.  The block count is stored
# in a 16-bit field, so 768 is well inside what the format can describe.
MAX_ZONES = 768
MAGIC = 0x137f
INODE_DISK_SIZE = 32
DIR_ENTRY_SIZE = 16
ROOT_INODE = 1

# The inode table needs 16 * 32 = 512 bytes = 1 block (512 bytes).
# But we reserve 2 blocks to match the existing layout and allow growth.
INODE_TABLE_BLOCKS = 2
INODE_TABLE_OFFSET = 3 * BLOCK_SIZE  # Block 3 and 4
ROOT_DIR_BLOCK = INODE_TABLE_OFFSET / BLOCK_SIZE + INODE_TABLE_BLOCKS  # Block 5
FIRST_DATA_ZONE = ROOT_DIR_BLOCK + 1  # Block 6

# Must match MUZIX_FS_DIRECT_ZONES in fs/fs_types.h: the kernel reads exactly
# this many direct zone pointers from the inode and then follows indirect_zone.
# A file larger than DIRECT_ZONES * 512 bytes needs a real indirect block.
DIRECT_ZONES = 7
ZONES_PER_INDIRECT_BLOCK = BLOCK_SIZE / 2

# Parse names: each name is a path like /bin/ls or just ls
# Returns [dirs, files] where dirs is an array of directory paths
# and files is the original array with parsed paths
parsed_files = []
dir_paths = {}  # path => true

files.each do |src, name|
  abort "input file not found: #{src} (the FILE side of the FILE NAME pair " \
        "for '#{name}'); every input must exist, a missing file aborts the build " \
        "- it is not skipped" unless File.file?(src)

  # Parse the name: if it contains '/', the parts are directory components
  # Strip leading slashes and split
  clean_name = name.gsub(/^\/+/, '')  # Remove leading slashes
  parts = clean_name.split('/')
  fname = parts.pop  # Last component is the filename
  dir_path = parts.empty? ? nil : parts  # nil means root

  abort "name too long (max 14)" if fname.bytesize > 14

  parsed_files << { src: src, name: fname, dir: dir_path }
  if dir_path
    # Build all parent directory paths
    (1..dir_path.length).each do |i|
      path = dir_path[0, i]
      dir_paths[path] = true
    end
  end
end

# Count all inodes needed
num_dirs = dir_paths.keys.length
all_entries = num_dirs + parsed_files.length  # directories + files + root
total_inodes_needed = 2 + all_entries  # inode 0 (unused) + inode 1 (root) + dirs + files

if total_inodes_needed > MAX_INODES
  abort "too many entries (max #{MAX_INODES - 2} after root and unused): " \
        "inode 0 is never handed out, inode 1 is the root, the inode table holds " \
        "#{MAX_INODES} slots, and every file or directory needs its own inode - " \
        "raise MAX_INODES (and the matching MUZIX_FS_INODE_SLOTS and inode-table extent) to add more"
end

total_blocks = FIRST_DATA_ZONE
data_blocks = []
file_entries = []  # Will hold {name:, data:, blocks:, start_block:, direct:, indirect_count:, indirect_block:, dir_path:, inode_num:}

inode_counter = 2  # Start after inode 0 (unused) and inode 1 (root)

# First, allocate inodes and data blocks for directories
# Directories are stored in directory entries and need their own data block
dir_inodes = {}  # dir_path_parts => inode_num
dir_blocks = {}  # dir_path_parts => block_num

dir_paths.keys.each do |dpath|
  dir_name = dpath
  dir_blocks_needed = 1
  blocks_needed = dir_blocks_needed
  direct = blocks_needed < DIRECT_ZONES ? blocks_needed : DIRECT_ZONES
  indirect_count = blocks_needed - direct

  if indirect_count > ZONES_PER_INDIRECT_BLOCK
    abort "#{dir_name}: too large"
  end

  start_block = total_blocks
  indirect_block = nil
  if indirect_count > 0
    indirect_block = total_blocks + direct
    total_blocks += 1
  end
  total_blocks += blocks_needed

  dir_inodes[dpath] = inode_counter
  dir_blocks[dpath] = start_block
  file_entries << {
    name: dir_name,
    data: nil,  # Directory: data filled later
    blocks: blocks_needed,
    start_block: start_block,
    direct: direct,
    indirect_count: indirect_count,
    indirect_block: indirect_block || 0,
    dir_path: dpath,
    inode_num: inode_counter,
    is_dir: true
  }
  inode_counter += 1
end

# Allocate inodes and data blocks for files
file_entries_files = []
parsed_files.each do |pf|
  src = pf[:src]
  fname = pf[:name]
  dir_path = pf[:dir]

  data = File.binread(src)
  blocks_needed = (data.bytesize + BLOCK_SIZE - 1) / BLOCK_SIZE
  blocks_needed = 1 if blocks_needed == 0

  direct = blocks_needed < DIRECT_ZONES ? blocks_needed : DIRECT_ZONES
  indirect_count = blocks_needed - direct

  if indirect_count > ZONES_PER_INDIRECT_BLOCK
    abort "#{fname}: too large (#{blocks_needed} blocks, max #{DIRECT_ZONES + ZONES_PER_INDIRECT_BLOCK})"
  end

  start_block = total_blocks
  direct_end = start_block + direct
  indirect_block = nil
  if indirect_count > 0
    indirect_block = direct_end
    total_blocks += 1  # for the indirect block
  end
  total_blocks += blocks_needed

  file_entries_files << {
    name: fname,
    data: data,
    blocks: blocks_needed,
    start_block: start_block,
    direct: direct,
    indirect_count: indirect_count,
    indirect_block: indirect_block || 0,
    dir_path: dir_path,
    inode_num: inode_counter,
    is_dir: false
  }
  inode_counter += 1
end

if total_blocks > MAX_ZONES
  abort "filesystem does not fit: the image needs #{total_blocks} blocks but the " \
        "ROM block device is configured for #{MAX_ZONES} blocks " \
        "(#{MAX_ZONES * BLOCK_SIZE / 1024} KiB, kernel/rom_read.h " \
        "MUZIX_ROMFS_BLOCK_COUNT). The ceiling is the device size, not the " \
        "16-bit block-count field; drop files or raise MUZIX_ROMFS_BLOCK_COUNT " \
        "and the ROM's filesystem region to match."
end
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
# allocator, inode table, all directory data blocks) and can never be file data.
# The kernel's muzix_fs_zone_is_used() must report these correctly.
(0...FIRST_DATA_ZONE).each do |zone|
  image[alloc + 4 + zone / 8] |= (1 << (zone % 8))
end

# Collect all data block zones (files + directories)
all_data_entries = file_entries + file_entries_files
all_data_entries.each do |fe|
  fe[:blocks].times do |i|
    zone = fe[:start_block] + i
    byte_idx = zone / 8
    bit = zone % 8
    image[alloc + 4 + byte_idx] |= (1 << bit)
  end
  next if fe[:indirect_count] == 0
  zone = fe[:indirect_block]
  idx = zone / 8
  image[alloc + 4 + idx] |= (1 << (zone % 8))
end

# Inode table (blocks 3-4)
inode_table = 3 * BLOCK_SIZE
image.fill(0, inode_table, 2 * BLOCK_SIZE)

# Write directory inodes and their data
# Build directory entries: parent dir -> list of (name, inode)
# First, collect all entries for each directory
dir_entries = {}  # inode_num => array of {name:, inode:}

# Root directory entries
root_entries = []
root_entries << { name: ".", inode: ROOT_INODE }
root_entries << { name: "..", inode: ROOT_INODE }

# Subdirectory entries in root
dir_inodes.each do |dpath, inode_num|
  dir_name = dpath.last.to_s
  root_entries << { name: dir_name, inode: inode_num }
  # Each subdirectory gets "." and ".." entries pointing to itself and parent
  entries = [{ name: ".", inode: inode_num }, { name: "..", inode: ROOT_INODE }]
  dir_entries[inode_num] = entries
end

dir_entries[ROOT_INODE] = root_entries

# Add files to their parent directory's entries
file_entries_files.each do |fe|
  dir_path = fe[:dir_path]
  if dir_path.nil?
    dir_entries[ROOT_INODE] << { name: fe[:name], inode: fe[:inode_num] }
  else
    parent_inode = dir_inodes[dir_path]
    dir_entries[parent_inode] << { name: fe[:name], inode: fe[:inode_num] }
  end
end

# Write inode table entries
all_entries_to_write = []
# Root inode (inode 1)
root_inode_off = inode_table + ROOT_INODE * INODE_DISK_SIZE
# Find root dir's data block
root_dir_block = dir_paths.keys.empty? ? ROOT_DIR_BLOCK : nil
if root_dir_block.nil?
  # Root directory is at ROOT_DIR_BLOCK (block 5) as before
  root_dir_block = ROOT_DIR_BLOCK
end
all_entries_to_write << {
  offset: root_inode_off,
  mode: 0x4000,  # directory
  size: 0,       # Will be set after writing directory data
  blocks: 1,
  start_block: root_dir_block,
  direct: 1,
  indirect_count: 0
}

# Directory inodes
file_entries.select { |fe| fe[:is_dir] }.each do |fe|
  offset = inode_table + fe[:inode_num] * INODE_DISK_SIZE
  all_entries_to_write << {
    offset: offset,
    mode: 0x4000,  # directory
    size: 0,
    blocks: fe[:blocks],
    start_block: fe[:start_block],
    direct: fe[:direct],
    indirect_count: fe[:indirect_count]
  }
end

# File inodes
file_entries_files.each do |fe|
  offset = inode_table + fe[:inode_num] * INODE_DISK_SIZE
  all_entries_to_write << {
    offset: offset,
    mode: 0x8000 | 0777,  # regular file with rwxrwxrwx
    size: fe[:data].bytesize,
    blocks: fe[:blocks],
    start_block: fe[:start_block],
    direct: fe[:direct],
    indirect_count: fe[:indirect_count],
    indirect_block: fe[:indirect_block]
  }
end

inode_num = 2
all_entries_to_write.each do |e|
  if e[:mode] & 0x4000 != 0  # directory
    write16(image, e[:offset] + 0, e[:mode])
    write16(image, e[:offset] + 2, 0)           # uid
    image[e[:offset] + 13] = 2                  # nlinks (for . and ..)
    write32(image, e[:offset] + 8, $build_time) # mtime
    # Size will be set after directory data is written
    write16(image, e[:offset] + 14, e[:start_block]) # zone 0
    if e[:indirect_count] > 0
      write16(image, e[:offset] + 28, e[:indirect_block])
    else
      write16(image, e[:offset] + 28, 0)
    end
  else
    write16(image, e[:offset] + 0, e[:mode])
    write16(image, e[:offset] + 2, 0)           # uid
    write32(image, e[:offset] + 4, e[:size])     # size
    write32(image, e[:offset] + 8, $build_time)  # mtime
    image[e[:offset] + 12] = 0                  # gid
    image[e[:offset] + 13] = 1                  # nlinks
    e[:direct].times do |i|
      write16(image, e[:offset] + 14 + i * 2, e[:start_block] + i)
    end
    if e[:indirect_count] > 0
      ind = e[:indirect_block] * BLOCK_SIZE
      e[:indirect_count].times do |i|
        write16(image, ind + i * 2, e[:indirect_block] + 1 + i)
      end
      write16(image, e[:offset] + 28, e[:indirect_block])
    else
      write16(image, e[:offset] + 28, 0)
    end
  end
end

# Write directory data blocks
# Root directory
dir = root_dir_block * BLOCK_SIZE
write_dir_entries = lambda do |inode_num, entries, block_num|
  block = block_num * BLOCK_SIZE
  entries.each_with_index do |entry, idx|
    offset = block + idx * DIR_ENTRY_SIZE
    write16(image, offset, entry[:inode])
    name_bytes = entry[:name].bytes
    name_bytes.each_with_index do |b, i|
      image[offset + 2 + i] = b
    end
  end
  size = entries.length * DIR_ENTRY_SIZE
  # Update the directory inode's size
  dir_inode_off = inode_table + inode_num * INODE_DISK_SIZE
  write32(image, dir_inode_off + 4, size)
end

# Write root directory
write_dir_entries.call(ROOT_INODE, dir_entries[ROOT_INODE], root_dir_block)

# Write subdirectory directory data blocks
file_entries.select { |fe| fe[:is_dir] }.each do |fe|
  write_dir_entries.call(fe[:inode_num], dir_entries[fe[:inode_num]], fe[:start_block])
end

# Write data blocks for files
file_entries_files.each do |fe|
  data = fe[:data].bytes.to_a
  # Direct zones: start_block + 0 through start_block + direct - 1
  fe[:direct].times do |i|
    block_idx = fe[:start_block] + i
    chunk = data[i * BLOCK_SIZE, BLOCK_SIZE] || []
    chunk.each_with_index { |byte, j| image[block_idx * BLOCK_SIZE + j] = byte }
  end
  # Indirect zones: indirect_block + 1 through indirect_block + indirect_count
  fe[:indirect_count].times do |i|
    block_idx = fe[:indirect_block] + 1 + i
    chunk = data[(fe[:direct] + i) * BLOCK_SIZE, BLOCK_SIZE] || []
    chunk.each_with_index { |byte, j| image[block_idx * BLOCK_SIZE + j] = byte }
  end
end

File.binwrite(output, image.pack("C*"))
puts "Created #{output} (#{total_blocks} blocks, #{total_blocks * BLOCK_SIZE} bytes)"
