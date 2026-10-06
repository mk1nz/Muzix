#!/usr/bin/env ruby
# frozen_string_literal: true

# Verify that a linked userspace image respects the layout lib/crt0.s assumes.
#
# The userspace equivalent of check_kernel_layout.rb, and it guards three
# separate assumptions that used to fail silently:
#
#   1. _DATA must not sit inside _CODE.  shell/shell.lk used to pin
#      `-b _DATA = 0x9000` while shell's text ran 0x8000..0x9833.  sdld emitted
#      both, the ROMFS was built, every size check passed, and the data image
#      landed on live instructions: _sys_times at 0x9129, _sys_signal at
#      0x914A, _sys_open at 0x9191.  The .lk files now let the linker place
#      _DATA after _CODE; this check is what keeps them that way.
#
#   2. _DATA must be followed by _INITIALIZED, then _INITIALIZER.  lib/crt0.s
#      has no _BSS area to clear, so it clears l__DATA bytes at s__DATA.  That
#      is only correct while _DATA is the uninitialised area and the other two
#      sit above it.  Deriving the BSS length as
#      (s__DATA + l__DATA) - (s__INITIALIZED + l__INITIALIZED) is what crt0 used
#      to do; with _DATA below _INITIALIZED that is 0x992C - 0x99C4 = -152, and
#      memset_bss cleared 65416 bytes upward out of the process image.
#
#   3. The whole image must fit the text window.  exec_loader copies the flat
#      image to MUZIX_EXEC_TEXT_ADDR in window 2 ($8000-$BFFF) and rejects
#      anything larger than MUZIX_EXEC_TEXT_SIZE, so _INITIALIZER's end has to
#      stay inside the window as well.
#
# Usage: check_userspace_layout.rb <binary.map> [text_base] [text_limit]

CODE_BASE = 0x8000
TEXT_LIMIT = 0x4000

def symbol(map, name)
  m = map.match(/^\s+([0-9A-Fa-f]{4,8})\s+#{Regexp.escape(name)}\s*$/)
  m && m[1].to_i(16)
end

def area(map, name)
  m = map.match(/^#{Regexp.escape(name)}\s+([0-9A-Fa-f]{4,8})\s+([0-9A-Fa-f]{4,8})\s+=/)
  m && [m[1].to_i(16), m[2].to_i(16)]
end

map_path = ARGV[0] or abort 'usage: check_userspace_layout.rb <binary.map> [text_base] [text_limit]'
code_base = ARGV[1] ? ARGV[1].to_i(16) : CODE_BASE
text_limit = ARGV[2] ? ARGV[2].to_i(16) : TEXT_LIMIT

map = File.read(map_path)

code_start = symbol(map, 's__CODE') || (area(map, '_CODE') || [nil])[0]
code_len   = symbol(map, 'l__CODE') || (area(map, '_CODE') || [0, nil])[1]
data_start = symbol(map, 's__DATA') || (area(map, '_DATA') || [nil])[0]
data_len   = symbol(map, 'l__DATA') || (area(map, '_DATA') || [0, nil])[1]
init_start = symbol(map, 's__INITIALIZED') || (area(map, '_INITIALIZED') || [nil])[0]
init_len   = symbol(map, 'l__INITIALIZED') || (area(map, '_INITIALIZED') || [0, nil])[1]
img_start  = symbol(map, 's__INITIALIZER') || (area(map, '_INITIALIZER') || [nil])[0]
img_len    = symbol(map, 'l__INITIALIZER') || (area(map, '_INITIALIZER') || [0, nil])[1]

if [code_start, code_len, data_start, data_len].any?(&:nil?)
  abort "FAIL: could not read _CODE/_DATA extents from #{map_path}"
end

code_end = code_start + code_len
data_end = data_start + data_len

errors = []

# (1) code and data must not collide.
if code_start != code_base
  errors << format('_CODE base is 0x%04X, expected 0x%04X', code_start, code_base)
end
if data_start < code_end
  errors << format('_DATA at 0x%04X is inside _CODE (ends 0x%04X): %d bytes of collision',
                    data_start, code_end, code_end - data_start)
end

# (2) crt0 clears l__DATA at s__DATA, so the initialised pair must sit above it.
if init_start && init_start < data_end
  errors << format('_DATA ends at 0x%04X but _INITIALIZED starts at 0x%04X: crt0 memset would ' \
                    'clear the initialised copy target', data_end, init_start)
end
if init_start && img_start && init_start + init_len.to_i > img_start
  errors << format('_INITIALIZED 0x%04X..0x%04X overlaps _INITIALIZER at 0x%04X',
                    init_start, init_start + init_len.to_i, img_start)
end

# (3) the image must fit the text window.
image_end = img_start ? img_start + img_len.to_i : data_end
if image_end > code_base + text_limit
  errors << format('image ends at 0x%04X, past the text window 0x%04X..0x%04X',
                    image_end, code_base, code_base + text_limit)
end

puts "  userspace layout: _CODE 0x%04X..0x%04X (%d bytes)" % [code_start, code_end, code_len.to_i]
puts "                   _DATA 0x%04X..0x%04X  _INITIALIZED 0x%04X+0x%02X  _INITIALIZER 0x%04X+0x%02X" %
     [data_start, data_end, init_start.to_i, init_len.to_i, img_start.to_i, img_len.to_i]
puts "                   image ends 0x%04X, window limit 0x%04X" % [image_end, code_base + text_limit]

if errors.any?
  errors.each { |e| puts "  x #{e}" }
  abort "FAIL: #{File.basename(map_path)} layout violates an assumption lib/crt0.s makes"
end

puts '  x no _CODE/_DATA overlap, crt0 data order intact: OK'
