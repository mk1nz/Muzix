#!/usr/bin/env ruby
# frozen_string_literal: true

# Check that every park() call site parks the process that is actually current.
#
# WHY THIS NEEDS A GATE
# ---------------------
# A parked slot's resume point is a PAIR of words the syscall trap captured: the
# program counter and the stack pointer it read off the user's stack on the way in
# (platform/zeta-v2/syscall_entry.s).  park() in kernel/proc_table.c records that
# pair into the slot, and it is the only place that can - the trap's capture is
# written to two global cells, muzix_resume_pc and muzix_resume_sp, because they
# have to be read while window 1 still holds the user's own page, which is long
# past by the time any handler runs.
#
# Those cells are GLOBAL.  They describe the process that made the most recent
# system call, and nothing marks which process that was.  So park() is only
# correct when the slot it is about to park is that same process - and the only
# thing that establishes that today is the argument its one call site passes:
# muzix_current_slot(ctx), which is the slot of the context currently executing.
#
# Park any other slot and its resume pair belongs to somebody else.  The CPU is
# then later pointed at another process's stack: it executes stack bytes, the
# boot stub eventually runs, window 1 lands on ROM so an interrupt's pushes are
# discarded, and the machine ends up jumping to 0x0000 and rebooting.
#
# That is not hypothetical - it is exactly what happened, and it looked like a
# mystery because every other part of the park path was correct.  park() itself
# was also, at one point, reading the resume PC back out of the stack itself,
# which is the same class of bug from the other side: it read the kernel's own
# code, because by then window 1 was kernel page 0x21 and not the process's.
#
# So the invariant is checked structurally here rather than left to a comment.
# A second call site that parks a different slot is a build failure, not a review
# comment - the same bargain check_memory_manager.rb and check_window0.rb make.
#
# Usage: check_park_site.rb <source-root>
#
# A failure means a new park() call site is attributing the wrong resume point.
# The fix is never to move the check: either the caller must park
# muzix_current_slot(), or park() has to take the pair as an argument instead of
# reading it from the global cells.

root = ARGV[1] || "."

# Park() and the accessor that names the slot a context is currently running.
PARK = /muzix_proc_table_park\s*\(([^;]*?)\)\s*;/m
CURRENT_SLOT = /muzix_current_slot\s*\(\s*ctx\s*\)/

# The declaration and the prototype are not call sites.
SKIP = /\A\s*(?:void|int)\s+muzix_proc_table_park\s*\(/

failures = []
sites = 0

Dir.glob(File.join(root, "kernel", "**", "*.c")).sort.each do |path|
  next unless File.file?(path)

  rel = path.sub("#{root}/", "")

  # Tests are excluded, and deliberately so: they drive park() directly on a
  # model table, parking slots 0, 1, 3 and -1 on purpose to exercise the bounds
  # check and the queue behaviour.  That is the point of them and it is not what
  # this rule is about.  The rule is about production call sites, where the slot
  # is not a choice - it is whichever process the trap just captured.
  next if File.basename(path).start_with?("test_")

  src = File.read(path)

  src.each_line.with_index(1) do |line, lineno|
    next unless line.include?("muzix_proc_table_park")
    next if line =~ SKIP

    # Skip comments.  The fix for this very bug is quoted in the comment above
    # the call site, so a scanner that reads comments reports the "before" form
    # as a live call and the count stops meaning anything.
    stripped = line.lstrip
    next if stripped.start_with?("*", "/*", "//", "#")

    # Collect the full argument list, which may span lines.
    idx = src.index(line)
    tail = src[idx..-1]
    m = tail.match(PARK)
    next unless m

    sites += 1
    args = m[1]

    # The second argument is the slot. It must name the context's own current
    # slot; anything else attributes another process's resume pair to this one.
    second = args.split(",")[1].to_s.strip
    puts "check_park_site:   #{rel}:#{lineno} parks #{second}"

    if !second.match?(CURRENT_SLOT)
      failures << "#{rel}:#{lineno}: parks slot '#{second}' - must be muzix_current_slot(ctx)"
    end
  end
end

if sites.zero?
  warn "check_park_site: no park() call sites found - the scan is broken, not the tree"
  exit 1
end

failures.each { |f| warn "check_park_site: #{f}" }
unless failures.empty?
  warn "check_park_site: #{failures.size} of #{sites} park() call sites attribute the wrong resume point"
  warn "check_park_site: park() reads muzix_resume_pc/sp, which describe the process that made the"
  warn "check_park_site: last system call.  Parking a slot that is not that process resumes it at"
  warn "check_park_site: another process's stack, and the machine reboots."
  exit 1
end

puts "check_park_site: #{sites} park() call site(s), all parking muzix_current_slot(ctx): OK"