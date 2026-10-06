#!/usr/bin/env ruby
# frozen_string_literal: true

# Every source the emulator build compiles must be tracked, and must not be
# matched by an ignore rule.
#
# Why this exists: emulator/.gitignore listed `zeta_sbc_v2` as though it were a
# build product, which untracked the whole Zeta SBC V2 model - including
# emu_ds1302.c, which the Makefile compiles and which the DS1302 driver and the
# clock depend on.  A source the build compiles and the repository does not have
# is a source the next checkout does not build, and it is worse than that: a fix
# to it sits uncommitted and nothing reports it.  That is not hypothetical.  The
# calendar-carry fix landed in 6ae6a3a, in a file git was not tracking, and had
# to be caught by hand.
#
# The check reads the Makefile's own answer rather than re-parsing the build:
# `make -C emulator info` prints the resolved SRCS list.  Re-deriving it here
# would be a second, drifting copy of the truth.
#
# The ignore check is deliberately separate from the tracked check.
# `git check-ignore` skips files git already tracks, so a path that is tracked
# AND matched by an ignore rule looks fine until the file is ever edited as a new
# file - at which point the edit is ignored and the file silently stops being
# part of the repository.  --no-index asks about the path itself.  That is
# exactly the state apps/date.lk was in: force-added, so it built, and matched
# by a rule, so the next linker script would have been lost.

require 'open3'

ROOT = File.expand_path('..', __dir__)
EMULATOR = File.join(ROOT, 'emulator')

def git(*args)
  out, status = Open3.capture2e('git', *args, chdir: ROOT)
  [out.strip, status.success?]
end

# A source tarball has no .git and still has to build the emulator.  Presence
# is checked either way; the tracking assertions only apply inside a work tree,
# and the output says which of the two ran rather than passing quietly.
_, in_repo = git('rev-parse', '--is-inside-work-tree')

out, ok = Open3.capture2e('make', '-C', EMULATOR, '--no-print-directory', 'info')
unless ok.success?
  abort "FAIL: could not ask emulator/Makefile what it compiles:\n#{out}"
end

srcs_line = out.lines.find { |l| l.start_with?('Source files:') }
unless srcs_line
  abort 'FAIL: `make -C emulator info` did not print a "Source files:" line; ' \
        'this check has nothing to verify against'
end

sources = srcs_line.sub('Source files:', '').split(/\s+/).reject(&:empty?)

# Headers are not in SRCS, but a compiled source whose header is untracked does
# not compile from a fresh checkout either.  Ask the preprocessor rather than
# globbing, so a header reached through zeta_sbc_v2/ counts and one that is only
# textually mentioned does not.  The Makefile passes -Iz80pack -Iz80 to the
# sources it builds from those directories, so the same two are used here.
#
# The .d files the Makefile includes do not help here: nothing passes -MMD, so
# they do not exist, and a check that reports "0 headers" because it looked in
# an empty place is a check that verifies nothing.
headers = sources.flat_map do |src|
  out, ok = Open3.capture2e(ENV.fetch('HOST_CC', 'gcc'), '-MM',
                           '-I' + File.join(EMULATOR, 'z80pack'),
                           '-I' + File.join(EMULATOR, 'z80'),
                           File.join(EMULATOR, src), chdir: EMULATOR)
  unless ok.success?
    abort "FAIL: could not resolve the headers of emulator/#{src}:\n#{out}"
  end
  # Everything after the leading "target.o:" is a header path.  The line
  # continuations are backslashes, and given an absolute source path gcc prints
  # absolute header paths, so both are normalised back to emulator-relative.
  out.sub(/\A[^\s:]+:/, '').split(/\s+/).reject { |t| t.empty? || t == '\\' }
     .map { |t| t.sub(/\A#{Regexp.escape(EMULATOR)}\//, '').sub(%r{\A\./}, '') }
     .uniq
end

# Keep only headers that are real project files: a system header resolved
# outside the emulator directory is not something this repository can lose.
headers = headers.select do |h|
  h.end_with?('.h') && !h.start_with?('/') && File.file?(File.join(EMULATOR, h))
end

errors = []

(sources + headers).uniq.sort.each do |rel|
  full = File.join(EMULATOR, rel)

  unless File.file?(full)
    errors << "emulator/#{rel}: the Makefile compiles it but it is not on disk"
    next
  end

  next unless in_repo

  _, tracked = git('ls-files', '--error-unmatch', "emulator/#{rel}")
  unless tracked
    errors << "emulator/#{rel}: compiled by emulator/Makefile but NOT tracked " \
              'by git - a fresh checkout would not have it and could not build'
  end

  _, ignored = git('check-ignore', '--no-index', '-q', "emulator/#{rel}")
  next unless ignored
  errors << "emulator/#{rel}: compiled by emulator/Makefile and matched by a " \
            'gitignore rule, so it is invisible to the repository - ' \
            'un-exclude it in emulator/.gitignore rather than adding it with -f'
end

if errors.any?
  errors.each { |e| puts "  x #{e}" }
  abort 'FAIL: the emulator build depends on files the repository does not have'
end

if in_repo
  puts "  emulator sources: %d compiled, %d headers, all tracked and un-ignored" %
       [sources.size, headers.size]
  puts '  x every source emulator/Makefile compiles is tracked: OK'
else
  puts "  emulator sources: %d compiled, %d headers, all present" %
       [sources.size, headers.size]
  puts '  (not a git work tree: presence checked, tracking not applicable)'
end
