#!/usr/bin/env ruby
# frozen_string_literal: true

# tick_z80.rb - build platform/zeta-v2/tick.s as an image and run it.
#
# WHAT IT IS FOR
# --------------
# platform/zeta-v2/test_tick.c exercises a C model of the tick module:
# test_host/z80_io_host.c's muzix_host_tick is a uint16_t, so there is one byte
# order, no register file and no multi-byte object in the model at all.  That
# model passed every case while tick.s itself had three readers that took the
# low byte of the counter first, read the other half out of window 0, and added
# the high half of a difference into the low half of the counter.  A C model
# cannot see any of it, because none of those mistakes exist in C.
#
# So the routine is tested here as what it is: assembled, linked and executed.
# The three pieces are deliberately not in the Makefile.
#
#   * sdasz80 and sdldz80 are the kernel toolchain, and this script's whole
#     subject is a .s file, so it uses them the way the ROM build does.
#   * The runner is tools/tick_z80_run.c and the harness is
#     tools/tick_z80_harness.s, because they belong to the rig rather than to
#     the module: nothing here is linked into the ROM, and the kernel's module
#     list is explicit, so a second .s in platform/zeta-v2/ would be a question
#     with no answer.
#   * The core is emulator/z80/z80.c, the tree's own, compiled here with the
#     host compiler.  Using the tree's core rather than a vendored copy means
#     the image this gate reports on is the same machine the ROM is booted on.
#
# WHY IT IS A TOOL AND NOT A NEW MAKEFILE TARGET
# ----------------------------------------------
# AGENTS.md does not allow new top-level targets, and tools/ is where the tree's
# measurement gates live.  The one thing it must not become is a gate that can
# pass by not running: if either assembler is missing, or the link fails, or the
# harness never halts, this exits non-zero with the reason on stderr.  A check
# that cannot find its subject does not get to report success - see
# tools/check_z80_call_convention.rb, which fails the build when its probe stops
# compiling for exactly that reason.
#
# It is invoked from platform/zeta-v2/test_tick.c rather than from the Makefile
# so that it runs through host_tests, which is the tree's only runner that
# actually executes anything, and so that the test named after the module is the
# one that covers it.
#
# Usage: tick_z80.rb <repo-root>

require 'fileutils'
require 'open3'

ROOT = File.expand_path(ARGV[0] || '.')
WORK = File.join(ROOT, 'build', 'tick_z80')
CC   = ENV['HOST_CC'] || 'cc'

# 0x8000 and 0x9000 put the image above window 0, so a read of window 0 by the
# counter reader is visible to the runner as a data access rather than as an
# instruction fetch.  That is the measurement that makes the byte-order fault
# detectable at all; see tools/tick_z80_run.c.
CODE = '0x8000'
DATA = '0x9000'

def fail(what, detail = nil)
  warn "tick_z80: #{what}"
  warn detail.to_s.strip unless detail.nil? || detail.to_s.strip.empty?
  warn 'tick_z80: the real tick.s was not run, so nothing here is a pass.'
  exit 1
end

FileUtils.rm_rf(WORK)
FileUtils.mkdir_p(WORK)

# ---- assemble ------------------------------------------------------------
{
  'tick.rel'    => File.join(ROOT, 'platform/zeta-v2/tick.s'),
  'harness.rel' => File.join(ROOT, 'tools/tick_z80_harness.s'),
}.each do |out, src|
  o, s = Open3.capture2e(ENV.fetch('SDASZ80', 'sdasz80'),
                         '-o', out, src, chdir: WORK)
  fail("sdasz80 could not assemble #{src.sub("#{ROOT}/", '')}", o) unless s.success?
end

# ---- link ----------------------------------------------------------------
# -mjwx writes t.map beside the image, and the runner reads the harness's
# addresses out of it rather than out of constants, so the two cannot disagree.
o, s = Open3.capture2e(ENV.fetch('SDLDZ80', 'sdldz80'), '-mjwx', '-i', 't.ihx',
                       '-b', "_CODE=#{CODE}", '-b', "_DATA=#{DATA}",
                       'tick.rel', 'harness.rel', chdir: WORK)
fail('sdldz80 could not link tick.s with the harness', o) unless s.success?

# ---- build the runner ----------------------------------------------------
o, s = Open3.capture2e(CC, '-std=c99', '-O1', '-w',
                       "-I#{File.join(ROOT, 'emulator/z80')}",
                       File.join(ROOT, 'tools/tick_z80_run.c'),
                       File.join(ROOT, 'emulator/z80/z80.c'),
                       '-o', File.join(WORK, 'tick_z80_run'))
fail('the tick_z80 runner would not build', o) unless s.success?

# ---- run it --------------------------------------------------------------
o, s = Open3.capture2e(File.join(WORK, 'tick_z80_run'),
                       File.join(WORK, 't.ihx'), File.join(WORK, 't.map'))
print o
exit(s.success? ? 0 : 1)