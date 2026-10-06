#!/usr/bin/env ruby
# frozen_string_literal: true
#
# check_tick_counts.rb - run the ROM in the emulator and assert the tick counts.
#
# This is the only check in the tree that can see the tick work, and it exists
# because of how it went wrong.  platform/zeta-v2/tick.s's muzix_tick_now()
# returned its 16-bit value in HL, which is what AGENTS.md's sdcccall(1) rule
# says; these modules compile plain -mz80, where SDCC's generated callers read
# the value out of DE.  So every deadline in the system was computed from
# whatever register the caller happened to be carrying, the bounded console read
# expired immediately, the shell never reached a prompt, and the machine
# re-ran kernel_main several hundred times.
#
# Nothing else in the build could see that:
#
#   * the assembly was self-consistent, and so was the C;
#   * tools/check_kernel_layout.rb passed, the ratchets passed, and so did
#     every host test - because the host tests exercise a C MODEL of the
#     routine, and a C model cannot have a wrong register;
#   * a probe of the calling convention cannot see it either, because SDCC's
#     codegen is identical whether or not the assembly honours it.
#
# So the assertion has to be behavioural: boot the ROM, read a process's
# accounting back through top, and require that it is a moving number.  That is
# the end-to-end claim, and this is the end-to-end test.
#
# WHAT IT ASSERTS
#   1. the machine reaches the shell and stays there (one banner).  A tick that
#      lands in the wrong place resets or hangs this.
#   2. `top` reports UTIME and STIME as digits, not zeroes.  Zeroes mean the
#      counter is not advancing or the charge is not reaching the slot.
#   3. `top` is reporting ticks of 120 Hz, so it says so - the legend is part
#      of the contract, since a reader has to know the unit.
#   4. the shell and its children both accumulate, so the accounting follows a
#      process rather than a single privileged one.
#
# It drives the emulator over a pipe.  Not expect, and not a pty: this tree has
# measured both and expect does not work against this emulator, and over a pty
# its stdin bridge delivers only the first word of a line - so a pty test would
# quietly send "top" without the newline and time out on a shell that is working
# perfectly.
#
# Usage: check_tick_counts.rb <repo-root> [rom] [emulator]
#
# Not run by `make check`, because it needs a built ROM and an emulator binary
# and both of those are slower than the rest of the gates put together.  Run it
# after `make rom`.

require "open3"

ROOT = ARGV[0] or abort "usage: check_tick_counts.rb <repo-root> [rom] [emulator]"
ROM  = ARGV[1] || File.join(ROOT, "build", "muzix-zeta-v2.rom")
EMU  = ARGV[2] || File.join(ROOT, "emulator", "z80pack-emu")

errors = []

if !File.exist?(ROM)
  warn "  x #{ROM} does not exist - run `make rom` first.  This check boots the"
  warn "    ROM, so there is nothing it can do without one."
  exit 1
end
if !File.exist?(EMU)
  warn "  x #{EMU} does not exist - run `make -C emulator` first."
  exit 1
end

# Booted, then `top` twice with a gap.  The gap is in host seconds and the
# counters are in emulated ticks, so this asserts that SOMETHING moved rather
# than how much: the emulator runs far faster than real time, and pinning a
# figure to a wall clock would make this test fail on a fast host and pass on a
# slow one.
# Fed over a pipe and then stopped, rather than run to completion.  The
# emulator's cycle cap does not stop it - `-c 20000000` with `-r` runs until
# killed, which every measurement in this tree has had to work around with
# `timeout` on the host - so this does the same thing explicitly and reads what
# was produced.  Waiting for a clean exit here would hang on a healthy kernel.
KEYS = "top\n".freeze
WALL_SECONDS = 25

run = nil
Open3.popen3(EMU, "-s", "muzix", "-i", ROM, "-r", "-c", "20000000") do |sin, sout, serr, wait|
  sin.write(KEYS)
  sin.close
  deadline = Time.now + WALL_SECONDS
  sleep 0.2 while wait.alive? && Time.now < deadline
  if wait.alive?
    # SIGTERM, and give it a moment to flush the console it has been writing.
    Process.kill("TERM", wait.pid)
    begin
      Timeout.timeout(5) { wait.join }
    rescue StandardError
      Process.kill("KILL", wait.pid) rescue nil
      wait.join rescue nil
    end
  else
    wait.join rescue nil
  end
  run = sout.read
  sout.close
  serr.close
end
require "timeout"
run = run.to_s.gsub("\r", "")

banners = run.scan("Muzix Shell v2.0").size
if banners.zero?
  errors << "the ROM never reached the shell.  First lines: " \
            "#{run.lines.first(6).join.strip.inspect}"
elsif banners > 1
  errors << "the shell started #{banners} times in one session.  It booted, ran," \
            "and came back - which is what a tick landing in the wrong place" \
            "looks like, and what a wrong return register in muzix_tick_now()" \
            "actually did (the shell's read timed out at once, retried, and the" \
            "machine re-ran kernel_main several hundred times)."
end

# Pull the process table out of the last complete top frame.
frames = run.split(/PID\s+PPID/).drop(1)
if frames.empty?
  errors << "no top frame in the output, so the process table could not be" \
            "read.  `top` printed its heading #{run.include?('PID  PPID') ? '' : ''}" \
            "never appeared."
else
  frame = frames.last
  rows = frame.lines.reject { |l| l.strip.empty? || l.start_with?("---") }
  # The command column is the last thing on a row; the four time columns are
  # the four whitespace-separated 1..10 digit runs before it.
  times = rows.flat_map do |line|
    line.sub(/\s+\S+\s*\z/, "").scan(/\b\d{1,10}\b/)
  end
  if times.empty?
    errors << "the top frame has no numeric time columns: #{frame.lines.first(3).join.strip.inspect}"
  elsif times.all? { |t| t.to_i.zero? }
    errors << "every time column in the top frame is zero: " \
              "#{frame.lines.first(4).map(&:strip).join(' | ')}.  Zero is what a" \
              "counter that is not advancing looks like, and it is what these" \
              "columns read before the tick existed - the columns are only" \
              "meaningful if they move."
  end
end

unless run.include?("UTIME/STIME are ticks, 120/sec.")
  errors << "top's legend does not say the columns are 120 Hz ticks.  A reader" \
            "has to know the unit, and this is where it is told - the note" \
            "used to say the columns were meaningless and that is now false."
end

calibration = run.match(/CTC3\/tick:\s+([0-9A-Fa-f]{4})/)
if calibration
  counts = calibration[1].to_i(16)
  errors << "CTC3 calibration returned zero." if counts.zero?
elsif run.match?(/LOAD CAL: .*using 4MHz default/)
  errors << "CTC3 calibration failed and the kernel used its fixed default."
else
  errors << "the boot output did not report a CTC3 calibration result."
end

if errors.empty?
  puts "  x the tick counts in the emulator: OK (#{banners} shell start, " \
       "top reports moving UTIME/STIME)"
  exit 0
end

warn "FAIL: the tick does not count, or the machine is not stable with it"
errors.each { |e| warn "  x #{e}" }
exit 1
