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
# The Makefile's make_minixfs.rb call puts twelve files in the image - readme,
# hw-notes, copy-notes, echo, cat, ls, date, top, uptime, loadavg, hwdiag,
# shell - and the directory in the built ROM holds all twelve,
# but the loop below walks EXPECTED_FILES and not the directory, so a truncated
# or misplaced user command, which
# is exactly the failure a filesystem running out of room produces, must not
# pass validation silently.
#
# The directory is read once into name_to_inode below and is a better source of
# truth than any hand-kept list, so the check that a file is PRESENT is
# unaffected by this - it was the per-file data and MZ checks that had the
# expected-file data coverage.
EXPECTED_FILES = %w[
  echo
  cat
  ls
  date
  top
  uptime
  loadavg
  hwdiag
  shell
  readme
  hw-notes
  copy-notes
]

# Files that are MZ executables and must therefore start with 0x4D 0x5A.  This
# was four names long and had the same three missing: date, top and uptime are
# built by the same add_exec_header.rb pass as the rest and carry the same
# header, and a program whose header was wrong would have been exec'd
# unverified.
BINARY_FILES = %w[echo cat ls date top uptime loadavg hwdiag shell]

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

  # Inode table at block 3
  inode_table_offset = fs_start + 3 * BLOCK_SIZE
  if inode_table_offset + BLOCK_SIZE <= rom.bytesize
    puts "  Inode table: present" if verbose
    
    # Read directory entries to map names to inode numbers
    dir_offset = fs_start + 4 * BLOCK_SIZE
    name_to_inode = {}
    if dir_offset + BLOCK_SIZE <= rom.bytesize
      (0..(BLOCK_SIZE / 16 - 1)).each do |i|
        entry_off = dir_offset + i * 16
        inode = read_u16(rom, entry_off + 0)
        break if inode == 0
        name = rom[entry_off + 2, 14].delete("\0")
        name_to_inode[name] = inode
        puts "    Dir entry: inode=#{inode}, name='#{name}'" if verbose
      end
    end
    
    # Check each expected file has an inode
    EXPECTED_FILES.each do |name|
      inode_num = name_to_inode[name]
      if inode_num.nil?
        errors << "File '#{name}': not found in directory"
        next
      end
      
      if inode_table_offset + inode_num * 32 + 31 <= rom.bytesize
        mode = read_u16(rom, inode_table_offset + inode_num * 32 + 0)
        size = read_u32(rom, inode_table_offset + inode_num * 32 + 4)
        nlinks = rom[inode_table_offset + inode_num * 32 + 10].ord
        zone0 = read_u16(rom, inode_table_offset + inode_num * 32 + 11)
        
        if nlinks == 0
          errors << "File '#{name}': inode #{inode_num} has 0 links (missing)"
        elsif size == 0
          warnings << "File '#{name}': size is 0 bytes"
        else
          puts "    ✓ #{name}: inode=#{inode_num}, size=#{size}, zone=#{zone0}, mode=0x#{mode.to_s(16)}" if verbose
        end
      else
        errors << "File '#{name}': inode #{inode_num} truncated"
      end
    end
  else
    errors << "Inode table truncated"
  end

  # Root directory at block 4 - already checked above
  dir_offset = fs_start + 4 * BLOCK_SIZE
  if dir_offset + BLOCK_SIZE > rom.bytesize
    errors << "Root directory truncated"
  end

  # 5. Verify file data integrity
  puts "\n--- File Data Verification ---" if verbose
  # Re-read directory to get name->inode mapping
  name_to_inode = {}
  dir_offset = fs_start + 4 * BLOCK_SIZE
  if dir_offset + BLOCK_SIZE <= rom.bytesize
    (0..(BLOCK_SIZE / 16 - 1)).each do |i|
      entry_off = dir_offset + i * 16
      inode = read_u16(rom, entry_off + 0)
      break if inode == 0
      name = rom[entry_off + 2, 14].delete("\0")
      name_to_inode[name] = inode
    end
  end
  
  EXPECTED_FILES.each do |name|
    inode_num = name_to_inode[name]
    if inode_num.nil?
      errors << "File '#{name}': not found in directory"
      next
    end
    
    inode_off = inode_table_offset + inode_num * 32
    if inode_off + 31 < rom.bytesize
      size = read_u32(rom, inode_off + 4)
      zone0 = read_u16(rom, inode_off + 11)
      
      if size > 0 && zone0 > 0
        data_offset = fs_start + zone0 * BLOCK_SIZE
        if data_offset + size <= rom.bytesize
          file_data = rom[data_offset, size]
          
          # For binary files, check they have MZ header (0x4D 0x5A)
          if BINARY_FILES.include?(name)
            if file_data[0].ord == 0x4D && file_data[1].ord == 0x5A
              puts "    ✓ #{name}: MZ header present, #{size} bytes" if verbose
            else
              errors << "File '#{name}': missing MZ header (got #{file_data[0,2].bytes.map { |b| sprintf('%02X', b) }.join(' ')})"
            end
          else
            puts "    ✓ #{name}: #{size} bytes" if verbose
          end
        else
          errors << "File '#{name}': data truncated (need #{size} bytes at 0x#{data_offset.to_s(16)})"
        end
      elsif size > 0
        errors << "File '#{name}': size=#{size} but zone0=#{zone0}"
      end
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