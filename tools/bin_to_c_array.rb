#!/usr/bin/env ruby
# Convert a binary file to a C source file with a const array.
# Usage: bin_to_c_array.rb SYMBOL_NAME OUTPUT.c INPUT.bin [block_count]

abort "usage: #{$PROGRAM_NAME} SYMBOL OUTPUT.c INPUT.bin [block_count]" if ARGV.length < 3

symbol = ARGV[0]
output = ARGV[1]
input = ARGV[2]
block_count = ARGV[3] || 0

data = File.binread(input)

File.open(output, "w") do |f|
  f.puts "/* Auto-generated from #{input} */"
  f.puts "#include <stdint.h>"
  f.puts "const uint8_t #{symbol}[] = {"
  data.bytes.each_slice(16) do |row|
    f.puts "  " + row.map { |b| "0x%02x" % b }.join(", ") + ","
  end
  f.puts "};"
  f.puts "const uint16_t #{symbol}_blocks = #{block_count};"
end

puts "Generated #{output} (#{data.bytesize} bytes, #{block_count} blocks)"
