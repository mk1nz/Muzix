#!/usr/bin/env ruby
# frozen_string_literal: true

# Verify that the linked kernel image does not overlap itself.
#
# sdldz80 happily places _CODE and _DATA on top of each other when the two
# areas do not fit in the 64 KiB Z80 address space: it emits both, the ROM is
# produced, and every size check passes.  At runtime the kernel then zeroes
# its own code, so the failure shows up as a wild jump, not as a build error.
#
# This guard is the only thing standing between a layout regression and a
# silently self-corrupting kernel, so it fails the build.

CODE_BASE   = 0x0098
ADDRESS_MAX = 0x10000
# The kernel stack lives above _DATA: kernel_entry.s and syscall_entry.s both
# `ld sp,#0xFFFE`, and window 3 is the kernel page in every map, so the free
# span between the top of _DATA and the top of the address space IS the stack.
# 1024 bytes is the floor: the deepest chain measured (userspace syscall ->
# filesystem service -> block cache) needs over 1500, and with less the kernel
# writes through its own globals - SP reached 0xEEF7, inside _DATA, and the
# next `ret` jumped into the boot stub.
MIN_HEADROOM = 1024

map_path = ARGV[0] or abort "usage: check_kernel_layout.rb <kernel.map>"

map = File.read(map_path)

# s__CODE / l__CODE and s__DATA / l__DATA are emitted in the .ABS. block and
# are unambiguous, unlike the repeated area headers.
def symbol(map, name)
  m = map.match(/^\s+([0-9A-Fa-f]{4,8})\s+#{Regexp.escape(name)}\s*$/)
  m && m[1].to_i(16)
end

def area(map, name)
  m = map.match(/^#{Regexp.escape(name)}\s+([0-9A-Fa-f]{4,8})\s+([0-9A-Fa-f]{4,8})\s+=/)
  m && [m[1].to_i(16), m[2].to_i(16)]
end

code_start = symbol(map, 's__CODE')
code_len   = symbol(map, 'l__CODE')
data_start = symbol(map, 's__DATA')
data_len   = symbol(map, 'l__DATA')

if code_start.nil? || code_len.nil? || data_start.nil? || data_len.nil?
  code_start, code_len = area(map, '_CODE')
  data_start, data_len = area(map, '_DATA')
end

abort "FAIL: could not read _CODE/_DATA extents from #{map_path}" if code_start.nil? || data_start.nil?

code_end   = code_start + code_len
data_end   = data_start + data_len
init_start = symbol(map, 's__INITIALIZED')
init_end   = (init_start && symbol(map, 'l__INITIALIZED')) ? init_start + symbol(map, 'l__INITIALIZED') : init_start

# The _DATA *area size* the linker reports is not trustworthy on its own: when
# the objects do not fit, sdld places them on top of each other and keeps
# reporting the smaller area size. So measure the real extent from the symbols
# the linker actually emitted under the _DATA area header, and use whichever
# of the two reaches further.
data_syms = []
area_name = nil
map.each_line do |line|
  if (m = line.match(/^(_[A-Za-z0-9_]+)\s+[0-9A-Fa-f]{4,8}\s+[0-9A-Fa-f]{4,8}\s+=/))
    area_name = m[1]
    next
  end
  next if line.start_with?('ASxxxx', 'Area')
  next unless area_name == '_DATA'
  if (m = line.match(/^\s+([0-9A-Fa-f]{4,8})\s+(_[A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*$/))
    data_syms << [m[1].to_i(16), m[2]]
  end
end

highest = data_syms.map(&:first).max || data_start
highest_name = (data_syms.find { |a, _n| a == highest } || [0, '?'])[1]
# Extent of the highest object: its address plus a conservative 8 KiB probe is
# meaningless, so report the address itself and let the overlap check below
# catch anything sitting on top of another object.
data_reach = [data_end, highest].max

errors = []

if code_start != CODE_BASE
  errors << "_CODE base is 0x#{code_start.to_s(16)}, expected 0x#{CODE_BASE.to_s(16)}"
end

# The kernel's own initialised-data copy must not land on its own code.
if data_start < code_end
  errors << format('_DATA at 0x%04X overlaps _CODE which ends at 0x%04X (%d bytes of collision)',
                   data_start, code_end, code_end - data_start)
end

if init_start && init_start < data_end
  errors << format('_INITIALIZED at 0x%04X overlaps _DATA which ends at 0x%04X', init_start, data_end)
end

# NO CODE OUTSIDE _CODE, and _HOME is where sdldz80 puts it
# --------------------------------------------------------------
# _HOME is a third code area and it is placed at the _DATA base, so a kernel
# that populated it would be running multiply code from inside the data segment,
# under the kernel stack, where gsinit zeroes it.  Both gates passed that image:
# this file collected only the symbols listed under the _CODE and _DATA area
# headers, and tools/kernel_data_base.rb derives the _DATA base from l__CODE
# alone, so an area that is neither of the two is in neither list.
#
# It is reachable without anything obviously wrong: the kernel holds no 32-bit
# multiply, __mullong does not resolve from the .rel files the Makefile links,
# and supplying it from SDCC's own lib/z80/z80.lib makes the link succeed.
# Measured with that library linked, the map reads
#
#     _CODE  0x0098  56 bytes
#     _HOME  0xEA00  227 bytes      <- s__HOME == s__DATA
#
# So the rule is that the kernel link must not populate _HOME at all, rather
# than a size limit: even placed correctly it would be code below _DATA, which
# every figure in this file - the headroom, the overlap check, the "_CODE ends
# here" claim - assumes does not exist.
home_len = symbol(map, 'l__HOME')
home_start = symbol(map, 's__HOME')
if home_len && home_len > 0
  errors << format('_HOME holds %d bytes at 0x%04X: sdldz80 places that area at ' \
                   'the _DATA base, so this is code linked INSIDE the data ' \
                   'segment and under the kernel stack. In this kernel code ' \
                   'belongs in _CODE; check whether something pulled in an ' \
                   'unresolved runtime helper such as __mullong, which the ' \
                   '.rel-only link cannot supply and z80.lib can.',
                   home_len, home_start || 0)
end

# NO LIBRARY IN THE KERNEL LINK
# -------------------------------
# KERNEL_LINK_ORDER is a list of .rel files and must stay one.  A library can be
# made to supply a runtime helper the .rel files do not carry - __mulint for a
# uint16 multiply, __mullong for a 32-bit one - and the link then succeeds and
# puts the helper's code in _HOME, which sdldz80 places at the _DATA base.  The
# library does not fail the link; it puts executable code inside the data segment
# where gsinit zeroes it, and the image boots and then runs zeroes.
#
# AGENTS.md carries the numbers, and the _HOME check above catches the symptom.
# This catches the cause, at the point where it is typed, and reads the Makefile
# by LINE rather than by pattern so that a reworded comment cannot make it look
# satisfied.
#
# The tree writes its own arithmetic instead: platform/zeta-v2/tick.s builds a
# 32-bit accumulate out of four byte-sized steps because no helper exists here.
# Adding one to a module is fine; borrowing one is what this forbids.
library_refs = []
makefile = File.expand_path('../Makefile', __dir__)
if File.readable?(makefile)
  in_order = false
  File.read(makefile, encoding: 'UTF-8').each_line do |line|
    text = line.chomp
    if text =~ /\A\s*KERNEL_LINK_ORDER\s*:?=/ then
      in_order = true
      text = text.sub(/\A\s*KERNEL_LINK_ORDER\s*:?=/, '')
    end
    if in_order
      body = text.sub(/\\\s*\z/, '')
      body.scan(/[^\s\\]+/) { |t| library_refs << t unless t.end_with?('.rel') }
      in_order = false unless line.rstrip.end_with?('\\')
    end
    if text =~ /sdldz80/ || text =~ /\A\s*-l/
      library_refs.concat(text.scan(/(?:^|[\s])(-l\S+|\S+\.lib)\b/).flatten)
    end
  end
else
  errors << 'could not read the Makefile to check that the kernel link is .rel only'
end
library_refs.uniq!
unless library_refs.empty?
  errors << 'the kernel link names something that is not a .rel file: ' +
            library_refs.join(', ') +
            '. KERNEL_LINK_ORDER is .rel only, and that is the rule: a library ' \
            'supplies a runtime helper the tree does not carry, and its code ' \
            'lands in _HOME at the _DATA base, inside the data segment.'
end

if data_reach > ADDRESS_MAX
  errors << format('_DATA objects reach 0x%04X (%s), past the 0x%04X address space',
                   data_reach, highest_name, ADDRESS_MAX)
end

headroom = ADDRESS_MAX - data_reach
if headroom < MIN_HEADROOM
  errors << format('kernel stack headroom is %d bytes, below the %d-byte floor: ' \
                   'the C stack is the span above _DATA, and the syscall path ' \
                   'overflows it', headroom, MIN_HEADROOM)
end

# Independently confirm no code symbol lives inside the data area. The map
# lists symbols grouped under their area, so only entries recorded under an
# _CODE area header are code; globals in _DATA share the _DATA range and must
# not be flagged. Addresses are right-aligned in an 8-wide hex field.
code_syms = []
area_name = nil
map.each_line do |line|
  if (m = line.match(/^(_[A-Za-z0-9_]+)\s+[0-9A-Fa-f]{4,8}\s+[0-9A-Fa-f]{4,8}\s+=/))
    area_name = m[1]
    next
  end
  next if line.start_with?('ASxxxx', 'Area')
  next unless area_name == '_CODE'
  if (m = line.match(/^\s+([0-9A-Fa-f]{4,8})\s+(_[A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*$/))
    code_syms << [m[1].to_i(16), m[2]]
  end
end

inside = code_syms.select { |a, _n| a >= data_start && a < data_reach }
if inside.any?
  errors << "#{inside.size} code symbol(s) inside _DATA, e.g. " +
            inside.first(4).map { |a, n| format('0x%04X %s', a, n) }.join(', ')
end

puts "  kernel layout: _CODE 0x%04X..0x%04X (%d bytes)" % [code_start, code_end, code_len]
puts "                _DATA 0x%04X..0x%04X (area %d, highest object 0x%04X %s)" %
     [data_start, data_end, data_len, highest, highest_name]
puts "                headroom to 0x%04X: %d bytes%s" %
     [ADDRESS_MAX, headroom, headroom < MIN_HEADROOM ? ' (BELOW FLOOR)' : '']
puts "                kernel link: .rel only" if library_refs.empty?
puts "                _HOME %d bytes%s" %
     [home_len || 0, home_len && home_len > 0 ? ' (CODE OUTSIDE _CODE)' : '']

if errors.any?
  errors.each { |e| puts "  x #{e}" }
  abort "FAIL: kernel image overlaps itself (sdld does not catch this)"
end

puts '  x no _CODE/_DATA overlap: OK'
