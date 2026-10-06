#!/usr/bin/env ruby
# Prefix a flat user image with its linker-resolved entry point.  This target
# has no generated GSINIT startup body, so _CODE begins with ordinary helper
# functions; execution must start at _main.
input, map_file = ARGV
abort "usage: add_exec_header.rb IMAGE.bin IMAGE.map" unless map_file
image = File.binread(input)
map = File.read(map_file)
match = map.match(/^\s*([0-9A-Fa-f]+)\s+start\s+/)
abort "missing start in #{map_file}" unless match
entry = Integer(match[1], 16)
header = [0x4d, 0x5a, entry & 0xff, (entry >> 8) & 0xff].pack("C*")
File.binwrite(input, header + image)
