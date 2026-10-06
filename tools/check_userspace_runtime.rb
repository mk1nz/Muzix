#!/usr/bin/env ruby
# frozen_string_literal: true

# Fail the build if any userspace link reports an undefined global.
#
# This is the one class of mistake on this target that a successful build does
# not rule out, and it costs a restart rather than a diagnostic.
#
# SDCC lowers some operations to calls into a runtime that is named, not
# present.  A 32-bit multiply becomes `call __mullong`; `/` and `%` on unsigned
# values become calls to __divuint and __moduint.  The z80 build of SDCC 4.5
# ships no __mullong at all - it has one for the 6502, the STM8 and the PIC14 -
# so every userspace link line in this tree reported
#
#   ?ASlink-Warning-Undefined Global '__mullong' referenced by module 'libc'
#
# and the linker resolved the name to 0x0000, because an unresolved symbol is a
# definition at zero.  The image is then complete, every size check passes, and
# the instruction at the call site is `call 0x0000` - which is the boot stub.
# Calling that routine restarts the machine.
#
# It was invisible for two reasons.  The link rules filter the linker's output
# for the string "error" and discard warnings, and this line is a warning; and
# the only program that reached one of these calls was apps/top, which reached
# it on its very first display.  So a display that had never worked looked like
# a display that had regressed, and nothing said otherwise.
#
# This gate re-runs each link and fails on the warning itself.  Re-linking is
# cheap and idempotent - the .lk files write their .ihx and .map beside
# themselves, which is where the build already put them - and the alternative is
# the next program that multiplies two 32-bit values being the next program that
# restarts the machine.

require 'open3'

DIRS = ARGV.empty? ? %w[apps shell] : ARGV

# A warning is the finding.  An error is reported too, because a link that
# failed outright would otherwise leave a stale .ihx behind and this gate would
# pass on the previous build's output.
FINDING = /(Undefined Global|ASlink-Error|ASlink-Warning-Unresolved)/

failures = []
links = 0

DIRS.each do |dir|
  Dir.glob(File.join(dir, '*.lk')).sort.each do |lk|
    name = File.basename(lk, '.lk')
    next unless File.file?(lk)

    links += 1
    out, _status = Open3.capture2e(ENV.fetch('SDLDZ80', 'sdldz80'),
                                   '-f', "#{name}.lk", chdir: dir)
    out.each_line do |line|
      failures << "#{dir}/#{name}: #{line.strip}" if line =~ FINDING
    end
  end
end

if failures.empty?
  puts "  x every userspace link resolves: OK (#{links} link scripts)"
  exit 0
end

puts '  x every userspace link resolves: FAILED'
failures.uniq.sort.each { |f| puts "      #{f}" }
puts
puts '  An undefined global in a userspace link is a call to address 0, which is'
puts '  the boot stub: the program restarts the machine instead of returning.'
puts '  Rewrite the expression - a 32-bit multiply has no routine on this target'
puts '  and has to be built out of 16-bit partial products.'
abort 'FAIL: a userspace link has an unresolved runtime call'
