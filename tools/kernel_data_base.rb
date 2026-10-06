#!/usr/bin/env ruby
# frozen_string_literal: true

# Print the _DATA base the kernel link should use.
#
# _DATA has to start above the end of _CODE, and sdldz80 does not enforce that:
# given `-b _DATA` below the real end of _CODE it simply emits both and lets the
# two overlap.  The kernel then calls a function whose body lives in the middle
# of its own variables.  That is not hypothetical - pinning _DATA at 0xEA00 while
# _CODE ran to 0xEA93 put _muzix_set_syscall_entry and
# _muzix_get_syscall_entry at 0xEA8A/0xEA8E, inside _DATA, and calling
# muzix_set_syscall_entry() executed whatever _DATA held there.
#
# A hand-written constant cannot be kept correct: _CODE moves every time a
# module changes size, and the value is only ever right on the day it is typed.
# So the link runs twice - once with a provisional base to find out where _CODE
# really ends, then again with the base derived from that.
#
# Usage:
#   kernel_data_base.rb <kernel.map>          # print the _DATA base to use
#   kernel_data_base.rb --end <kernel.map>    # print the address _CODE ends at

CODE_BASE = 0x0098
MARGIN = 0x40      # room for the next few bytes of code growth
GRANULE = 0x100    # _DATA is page-aligned; costs at most 255 bytes of stack

# THE GRANULE IS NOT THE DISCRIMINATOR - AND THIS NOTE WAS WRONG
# ============================================================
# Written on 2026-10-01: "set GRANULE to 1 and the machine breaks, 3 runs out of
# 3, cause NOT found", with a table blaming the granule.  That conclusion was
# wrong and is kept here only so the wrong reasoning is recognisable.
#
# The fault was muzix_tick_add_process_time() in platform/zeta-v2/tick.s.  It
# restamped through DE AFTER DE had been reused to hold the tick difference, so
# the two bytes of the new stamp were stored at address `elapsed` - a small
# 16-bit number - instead of at *stamp.  With elapsed = 0 the two writes landed
# at 0x0000 and 0xFFFF and nothing happened.  With a live tick elapsed swept
# 0,1,2,... and the routine walked two bytes at a time up window 0, through the
# boot stub, the IM1 vector at 0x0038 and tick.s including its own handler, and
# eventually into _DATA at the tick counter itself, which amplified it.  The
# machine re-entered kernel_main: KM, MNT, SVC, RDY, LOAD, SCHED, the stack
# report, and the shell banner a second time - which is exactly the symptom
# recorded here as a granule problem.
#
# Two isolation results from the day are now explained, and neither had tested
# the granule at all: a bare-`reti` handler froze the tick counter at zero, and
# repointing the handler's `ld hl,#_muzix_tick_count` at 0x2100 froze it too,
# because the routine reads the counter through its own `ld bc,#_muzix_tick_count`
# which that patch does not touch.
#
# With the restamp fixed, both GRANULE settings are clean.  So the honest
# statement is: the granule was never implicated, the dead tick was what hid the
# fault, and the room arithmetic above is real and independent of all this.
# Dropping GRANULE to 1 still buys the room it claims to - whether it is safe to
# do is now unmeasured rather than known-bad, and 411 bytes of room means
# nothing needs it.

MARGIN = 0x40      # room for the next few bytes of code growth
GRANULE = 0x100    # _DATA is page-aligned; costs at most 255 bytes of stack

# DO NOT "FIX" THE SPACE PROBLEM BY DROPPING GRANULE
# =================================================
# The arithmetic above makes GRANULE look like a free 64 bytes: set it to 1 and
# _DATA lands at code_end + MARGIN instead of the next 256-byte boundary, which
# on this tree moves the room for _CODE growth from 23 bytes to 175.
#
#   GRANULE 0x100   _DATA 0xEA00   _CODE may reach 0xE9C0   411 bytes of room
#   GRANULE 1       _DATA 0xE9E9   _CODE may reach 0xEA58   175 bytes of room
#
# That was built and run, and the machine broke deterministically - 3 runs out
# of 3, no flake.  What was measured, on the ROM, sending `date 10010047`:
#
#   GRANULE 0x100, no kernel change      clean
#   GRANULE 1, no kernel change          clean, 3 of 3
#   GRANULE 0x100, plus a kernel change  clean, 2 of 2   (link still fails, but
#                                                          the image runs)
#   GRANULE 1, plus a kernel change      BROKEN, 3 of 3
#
# So it is neither change alone, it is the combination, and it is the granule
# that turns harmless growth into a fault.  Two symptoms, both memory
# corruption and neither with a diagnostic:
#
#   - the userspace shell prints "Goodbye!" and exits.  That is readline()
#     returning 0, which is the TTY reporting end of file, which means a 0x04
#     turned up in the input stream out of nowhere;
#   - or control returns to 0x0098 and the machine boots again: "KM", "MNT",
#     "SVC", "RDY", "LOAD", "SCHED", the stack report, and the shell banner a
#     second time.
#
# What was ruled out, so nobody repeats it: the linker maps are clean, a 23-byte
# shift and no overlap, with s__DATA, l__DATA, s__INITIALIZED and s__GSFINAL all
# moving together; muzix_zeta_syscall_runtime_t is built from named members and
# nothing addresses the tty member by offset; muzix_zeta_ldir_staged() takes an
# explicit source, destination and length and has no alignment requirement; and
# the only references to 0xEA00 in the tree are the comment here and the
# provisional first-pass -b _DATA in the Makefile, which is overwritten by the
# value this script prints on the second pass.
#
# The cause was NOT found.  What is established is enough to act on: the
# granule is load-bearing in a way that is not visible in this file, and the
# room a change needs has to come out of _CODE or out of _DATA, not out of
# moving the base.  When the next change does not fit, the honest options are to
# find the bytes in _CODE or to move the check one level up the way
# set_rtc()'s range check was moved into lib/libc.c - not to change this line.

path = ARGV.last or abort 'usage: kernel_data_base.rb [--end] <kernel.map>'
want_end = ARGV.size > 1 && ARGV[0] == '--end'
map = File.read(path)

# l__CODE comes from the .ABS. block and is the area extent sdld actually
# emitted, so it is the number that matters here - not any symbol address.
m = map.match(/^\s+[0-9A-Fa-f]{4,8}\s+l__CODE\s*$/)
code_len = m && m[0][/([0-9A-Fa-f]+)/, 1].to_i(16)

unless code_len && code_len > 0
  abort "FAIL: could not read l__CODE from #{path}"
end

code_end = CODE_BASE + code_len

if want_end
  puts format('0x%04X', code_end)
  exit
end

base = (((code_end + MARGIN + GRANULE - 1) / GRANULE) * GRANULE)
puts format('0x%04X', base)
