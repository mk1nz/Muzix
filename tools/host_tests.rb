#!/usr/bin/env ruby
# frozen_string_literal: true

# Run the tree's test_*.c sources on the host compiler, and actually run them.
#
# Why this exists
# ---------------
# The tree has carried thirty-odd test_*.c files for its whole life and not one
# of them has ever executed.  run_all_tests.sh reported each linked .ihx as
# "BUILT" and started nothing, so the suite read like coverage and contained
# none: a test that cannot fail also cannot pass.  This script is the
# replacement - it compiles the portable modules with the host compiler, links
# each test against the module(s) it actually calls, runs the resulting binary
# and fails on a non-zero exit.
#
# How a test finds its module
# ---------------------------
# By the linker, not by a list.  A hand-written table of "test_x needs x.c" is
# one more thing to forget to update, and a test added without its entry would
# be reported as broken rather than picked up.  Instead every module is
# compiled, nm is asked which globals each one defines, and each test is linked
# starting from nothing: whatever symbol the linker reports undefined is
# matched against the modules that define it and those modules are added, until
# the link succeeds or a symbol no module provides is left over.  That is the
# same closure a linker computes, so adding test_foo.c is enough - it is found
# by the glob and it finds its own module.
#
# What is not a failure
# ---------------------
# A test that does not compile, does not link, or does not terminate is
# reported, counted and named - never silently skipped, and never counted as a
# pass.  Those tests are coverage that does not exist yet, and the only honest
# way to say so is to print them on every run.  A test that builds, runs and
# returns non-zero IS a failure and fails the build: that is a real disagreement
# between a test and the code, and it is the whole point.
#
# The exception is KNOWN_FAILING below, and it is a list of names rather than a
# list of files to ignore: those tests still build, still link and still RUN on
# every gate.  Their failure is recorded, attributed to the reason beside them and
# printed, and the build goes on.  A test that has never run and fails is not
# evidence that the code is wrong - several of these were written against an
# arrangement the code has since deliberately moved away from - but neither is it
# evidence that the test is right, so it is not a pass either.  Each entry says
# which assertion fails and what has been established about it.  Deleting an
# entry is how a test starts gating again.
#
# Usage:
#   host_tests.rb <project-root> [--timeout SECONDS] [--verbose]

require 'fileutils'
require 'io/wait'
require 'open3'

ROOT = File.expand_path(ARGV[0] || '.')

timeout_index = ARGV.index('--timeout')
# Ten seconds, not the instant a hang is obvious: the budget has to be long
# enough that a slow machine does not report a hang, and short enough that a
# hung gate is a nuisance rather than a coffee break.  No test in this tree
# takes a second on the host - they are pure arithmetic over a few hundred
# bytes - so 10 is generous by two orders of magnitude.
TIMEOUT = (timeout_index && ARGV[timeout_index + 1]) ? ARGV[timeout_index + 1].to_i : 10
VERBOSE = ARGV.include?('--verbose')

# The directories the Makefile draws kernel modules from, plus the host stub
# seam and the emulator's DS1302 model.  lib/ is deliberately absent: those are
# userspace modules (libc, the syscall wrappers, the ROMFS blob), linked at
# _CODE=0x8000 for a Z80 image, and the host has its own C library - linking
# lib/libc.o beside the system libc duplicates every string function.  Their
# headers are still on the include path, because kernel modules include
# lib/tty_ioctl.h.
#
# emulator/ is in for one file only, and the filter below says which: the DS1302
# chip model.  platform/zeta-v2/test_rtc_ds1302.c compiles against it so that
# the host test and the emulator agree about the wire bit for bit, rather than
# there being a second chip model that can agree with the driver by
# construction.  The rest of the emulator is a machine, not a module, and its
# test_*.c files are unit tests of its own memory model built by emulator/Makefile.
SOURCE_DIRS = %w[kernel fs mm pm platform/zeta-v2 test_host emulator lib].freeze

# lib/ joins the pool for lib/syscall.c ALONE, and the two exclusions below are
# why.
#
# lib/syscall.c is the syscall trampoline, not libc: SYSCALL() calls
# muzix_syscall_void(), which dispatches through the entry pointer the platform
# installs, so it is exactly as testable on a host as anything else here - and
# platform/zeta-v2/test_syscall_runtime.c depends on it, because what that test
# asserts is that binding the runtime makes the entry reachable and resetting it
# makes the entry answer -1 again.  A test that provided its own sys_getpid
# would pass while proving nothing.
#
# lib/libc.c is excluded because it defines memcpy, strlen and the rest against
# the host's own C library, and two definitions of those do not link.
# lib/romfs_data.c is excluded because it is a filesystem blob, not code, and
# nothing in the host gate asks for its contents.

# The only emulator source in the pool, and the include flags its neighbours
# need.  These are per-source rather than global because emulator.h pulls in the
# Z80 core and there is no reason for every kernel module's compile line to see
# them.
EXTRA_SOURCES = {
  'emulator/zeta_sbc_v2/emu_ds1302.c' => %w[-Iemulator -Iemulator/zeta_sbc_v2],
  'test_host/ds1302_host.c' => %w[-Iemulator -Iemulator/zeta_sbc_v2],
  # lib/syscall.h reaches upwards from lib/, so the -I is one level different
  # from every other entry here.
  'test_host/syscall_host.c' => %w[-Ilib]
}.freeze

# The same include list the Makefile uses for its -MM dependency scan.  gcc is
# not being used to build the kernel - only to resolve #include - so the fact
# that these are SDCC sources is irrelevant.
INCLUDES = %w[
  -I. -Ikernel -Ifs -Imm -Ipm -Iplatform/zeta-v2
  -Iplatform/zeta-v2/test_kernel -Ilib -Iapps -Ishell -Itools -Itest_host
].freeze

# -DMUZIX_TTY_CALLBACKS=1 on every host test binary, and why:
#
# fs/tty_device.h says the device dispatches through the struct's function
# pointers only when this is set, that the kernel must build with it 0, and
# that "a test binary can build with -DMUZIX_TTY_CALLBACKS=1 and drive the
# device with its own callbacks".  On the host that is not merely permitted, it
# is the only workable form: the 0 path calls the Zeta UART primitives in
# uart_io.s, which are `in`/`out` against $68-$6D and mean nothing off the
# board, so muzix_tty_input_pending() would report "nothing pending" for a byte
# the test had just queued and test_tty_pending() would report a failure that is
# an artefact of the host, not a defect.
#
# The indirect call that form needs is also exactly what SDCC 4.5 with this
# project's peephole file cannot emit - that is why the kernel is built with 0.
# Nothing here touches the ROM: KERNEL_CFLAGS does not set it.
#
# -DMUZIX_ENABLE_FORMAT=1 for the same reason and with the same carve-out.  A
# booted kernel mounts the read-only ROM image, so muzix_fs_volume_format() and
# muzix_fs_superblock_format() are compiled out of it (fs/volume.c:22,
# fs/superblock.c:9) - nothing in a running system can use them.  The fs tests do
# use them, so a test build needs them back.  This is what build_target.sh
# passed when the Z80 test binaries were built, which is where both flags come
# from.
HOST_DEFINES = %w[-DMUZIX_TTY_CALLBACKS=1 -DMUZIX_ENABLE_FORMAT=1].freeze

CC = ENV['HOST_CC'] || 'cc'
CC_ENV = { 'CC' => CC }

BUILD = File.join(ROOT, 'build', 'hosttests')

def rel(path)
  path.sub("#{ROOT}/", '')
end

def objname(path)
  path.tr('/', '_').sub(/\.c\z/, '.o')
end

def glob(pattern)
  Dir.glob(pattern, base: ROOT)
end

# Tests that run and fail, with what has been established about each.  They are
# executed on every run; this table changes what the run means, not whether it
# happens.
#
# All six are tests written against an arrangement the code has since moved away
# from deliberately, and none of them has ever been executed - which is how a
# test written in 2025 came to describe a design retired in 2026.  Each entry
# names the assertion that fails and the commit or the comment that retired the
# behaviour it expects, so the entry can be closed by fixing the test rather than
# by arguing about it.
#
# fs/test_file_io.c was the seventh name here and the only one nobody had looked
# at: its case 5 asserted a one-direct-zone inode, so a write at offset 512
# resolved to zone[1] instead of the indirect block.  MUZIX_FS_DIRECT_ZONES is 7
# - MINIX 1.0's NR_DZONE_NUM - and always has been, so the refusal was correct
# and the test was stale.  It has been moved to the documented arrangement, cases
# were added for the boundary-spanning and allocating indirect paths that nothing
# covered, and the reasoning is written down at the top of that file.  It is not
# back in this table because it now passes; do not re-add it without reading that
# comment first.
# Tests that run, fail, and are not counted as passes.  Each entry needs the
# measured reason it is stale, or the reason it is a real defect and what the
# expected result should be - never just a name.
#
# It is empty, and the six names that used to be in it were deleted rather than
# tolerated: each asserted behaviour the code had deliberately moved away from,
# and a permanently red gate reads as a broken build and teaches people to ignore
# the runner.  What each one asserted, and what was given up, is recorded in
# docs/deleted-tests.md.
KNOWN_FAILING = {}.freeze

# The first line that says why, for the report.  A wall of warnings is noise.
def first_error(text)
  line = text.to_s.lines.find { |l| l =~ /error/ }
  line ? line.strip : 'compilation failed'
end

# ---------------------------------------------------------------- sources ---

test_sources = SOURCE_DIRS.flat_map { |d| glob("#{d}/**/test_*.c") }
                           .reject { |p| rel(p).start_with?('test_host/') }
                           .reject { |p| rel(p).start_with?('emulator/') }.sort

# lib/libc.c and lib/romfs_data.c: see the note on SOURCE_DIRS.
HOST_POOL_EXCLUDED = ['lib/libc.c', 'lib/romfs_data.c'].freeze

module_sources = SOURCE_DIRS.flat_map { |d| glob("#{d}/**/*.c") }
                             .reject { |p| File.basename(p) =~ /\Atest_/ }
                             .reject { |p| HOST_POOL_EXCLUDED.include?(rel(p)) }
                             .select { |p| !rel(p).start_with?('emulator/') ||
                                         EXTRA_SOURCES.key?(rel(p)) }.sort

# Deterministic pool order, taken from the Makefile's link order so that the
# host link and the ROM link agree about who owns a global where two modules
# define the same one.
link_order = File.read(File.join(ROOT, 'Makefile'), encoding: 'UTF-8')
                     .split('KERNEL_LINK_ORDER').to_a.last.to_s.split("\n\n").first.to_s
                     .scan(/(\w+)\.rel/).flatten
module_sources.sort_by! do |p|
  base = File.basename(p, '.c')
  [link_order.index(base) || link_order.size, p]
end

# The host replacements for Z80 assembly primitives.  They are in every link.
STUBS = module_sources.select { |p| rel(p).start_with?('test_host/') }

# ---------------------------------------------------------------- compile ---

puts 'host tests: compiling modules with the host compiler'
FileUtils.rm_rf(BUILD)
FileUtils.mkdir_p(BUILD)

# Every module this runner can use has to be in git, and the failure mode when
# one is not is silent and total: .gitignore once listed a source directory as
# a build product, so a model the Makefile compiles was untracked and a fix to
# it could sit in the working tree with nothing reporting it.  The same thing
# happens here if test_host/z80_io_host.c is untracked - z80_outb goes
# undefined and every test that reaches the bank model is reported as not
# linkable, which reads like a coverage gap rather than like a mistake.
tracked = begin
  require 'set'
  Set.new(Open3.capture2('git', 'ls-files', '-z', chdir: ROOT).first.split("\x0"))
rescue StandardError
  nil
end
unless tracked
  warn 'host tests: git not available, skipping the tracked-source check'
else
  untracked = (module_sources + test_sources).reject { |p| tracked.include?(p) }
  unless untracked.empty?
    untracked.each { |p| warn "host tests: #{rel(p)} is not tracked by git" }
    $stdout.flush
    abort 'host tests: a module the host runner needs is not in the repository'
  end
end

def compile(path)
  Open3.capture2e(CC_ENV, CC, '-std=c99', '-w', *INCLUDES, *HOST_DEFINES,
                  *EXTRA_SOURCES.fetch(rel(path), []),
                  '-c', path, '-o', File.join(BUILD, objname(path)))
    .then { |out, status| [status.success?, out] }
rescue StandardError => e
  [false, e.message]
end

def defined_globals(obj)
  out, = Open3.capture2e('nm', '-g', '--defined-only', obj)
  out.scan(/^[0-9a-f]* [A-Za-z] (\S+)$/).flatten.map { |s| s.sub(/\A_/, '') }
end

MODULES = {}          # source => [globals it defines] or nil
module_errors = {}    # source => why it would not compile
module_sources.each do |src|
  ok, err = compile(src)
  if ok
    MODULES[src] = defined_globals(File.join(BUILD, objname(src)))
  else
    MODULES[src] = nil
    module_errors[src] = first_error(err)
  end
end

POOL = MODULES.reject { |_p, defs| defs.nil? }.keys

puts format("  %d of %d modules compile on the host", POOL.size, MODULES.size)

# A module the host cannot build is a module no host test can cover, so the
# reason is printed rather than left in a log nobody opens.  In this tree they
# are the modules with inline Z80 assembly and the ones that rely on implicit
# function declarations, which SDCC allows and C99 does not.
# A module the host cannot build is a module no host test can cover.  Printing the
# reason is not enough, because a build failure is silent about what it COSTS: when
# kernel/kernel_loop.c stopped compiling on the host on 2026-10-04 - an SDCC
# `__asm` block, which is not valid host C - this line printed a tidy explanation
# and the gate went on to report PASS.  Sixteen tests that had been running every
# build had quietly stopped, because every one of them links kernel_loop.c, and
# "a test that did not run" is not a failing test.
#
# So the rule is: a pool module that does not compile must say why, in its own
# source, in the same host-test-not-portable declaration a test uses.  An
# undeclared build failure is a build failure.  That is what stops a portability
# accident from reading as a portability statement.
UNPORTABLE_UNDECLARED = MODULES.select { |_p, defs| defs.nil? }
                               .reject { |p, _| File.read(p).include?('host-test-not-portable') }

UNPORTABLE_UNDECLARED.keys.sort.each do |p|
  puts format('  x not portable, UNDECLARED: %-38s %s', rel(p), module_errors[p])
end

# ------------------------------------------------------------------- run ---

# A test binary that does not terminate has to be killed from outside: the
# canonical wait in muzix_tty_read() is a C loop with no interrupt check, so
# nothing running inside the process can stop it.  fs/test_tty_device.c is a
# known case - its own comment says so - it runs a first phase, then queues
# five bytes with no newline and calls muzix_tty_read(), which on a line
# buffered console waits for a line that never comes.
def run(exe)
  r, w = IO.pipe
  pid = spawn(CC_ENV, exe, out: w, err: w)
  w.close
  unless r.wait_readable(TIMEOUT)
    Process.kill('KILL', pid)
    Process.wait(pid)
    r.read(4096)
    return :timeout
  end
  Process.wait(pid)
  output = r.read(65_536).to_s
  puts "      #{output.strip}" unless output.strip.empty? || VERBOSE
  # A child killed by a signal has no exit status at all: $?.exitstatus is nil,
  # and nil.to_i is 0.  So `status == 0` used to mean "passed" for every
  # segfault, which is worse than the fabricated coverage this script replaced -
  # it would have reported the SETRTC test passing while it was faulting, and
  # nothing in the output would have said otherwise.  A signal is a failure and is
  # named, because a test that cannot run has not passed anything.
  return "signal #{$?.termsig} (#{Signal.signame($?.termsig) rescue 'unknown'})" if $?.signaled?
  $?.exitstatus.to_i
end

# macOS ld prints `  "_sym", referenced from:`; GNU ld prints
# `undefined reference to `sym'`.  Both put the name in a quote pair.  One
# leading underscore is stripped from both sides of every comparison: nm on
# Mach-O prefixes C names with one, ELF does not, and normalising keeps the
# lookup correct on either host.
def undefined_symbols(text)
  text.scan(/["`]([A-Za-z_][A-Za-z0-9_]*)["`,]/).flatten.uniq
      .map { |s| s.sub(/\A_/, '') }.reject { |s| s == 'ld' }
end

def link(test_obj, selected)
  objs = selected.map { |src| File.join(BUILD, objname(src)) }
  Open3.capture2e(CC_ENV, CC, '-std=c99', test_obj, *objs, '-o', File.join(BUILD, 'a.out'))
    .then { |out, status| [status.success?, out] }
end

# Whether `candidate` defines anything `selected` already defines.  sdldz80
# resolves a duplicate global to the first definition in link order and says
# nothing about it; the host linker refuses to link at all.  Filtering the
# overlap here is what keeps a test that reaches two modules sharing a global
# from being reported as unlinkable.
def overlaps?(candidate, selected)
  mine = MODULES[candidate]
  selected.any? { |s| !MODULES[s].nil? && !(mine & MODULES[s]).empty? }
end

def resolve(test)
  name = rel(test)
  source = File.read(test)

  # A test can say it is not host-portable, in its own words, and be believed:
  #   /* host-test-not-portable: why the Z80 makes this unrunnable here */
  # The reason is printed with the test on every run rather than being inferred
  # here, because the only thing that can state it accurately is whoever knows
  # why the test needs the target.
  not_portable = source[/host-test-not-portable:\s*(.*?)\*\//m, 1]
  return [:not_built, name, "not portable by its own declaration: " \
                           "#{not_portable.gsub(/\s*\*\s*/, ' ').gsub(/\s+/, ' ').strip}",
          nil] if not_portable

  # A test may stand on its own seam, by naming the symbols it provides itself.
  # Those names are dropped from the pool for this test only, so the test's own
  # definition is the one that links; if it does not actually define one, the
  # link fails and is reported as such.  Nothing is shadowed implicitly.
  provides = source[/host-test-provides:\s*([^\n*]*)/, 1].to_s.split(/\s+/).reject(&:empty?)
  pool = if provides.empty?
           POOL
         else
           POOL.select { |m| (MODULES[m] & provides).empty? }
         end

  ok, err = compile(test)
  return [:not_built, name, first_error(err), nil] unless ok
  test_obj = File.join(BUILD, objname(test))

  selected = STUBS.dup
  16.times do
    linked, lerr = link(test_obj, selected)
    if linked
      status = run(File.join(BUILD, 'a.out'))
      case status
      when :timeout then return [:timeout, name, "no exit within #{TIMEOUT}s", selected]
      when 0        then return [:pass, name, nil, selected]
      else
        # A test can only be known-failing if it is named in KNOWN_FAILING.  An
        # unnamed failure is a failure, and one that stops being a failure by
        # being added to that table is the mechanism this file exists to prevent.
        detail = status.to_s.start_with?('signal') ? status : "exit status #{status}"
        return [:fail, name, detail, selected] if KNOWN_FAILING[name].nil?
        return [:known, name, KNOWN_FAILING[name], selected]
      end
    end

    # One module per unresolved symbol, and a module that would redefine a
    # global an already-chosen module defines is skipped rather than added:
    # sdldz80 resolves a duplicate to the first definition in link order and
    # says nothing, while the host linker refuses to link at all.  `pending` is
    # the selection so far *including* this round's choices, so two symbols
    # owned by the same module add it once.
    pending = selected.dup
    undefined_symbols(lerr).sort.each do |sym|
      owner = pool.find { |m| MODULES[m].include?(sym) }
      next if owner.nil? || pending.include?(owner) || overlaps?(owner, pending)
      pending << owner
    end
    added = pending - selected

    if added.empty?
      orphans = undefined_symbols(lerr).reject { |s| MODULES.any? { |_m, d| d&.include?(s) } }
      why = orphans.empty? ? first_error(lerr) : "needs #{orphans.sort.join(' ')} " \
                                                 'which no host-compilable module provides'
      return [:not_built, name, "link: #{why}", selected]
    end

    selected += added
  end
  [:not_built, name, 'link did not close after 16 modules', selected]
end

results = test_sources.map { |t| resolve(t) }

# ---------------------------------------------------------------- report ---

order = { pass: 0, fail: 1, known: 2, timeout: 3, not_built: 4 }
results.sort_by! { |r| [order[r[0]], r[1]] }

verb = { pass: 'PASS', fail: 'FAIL', known: 'KNOWN-FAIL', timeout: 'TIMEOUT',
         not_built: 'NOT BUILT' }
results.each do |kind, name, why, selected|
  # HOST_TEST_LINK=1 prints the module set for a failing or quarantined test as
  # well as a passing one, which is what you want when a test segfaults and the
  # question is which of forty-odd modules it managed to reach.
  detail = if selected.nil?
             why
           elsif kind == :pass || ENV['HOST_TEST_LINK']
             selected.map { |m| rel(m) }.join(' ')
           else
             why
           end
  puts format('  %-10s %-46s %s', verb[kind], name, detail)
end

executed = results.count { |r| %i[pass fail known].include?(r[0]) }
passed   = results.count { |r| r[0] == :pass }
failed   = results.count { |r| r[0] == :fail }
known    = results.count { |r| r[0] == :known }
timedout = results.count { |r| r[0] == :timeout }
absent   = results.count { |r| r[0] == :not_built }

puts format('  host tests: %d sources, %d executed, %d passed, %d failed, ' \
            '%d known-failing, %d timed out, %d not executed',
            results.size, executed, passed, failed, known, timedout, absent)

if known.positive?
  puts format('  KNOWN FAILING (%d), executed every run and not counted as passes - ' \
              'see KNOWN_FAILING in tools/host_tests.rb for what each asserts and ' \
              'why it is stale: %s', known,
              results.select { |r| r[0] == :known }.map { |g| rel(g[1]) }.join(' '))
end

if timedout.positive? || absent.positive?
  gaps = results.select { |r| %i[timeout not_built].include?(r[0]) }
  puts format('  NOT COVERAGE (%d): %s', gaps.size, gaps.map { |g| rel(g[1]) }.join(' '))
end

$stdout.flush
if UNPORTABLE_UNDECLARED.any?
  abort format('FAIL: %d pool module(s) stopped compiling on the host without saying ' \
               'why: %s. Every host test that links one of these has stopped running, ' \
               'and that is not a failing test. Either add a ' \
               'host-test-not-portable comment to the module, or fix it.',
               UNPORTABLE_UNDECLARED.size,
               UNPORTABLE_UNDECLARED.keys.map { |p| rel(p) }.join(' '))
end

abort "FAIL: #{failed} host test(s) failed" if failed.positive?
puts '  x every host test that ran and is not known-failing passed'
