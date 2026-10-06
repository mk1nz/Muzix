#!/usr/bin/env ruby
# Convert Intel HEX to a fixed-size binary region without external toolchain
# dependencies.  Usage: ihx_to_bin.rb INPUT.ihx OUTPUT.bin START SIZE

input, output, start_text, size_text = ARGV
abort "usage: #{$PROGRAM_NAME} INPUT.ihx OUTPUT.bin START SIZE" unless size_text

start = Integer(start_text)
size = Integer(size_text)
abort "invalid output range" if start.negative? || size <= 0
image = Array.new(size, 0)
upper = 0
max_offset = 0

File.foreach(input) do |line|
  text = line.strip
  next if text.empty?
  
  # SDCC linker outputs a variant where lines start with "00:" instead of ":"
  # Handle both standard Intel HEX (":LLAAAATTDD..DDCC") and SDCC format ("00:LLAAAATTDD..DDCC")
  if text.start_with?("00:")
    text = ":" + text[3..]
  end
  
  abort "invalid Intel HEX record" unless text.start_with?(":")
  bytes = [text[1..]].pack("H*").bytes
  length, address_hi, address_lo, type = bytes[0, 4]
  data = bytes[4, length]
  abort "truncated Intel HEX record" unless data&.length == length
  abort "invalid Intel HEX checksum" unless bytes.sum & 0xff == 0

  case type
  when 0
    address = upper + (address_hi << 8) + address_lo
    data.each_with_index do |byte, index|
      offset = address + index - start
      abort "record outside output range" if offset.negative? || offset >= size
      image[offset] = byte
      if offset > max_offset
        max_offset = offset
      end
    end
  when 1
    break
  when 4
    abort "invalid extended address" unless length == 2
    upper = ((data[0] << 8) | data[1]) << 16
  else
    abort "unsupported Intel HEX record type #{type}"
  end
end

# Trim to actual used size
actual_size = max_offset + 1
File.binwrite(output, image[0, actual_size].pack("C*"))
