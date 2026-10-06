#!/usr/bin/env ruby
# RAW ROM Validator - Byte-by-byte comparison of ROM components against source files
# Validates: boot stub, kernel binary, MINIX-v1-format filesystem image

ROM_SIZE = 512 * 1024
KERNEL_SIZE = 128 * 1024
KERNEL_OFFSET = 0x0098
BOOT_STUB_MAX = KERNEL_OFFSET
ROOTFS_OFFSET = KERNEL_SIZE
ROOTFS_SIZE = ROM_SIZE - KERNEL_SIZE

def read_u16(buf, offset)
  buf[offset].ord | (buf[offset + 1].ord << 8)
end

def validate_raw_rom(rom_path, verbose: false)
  abort "ROM file not found: #{rom_path}" unless File.file?(rom_path)
  
  rom = File.binread(rom_path)
  errors = []
  warnings = []

  puts "=== RAW ROM Byte-by-Byte Validator ===" if verbose
  puts "ROM: #{rom_path} (#{rom.bytesize} bytes)" if verbose

  # 1. Total size
  if rom.bytesize != ROM_SIZE
    errors << "ROM size: expected #{ROM_SIZE}, got #{rom.bytesize}"
  else
    puts "✓ ROM size: #{ROM_SIZE}" if verbose
  end

  # 2. Boot stub (0x0000 - 0x0097 = 152 bytes)
  puts "\n--- Boot Stub (0x0000-0x0097) ---" if verbose
  boot_stub_src = "build/muzix-rom-boot.bin"
  if File.file?(boot_stub_src)
    boot_stub_expected = File.binread(boot_stub_src)
    boot_stub_actual = rom[0, boot_stub_expected.bytesize]
    
    if boot_stub_actual == boot_stub_expected
      puts "✓ Boot stub matches source (#{boot_stub_expected.bytesize} bytes)" if verbose
    else
      # Find first mismatch
      mismatch = nil
      [boot_stub_actual.bytesize, boot_stub_expected.bytesize].min.times do |i|
        if boot_stub_actual[i] != boot_stub_expected[i]
          mismatch = i
          break
        end
      end
      if mismatch.nil? && boot_stub_actual.bytesize != boot_stub_expected.bytesize
        mismatch = [boot_stub_actual.bytesize, boot_stub_expected.bytesize].min
      end
      if mismatch
        errors << "Boot stub MISMATCH at offset 0x#{mismatch.to_s(16).upcase}: expected 0x#{boot_stub_expected[mismatch].ord.to_s(16).upcase}, got 0x#{boot_stub_actual[mismatch].ord.to_s(16).upcase}"
      else
        errors << "Boot stub MISMATCH (length differ)"
      end
    end
    
    # Check remaining bytes in boot region are zero
    if boot_stub_expected.bytesize < BOOT_STUB_MAX
      rest = rom[boot_stub_expected.bytesize, BOOT_STUB_MAX - boot_stub_expected.bytesize]
      if rest && rest.bytes.all? { |b| b == 0 }
        puts "  Remaining #{rest.bytesize} bytes in boot region are zero" if verbose
      else
        first_nonzero = rest&.bytes&.find_index { |b| b != 0 }
        if first_nonzero
          warnings << "Non-zero in boot region at offset 0x#{first_nonzero.to_s(16).upcase}"
        end
      end
    end
  else
    warnings << "Boot stub source not found: #{boot_stub_src}"
  end

  # 3. Kernel binary (at 0x0098, 64 KiB region)
  puts "\n--- Kernel Binary (0x0098-0xFFFF) ---" if verbose
  kernel_src = "build/muzix-kernel.bin"
  if File.file?(kernel_src)
    kernel_expected = File.binread(kernel_src)
    kernel_actual = rom[KERNEL_OFFSET, kernel_expected.bytesize]
    
    if kernel_actual == kernel_expected
      puts "✓ Kernel binary matches source (#{kernel_expected.bytesize} bytes)" if verbose
    else
      # Find first mismatch
      mismatch = nil
      [kernel_actual.bytesize, kernel_expected.bytesize].min.times do |i|
        if kernel_actual[i] != kernel_expected[i]
          mismatch = i
          break
        end
      end
      if mismatch.nil? && kernel_actual.bytesize != kernel_expected.bytesize
        mismatch = [kernel_actual.bytesize, kernel_expected.bytesize].min
      end
      if mismatch
        errors << "Kernel binary MISMATCH at 0x#{(KERNEL_OFFSET + mismatch).to_s(16).upcase}: expected 0x#{kernel_expected[mismatch].ord.to_s(16).upcase}, got 0x#{kernel_actual[mismatch].ord.to_s(16).upcase}"
      else
        errors << "Kernel binary MISMATCH (length differ)"
      end
    end
    
    # Check remaining bytes in kernel region are zero
    kernel_region_end = ROOTFS_OFFSET
    kernel_in_rom = rom[KERNEL_OFFSET, kernel_region_end - KERNEL_OFFSET]
    if kernel_in_rom.bytesize > kernel_expected.bytesize
      rest = kernel_in_rom[kernel_expected.bytesize..-1]
      if rest && rest.bytes.all? { |b| b == 0 }
        puts "  Remaining #{rest.bytesize} bytes in kernel region are zero" if verbose
      else
        first_nonzero = rest&.bytes&.find_index { |b| b != 0 }
        if first_nonzero
          warnings << "Non-zero in kernel region at 0x#{(KERNEL_OFFSET + kernel_expected.bytesize + first_nonzero).to_s(16).upcase}"
        end
      end
    end
  else
    warnings << "Kernel source not found: #{kernel_src}"
  end

  # 4. MINIX-v1-format filesystem image (at 0x20000, 384 KiB)
  puts "\n--- MINIX-v1-format Filesystem (0x20000-0x7FFFF) ---" if verbose
  minifs_src = "build/muzix-minifs.bin"
  if File.file?(minifs_src)
    minifs_expected = File.binread(minifs_src)
    minifs_actual = rom[ROOTFS_OFFSET, ROOTFS_SIZE]
    
    # The minifs may be smaller than 448 KiB - check up to its actual size
    if minifs_actual[0, minifs_expected.bytesize] == minifs_expected
      puts "✓ MINIX-v1-format FS matches source (#{minifs_expected.bytesize} bytes)" if verbose
      
      # Check remaining bytes are zero
      rest = minifs_actual[minifs_expected.bytesize..-1]
      if rest && rest.bytes.all? { |b| b == 0 }
        puts "  Remaining #{rest.bytesize} bytes are zero-padded" if verbose
      else
        first_nonzero = rest&.bytes&.find_index { |b| b != 0 }
        if first_nonzero
          warnings << "Non-zero data after MINIX-v1-format FS at offset 0x#{(ROOTFS_OFFSET + minifs_expected.bytesize + first_nonzero).to_s(16).upcase}"
        end
      end
    else
      # Find first mismatch
      mismatch = nil
      [minifs_actual.bytesize, minifs_expected.bytesize].min.times do |i|
        if minifs_actual[i] != minifs_expected[i]
          mismatch = i
          break
        end
      end
      if mismatch.nil? && minifs_actual.bytesize != minifs_expected.bytesize
        mismatch = [minifs_actual.bytesize, minifs_expected.bytesize].min
      end
      if mismatch
        errors << "MINIX-v1-format FS MISMATCH at 0x#{(ROOTFS_OFFSET + mismatch).to_s(16).upcase}: expected 0x#{minifs_expected[mismatch].ord.to_s(16).upcase}, got 0x#{minifs_actual[mismatch].ord.to_s(16).upcase}"
      else
        errors << "MINIX-v1-format FS MISMATCH (length differ)"
      end
    end
  else
    warnings << "MINIX-v1-format FS source not found: #{minifs_src}"
  end

  # 5. Verify MINIX-v1 filesystem magic at expected offset
  puts "\n--- MINIX-v1 Filesystem Magic Check ---" if verbose
  magic_offset = ROOTFS_OFFSET + 512 + 13
  if magic_offset + 1 < rom.bytesize
    magic = read_u16(rom, magic_offset)
    if magic == 0x137f
      puts "✓ MINIX-v1 filesystem magic 0x137f at 0x#{magic_offset.to_s(16).upcase}" if verbose
    else
      errors << "MINIX-v1 filesystem magic: expected 0x137f at 0x#{magic_offset.to_s(16).upcase}, got 0x#{magic.to_s(16).upcase}"
    end
  else
    errors << "MINIX-v1 filesystem magic offset out of bounds"
  end

  # Summary
  puts "\n=== Raw Validation Summary ==="
  if errors.empty?
    puts "✓ ALL RAW BYTE CHECKS PASSED"
    puts "ROM components exactly match source files."
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
require 'optparse'
OptionParser.new do |opts|
  opts.banner = "Usage: rom_raw_validator.rb [options] ROM_FILE"
  opts.on("-v", "--verbose", "Verbose output") { options[:verbose] = true }
  opts.on("-h", "--help", "Show help") { puts opts; exit }
end.parse!

rom_file = ARGV[0] || "build/muzix-zeta-v2.rom"

success = validate_raw_rom(rom_file, verbose: options[:verbose])
exit(success ? 0 : 1)