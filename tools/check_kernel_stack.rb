#!/usr/bin/env ruby
# frozen_string_literal: true

# Verify the kernel stack cannot overlap live kernel data.
#
# check_kernel_layout.rb only knows the distance from _DATA end to the top of
# the 64 KiB address space.  That is not the headroom the kernel actually
# needs: the deepest call chain - exec -> MM copy -> ldir staged - is what
# decides whether the stack reaches _DATA, and that chain runs only on the
# target.  This check runs that chain in the emulator and measures the minimum
# SP observed at or above 0xC000 (the kernel stack; user stacks live in window
# 1), then compares it with the real _DATA extent from the linker map.
#
# It is the check the 2026-11 loadavg regression needed: the loadavg/timer
# work grew _CODE past 0xDEC0, _DATA moved from 0xDF00 to 0xE000, and the
# deepest exec chain (SP 0xF88B) landed 25 bytes inside _DATA (ended 0xF8A4).
# The distance to 0x10000 was still 2140 bytes, so the old gate passed.
#
# Usage:
#   check_kernel_stack.rb [project-root]
#   MUZIX_STACK_CYCLES=N check_kernel_stack.rb   # override the cycle budget

require 'open3'

ROOT = File.expand_path(ARGV[0] || File.join(__dir__, '..'))
MAP  = File.join(ROOT, 'build', 'kernel.map')
ROM  = File.join(ROOT, 'build', 'muzix-zeta-v2.rom')
EMU  = File.join(ROOT, 'emulator', 'z80pack-emu')
CYCLES = ENV['MUZIX_STACK_CYCLES'] || '120000000'
INPUT  = "uptime\nloadavg\ntop\n"

# Clear stack below _DATA end, beyond the measured low.  The regression's
# deepest chain was 25 bytes INSIDE _DATA, so any positive margin catches it;
# 64 is a guard against a marginal pass.
MARGIN = 64

abort "FAIL: #{MAP} not found; run `make rom` first" unless File.exist?(MAP)
abort "FAIL: #{ROM} not found; run `make rom` first" unless File.exist?(ROM)
abort "FAIL: #{EMU} not found; run `make` in emulator/ first" unless File.exist?(EMU)

map = File.read(MAP, encoding: 'UTF-8')

def symbol(map, name)
  m = map.match(/^\s+([0-9A-Fa-f]{4,8})\s+#{Regexp.escape(name)}\s*$/)
  m && m[1].to_i(16)
end

data_start = symbol(map, 's__DATA')
data_len   = symbol(map, 'l__DATA')
if data_start.nil? || data_len.nil?
  # Fall back to the .ABS. area header, as check_kernel_layout.rb does.
  m = map.match(/^_DATA\s+([0-9A-Fa-f]{4,8})\s+([0-9A-Fa-f]{4,8})\s+=/)
  abort "FAIL: could not read _DATA extent from #{MAP}" unless m
  data_start = m[1].to_i(16)
  data_len   = m[2].to_i(16)
end
data_end = data_start + data_len

out, _status = Open3.capture2e(
  { 'MUZIX_REPORT_MIN_SP' => '1' },
  EMU, '-s', 'muzix', ROM, '-i', '-c', CYCLES,
  stdin_data: INPUT
)

line = out[/Kernel stack min SP: 0x([0-9A-Fa-f]+)/, 1]
unless line
  abort "FAIL: emulator did not report a kernel stack high-water mark\n" \
        "#{out.lines.last(20).join}"
end
min_sp = line.to_i(16)
headroom = min_sp - data_end

puts "  kernel stack: _DATA 0x%04X..0x%04X, min SP 0x%04X, headroom %d bytes" %
     [data_start, data_end, min_sp, headroom]

if min_sp < data_end + MARGIN
  abort format('FAIL: kernel stack reached 0x%04X, %d bytes above _DATA end ' \
               '0x%04X (need at least %d): the deepest exec/copy chain overlaps ' \
               'live kernel data',
               min_sp, headroom, data_end, MARGIN)
end

puts '  x kernel stack does not overlap live kernel data: OK'