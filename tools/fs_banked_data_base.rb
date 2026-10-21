#!/usr/bin/env ruby
# frozen_string_literal: true

# Print the _DATA base the banked filesystem module link should
# use, and refuse a module that no longer fits its page.
#
# The module is linked the way the kernel is: two passes, the
# first with a provisional _DATA to learn where the module's
# _CODE really ends, the second with the base derived from
# that.  sdldz80 does not enforce _CODE/_DATA separation - it
# emits both and lets them overlap - so the derived base is
# what keeps the module's code from being its own data.
#
# The kernel's twin (tools/kernel_data_base.rb) only derives a
# base.  This one also checks the fit, because a banked module
# has a hard limit the kernel does not: the whole image is one
# 16 KiB page.  _CODE starts at 0x4100 (0x4000 is the
# FS_ENTRY stub's reserved page-prefix), so _DATA and
# everything above it has 0x8000 - 0x4100 bytes to fit in.
# A module that outgrows that is a build failure here, not a
# silent corruption on the board: the trampoline maps the page
# into window 1 and whatever lies past 0x7FFF is the next
# window, which is kernel code.
#
# Usage:
#   fs_banked_data_base.rb <module.map>          # print the _DATA base
#   fs_banked_data_base.rb --end <module.map>    # print the _CODE end

CODE_BASE = 0x4100
PAGE_END = 0x8000
MARGIN = 0x40
GRANULE = 0x100

path = ARGV.last or abort 'usage: fs_banked_data_base.rb [--end] <module.map>'
want_end = ARGV.size > 1 && ARGV[0] == '--end'
map = File.read(path)

# l__CODE is the area extent sdld actually emitted.
m = map.match(/^\s+[0-9A-Fa-f]{4,8}\s+l__CODE\s*$/)
code_len = m && m[0][/([0-9A-Fa-f]+)/, 1].to_i(16)

unless code_len && code_len > 0
  abort "FAIL: could not read l__CODE from #{path}"
end

code_end = CODE_BASE + code_len

if want_end
  puts format('0x%04X', code_end)
  exit 0
end

# l__DATA is the data extent; the fit check needs it even on
# the base-printing path, because a module with no data at all
# is the common case and must still be checked against the
# page end.
m = map.match(/^\s+[0-9A-Fa-f]{4,8}\s+l__DATA\s*$/)
data_len = m && m[0][/([0-9A-Fa-f]+)/, 1].to_i(16)
data_len = 0 unless data_len

base = (((code_end + MARGIN + GRANULE - 1) / GRANULE) * GRANULE)

if base + data_len > PAGE_END
  abort format(
    'FAIL: banked module does not fit its page: _CODE ends at ' \
    '0x%04X, _DATA would be 0x%04X..0x%04X, page ends at 0x%04X',
    code_end, base, base + data_len, PAGE_END
  )
end

puts format('0x%04X', base)
