#!/usr/bin/env ruby
# frozen_string_literal: true

# Every linker script the build needs must be present, and must be tracked.
#
# Why this exists: the root .gitignore listed `*.lk` under "SDCC and assembler
# outputs", which is true of no file in this tree.  Measured by cleaning and
# rebuilding and comparing the set of .lk files before and after: the build
# emits none.  All seven are hand-written inputs, and an app's link rule names
# one as a prerequisite - `apps/date.ihx: apps/date.rel apps/date.lk ...` - so
# with the script untracked there is nowhere to put the code and no way to link
# it.  One of the seven had been force-added, which hid the problem: the file was
# present, so the build worked, and the other six looked fine too.
#
# The failure mode this gate exists to stop is silence.  A missing .lk does not
# fail loudly; sdldz80 is never reached, or links without the script's memory
# layout, and the symptom is a kernel that boots and then misbehaves.  Nothing
# in the build notices, so nothing in the build should be relied on to notice.

require 'open3'

ROOT = File.expand_path('..', __dir__)
MAKEFILE = File.join(ROOT, 'Makefile')

errors = []
seen = {}

def git(*args)
  out, status = Open3.capture2e('git', *args, chdir: ROOT)
  [out.strip, status.success?]
end

# Whether this checkout is a git work tree at all.  A source tarball has no
# .git, and a tarball has to build: the point of the tracking half of this gate
# is to protect the repository, and there is no repository to protect outside
# one.  Presence is still checked there; only the tracking assertions are
# skipped, and the output says so rather than passing quietly.
_, in_repo = git('rev-parse', '--is-inside-work-tree')

# ---- 1. every .ihx rule's .lk prerequisite ---------------------------------
#
# The rules are hand-written one-liners with a literal script name, so this is a
# line scan rather than a make invocation.  A prerequisite that is a variable
# reference rather than a literal is reported, not skipped: it would be a way
# for this check to pass while knowing nothing.

File.readlines(MAKEFILE, encoding: 'UTF-8').each_with_index do |line, n|
  next unless line.chomp =~ /\A([^\s:=#]+\.ihx):(.*)\z/

  target = Regexp.last_match(1)
  deps = Regexp.last_match(2)
  deps.split(/\s+/).select { |d| d.end_with?('.lk') }.each do |script|
    if script.include?('$')
      errors << "Makefile:#{n + 1}: #{target} names its linker script indirectly " \
                "(#{script}); this check has to be able to name the file"
      next
    end
    seen[script] ||= "#{target} (Makefile:#{n + 1})"
  end
end

if seen.empty?
  errors << 'no .ihx rule in the Makefile names a .lk prerequisite; the scan ' \
            'found nothing to check, which usually means the rules moved'
end

# ---- 2. each referenced script is present and tracked ----------------------

seen.each do |script, user|
  path = File.join(ROOT, script)
  unless File.file?(path)
    errors << "#{script}: needed by #{user} but not on disk"
    next
  end
  next unless in_repo
  _, ok = git('ls-files', '--error-unmatch', script)
  unless ok
    errors << "#{script}: needed by #{user} but NOT tracked by git - a fresh " \
              'checkout would not have it and could not link'
  end
end

# ---- 3. no .lk anywhere in the tree is ignored ------------------------------
#
# Belt and braces over the rules above.  This catches a hand-written script in
# a directory no rule references yet, which is the same bug waiting for the next
# app.  It also catches a script that is tracked but whose path an ignore rule
# still matches: `git status` stays clean while editing it as a new file is
# ignored, which is how the first one of the seven got in.
Dir.glob(File.join(ROOT, '**', '*.lk')).sort.each do |abs|
  next if abs.include?('/.git/')
  next unless in_repo
  rel = abs.sub("#{ROOT}/", '')
  _, ignored = git('check-ignore', '--no-index', '-q', rel)
  next unless ignored
  errors << "#{rel}: a linker script is matched by a gitignore rule.  Every " \
            '.lk in this tree is a hand-written input - the build emits none - ' \
            'so this is source, not a build product'
end

if errors.any?
  errors.each { |e| puts "  x #{e}" }
  abort 'FAIL: linker scripts the build needs are not all present and tracked'
end

if in_repo
  puts "  linker scripts: %d named by .ihx rules, all present and tracked" % seen.size
  puts '  x every .lk in the tree is source, and tracked: OK'
else
  puts "  linker scripts: %d named by .ihx rules, all present " % seen.size
  puts '  (not a git work tree: presence checked, tracking not applicable)'
end
