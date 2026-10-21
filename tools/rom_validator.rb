#!/usr/bin/env ruby
# ROM File Validator - Byte-by-byte validation of Muzix Zeta V2 ROM image
# Checks: boot stub, kernel, MINIX-v1-format filesystem structure and contents

require 'optparse'

ROM_SIZE = 512 * 1024
KERNEL_SIZE = 128 * 1024
KERNEL_OFFSET = 0x0098
BOOT_STUB_MAX = KERNEL_OFFSET
ROOTFS_OFFSET = KERNEL_SIZE
ROOTFS_SIZE = ROM_SIZE - KERNEL_SIZE

BLOCK_SIZE = 512
MINIX_MAGIC = 0x137f

# Every file the ROM is supposed to carry, checked for the size its inode
# claims and for its data being present at all.
#
# The Makefile's make_minixfs.rb call puts files in the image - readme.txt,
# hw-notes.txt, copy-notes.txt, echo, cat, ls, date, top, uptime, loadavg, hwdiag,
# z80test, shell, fs_server, mm_server, pm_server - and the directories
# holds all fifteen,
# but the loop below walks EXPECTED_FILES and not the directory, so a truncated
# or misplaced user command, which
# is exactly the failure a filesystem running out of room produces, must not
# pass validation silently.
#
# The three servers were added to the Makefile's make_minixfs.rb invocation when
# the FS/MM/PM servers moved out of the kernel, and this list was not widened
# with it: it stayed at twelve names, so a server blob that was missing or
# truncated failed in the emulator at boot with no size check disagreeing.  The
# server names are the ones most easily forgotten again - keep this list in
# step with that invocation.
#
# The directory is read once into name_to_inode below and is a better source of
# truth than any hand-kept list, so the check that a file is PRESENT is
# unaffected by this - it was the per-file data and MZ checks that had the
# expected-file data coverage.
#
# Files are stored in / (root for docs), /bin and /sbin subdirectories. Each entry is
# [name, path] where path is the full filesystem path.
EXPECTED_FILES = [
  ["echo",      "/bin/echo"],
  ["cat",       "/bin/cat"],
  ["ls",        "/bin/ls"],
  ["date",      "/bin/date"],
  ["top",       "/bin/top"],
  ["uptime",    "/bin/uptime"],
  ["loadavg",   "/bin/loadavg"],
  ["hwdiag",    "/bin/hwdiag"],
  ["readme",    "/readme.txt"],
  ["hw-notes",  "/hw-notes.txt"],
  ["copy-notes","/copy-notes.txt"],
  ["z80test",   "/bin/z80test"],
  [nil,         "/"],            # root directory
  [nil,         "/bin"],           # directory itself
  ["shell",     "/sbin/shell"],
  ["fs_server", "/sbin/fs_server"],
  ["mm_server", "/sbin/mm_server"],
  ["pm_server", "/sbin/pm_server"],
  [nil,         "/sbin"]           # directory itself
]

# Files that are MZ executables and must therefore start with 0x4D 0x5A.  The
# same files carry the same MZ header and are checked for it here - an exec
# header missing from a userspace binary is the difference between a program
# the loader runs and a blob it refuses at exec.
BINARY_FILES = %w[
  echo cat ls date top uptime loadavg hwdiag z80test
  shell fs_server mm_server pm_server
]

# Resolve a path like /bin/ls or /sbin/fs_server in the MINIX filesystem.
# Returns [inode_num, size, mode, zone0] or nil if not found.
def resolve_path(rom, fs_start, inode_table_offset, ninodes, path)
  # Parse path components
  components = path.split('/').reject { |c| c.empty? }
  current_inode = 1  # ROOT_INODE

  components.each_with_index do |comp, idx|
    is_last = (idx == components.length - 1)
    comp_len = comp.length

    # Read the inode for current directory
    inode_off = inode_table_offset + current_inode * 32
    return nil if inode_off + 31 >= rom.bytesize

    mode = read_u16(rom, inode_off + 0)

    # Get the data zone (directory block)
    zone0 = read_u16(rom, inode_off + 14)
    return nil if zone0 == 0

    dir_offset = fs_start + zone0 * BLOCK_SIZE
    return nil if dir_offset + BLOCK_SIZE > rom.bytesize

    # Search directory entries for this component
    found = false
    (0..(BLOCK_SIZE / 16 - 1)).each do |i|
      entry_off = dir_offset + i * 16
      entry_inode = read_u16(rom, entry_off + 0)
      break if entry_inode == 0

      entry_name = rom[entry_off + 2, 14].delete("\0")
      if entry_name == comp
        current_inode = entry_inode
        found = true
        break
      end
    end

    return nil unless found
  end

  # Get the final inode's info
  inode_off = inode_table_offset + current_inode * 32
  return nil if inode_off + 31 >= rom.bytesize

  info = {
    inode: current_inode,
    mode: read_u16(rom, inode_off + 0),
    size: read_u32(rom, inode_off + 4),
    nlinks: rom[inode_off + 13].ord,
    zone0: read_u16(rom, inode_off + 14)
  }
  info
end

def read_u16(buf, offset)
  buf[offset].ord | (buf[offset + 1].ord << 8)
end

def read_u32(buf, offset)
  buf[offset].ord | (buf[offset + 1].ord << 8) | (buf[offset + 2].ord << 16) | (buf[offset + 3].ord << 24)
end

def validate_rom(rom_path, verbose: false)
  abort "ROM file not found: #{rom_path}" unless File.file?(rom_path)
  
  rom = File.binread(rom_path)
  errors = []
  warnings = []

  puts "=== Muzix Zeta V2 ROM Validator ===" if verbose
  puts "ROM: #{rom_path} (#{rom.bytesize} bytes)" if verbose
  puts

  # 1. Check total ROM size
  if rom.bytesize != ROM_SIZE
    errors << "ROM size: expected #{ROM_SIZE} bytes, got #{rom.bytesize}"
  else
    puts "✓ ROM size: #{ROM_SIZE} bytes (512 KiB)" if verbose
  end

  # 2. Check boot stub region (0x0000 - 0x0097)
  puts "\n--- Boot Stub (0x0000-0x0097) ---" if verbose
  if rom[0, KERNEL_OFFSET].bytes.all? { |b| b == 0 }
    warnings << "Boot stub region is all zeros (may be expected if not yet built)"
  else
    puts "✓ Boot stub present (non-zero)" if verbose
  end

  # 3. Check kernel entry at 0x0098 (should be DI=0xF3, LD SP=0x31)
  puts "\n--- Kernel Entry (0x0098) ---" if verbose
  kernel_entry = rom[KERNEL_OFFSET, 2]
  if kernel_entry.bytesize == 2 && kernel_entry[0].ord == 0xF3 && kernel_entry[1].ord == 0x31
    puts "✓ Kernel entry: DI (0xF3) + LD SP,nn (0x31)" if verbose
  else
    errors << "Kernel entry at 0x0098: expected F3 31, got #{kernel_entry.bytes.map { |b| sprintf('%02X', b) }.join(' ')}"
  end

  # 4. Check MINIX-v1-format filesystem at ROOTFS_OFFSET
  puts "\n--- MINIX-v1-format Filesystem (0x20000) ---" if verbose
  fs_start = ROOTFS_OFFSET
  
  # Superblock at block 1 (offset 512 from fs start)
  sb_offset = fs_start + BLOCK_SIZE
  if sb_offset + 15 <= rom.bytesize
    ninodes = read_u16(rom, sb_offset + 0)
    nzones = read_u16(rom, sb_offset + 2)
    magic = read_u16(rom, sb_offset + 13)
    
    puts "  Superblock:" if verbose
    puts "    ninodes: #{ninodes}" if verbose
    puts "    nzones: #{nzones}" if verbose
    puts "    magic: 0x#{magic.to_s(16)}" if verbose
    
    if magic != MINIX_MAGIC
      errors << "MINIX-v1 filesystem magic: expected 0x137f, got 0x#{magic.to_s(16)}"
    else
      puts "  ✓ MINIX-v1 filesystem magic: 0x137f" if verbose
    end
  else
    errors << "Superblock truncated"
  end

  # Zone allocator at block 2
  alloc_offset = fs_start + 2 * BLOCK_SIZE
  if alloc_offset + 4 <= rom.bytesize
    first_zone = read_u16(rom, alloc_offset + 0)
    total_zones = read_u16(rom, alloc_offset + 2)
    puts "  Zone allocator: first=#{first_zone}, zones=#{total_zones}" if verbose
  else
    errors << "Zone allocator truncated"
  end

  # Inode table size depends on ninodes from superblock
  inode_table_blocks = (ninodes * 32 + BLOCK_SIZE - 1) / BLOCK_SIZE
  inode_table_offset = fs_start + 3 * BLOCK_SIZE
  if inode_table_offset + inode_table_blocks * BLOCK_SIZE <= rom.bytesize
    puts "  Inode table: #{ninodes} inodes in #{inode_table_blocks} block(s)" if verbose
    
     # Root directory follows inode table (inode table starts at block 3)
      root_dir_block = 3 + inode_table_blocks
      dir_offset = fs_start + root_dir_block * BLOCK_SIZE
      
      # Read root directory entries for verbose listing
      if dir_offset + BLOCK_SIZE <= rom.bytesize
        (0..(BLOCK_SIZE / 16 - 1)).each do |i|
          entry_off = dir_offset + i * 16
          inode = read_u16(rom, entry_off + 0)
          break if inode == 0
          name = rom[entry_off + 2, 14].delete("\0")
          puts "    Dir entry: inode=#{inode}, name='#{name}'" if verbose
        end
      end
      
      # Check each expected file by resolving its full path
      EXPECTED_FILES.each do |short_name, full_path|
        info = resolve_path(rom, fs_start, inode_table_offset, ninodes, full_path)
        if info.nil?
          errors << "File '#{short_name}': not found at '#{full_path}'"
          next
        end
        
        if info[:nlinks] == 0
          errors << "File '#{short_name}': inode #{info[:inode]} has 0 links (missing)"
        elsif info[:size] == 0
          warnings << "File '#{short_name}': size is 0 bytes"
        else
          puts "    ✓ #{short_name}: inode=#{info[:inode]}, size=#{info[:size]}, zone=#{info[:zone0]}, mode=0x#{info[:mode].to_s(16)}" if verbose
        end
      end
  else
    errors << "Inode table truncated"
  end

  # 5. Verify file data integrity
  puts "\n--- File Data Verification ---" if verbose
  
  EXPECTED_FILES.each do |short_name, full_path|
    next if short_name.nil?  # Skip directory entries
    info = resolve_path(rom, fs_start, inode_table_offset, ninodes, full_path)
    if info.nil?
      errors << "File '#{short_name}': not found at '#{full_path}'"
      next
    end
    
    size = info[:size]
    zone0 = info[:zone0]
    
    if size > 0 && zone0 > 0
      data_offset = fs_start + zone0 * BLOCK_SIZE
      if data_offset + size <= rom.bytesize
        file_data = rom[data_offset, size]
        
        # For binary files, check they have MZ header (0x4D 0x5A)
        if BINARY_FILES.include?(short_name)
          if file_data[0].ord == 0x4D && file_data[1].ord == 0x5A
            puts "    ✓ #{short_name}: MZ header present, #{size} bytes" if verbose
          else
            errors << "File '#{short_name}': missing MZ header (got #{file_data[0,2].bytes.map { |b| sprintf('%02X', b) }.join(' ')})"
          end
        else
          puts "    ✓ #{short_name}: #{size} bytes" if verbose
        end
      else
        errors << "File '#{short_name}': data truncated (need #{size} bytes at 0x#{data_offset.to_s(16)})"
      end
    elsif size > 0
      errors << "File '#{short_name}': size=#{size} but zone0=#{zone0}"
    end
  end

  # 6. Check for zero-padding after filesystem
  puts "\n--- Zero Padding Check ---" if verbose
  # Find last non-zero byte in rootfs region.
  #
  # getbyte(), and not rom[i].  In Ruby 1.9 and later String#[Integer] returns a
  # one-character STRING rather than the byte's value, and a string is never
  # == 0 - so `rom[i] != 0` is true for every byte including NUL, the loop
  # below breaks on its first iteration, and last_nonzero lands on the final
  # byte of the ROM no matter what is in the filesystem.  The report then reads
  # "used 393216 bytes (100.0%), free 0" on an image whose filesystem is
  # 17.5% full, and the "very little free space" warning below can never fire
  # for a real reason.  It measured that way on this tree's own ROM.
  #
  # read_u16/read_u32 in this file go through unpack and are unaffected; this was
  # the only place that indexed a String expecting a number.
  last_nonzero = nil
  (fs_start...rom.bytesize).reverse_each do |i|
    if rom.getbyte(i) != 0
      last_nonzero = i
      break
    end
  end
  
  if last_nonzero
    used = last_nonzero - fs_start + 1
    free = ROOTFS_SIZE - used
    puts "  Filesystem used: #{used} bytes (#{(used.to_f/ROOTFS_SIZE*100).round(1)}%)" if verbose
    puts "  Free space: #{free} bytes" if verbose
    if free < 1024
      warnings << "Very little free space in ROMFS: #{free} bytes"
    end
  end

  # Summary
  puts "\n=== Validation Summary ==="
  if errors.empty?
    puts "✓ ALL CHECKS PASSED"
    puts "ROM image is valid and complete."
  else
    puts "✗ FAILED: #{errors.length} error(s)"
    errors.each { |e| puts "  ERROR: #{e}" }
  end
  
  if warnings.any?
    puts "\nWarnings (#{warnings.length}):"
    warnings.each { |w| puts "  WARN: #{w}" }
  end

  errors.empty?
end

# Main
options = { verbose: false }
OptionParser.new do |opts|
  opts.banner = "Usage: rom_validator.rb [options] ROM_FILE"
  opts.on("-v", "--verbose", "Verbose output") { options[:verbose] = true }
  opts.on("-h", "--help", "Show help") { puts opts; exit }
end.parse!

rom_file = ARGV[0] || "build/muzix-zeta-v2.rom"

success = validate_rom(rom_file, verbose: options[:verbose])
exit(success ? 0 : 1)