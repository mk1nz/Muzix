#!/usr/bin/env ruby
# frozen_string_literal: true

# Check the link map for the one invariant that the banked-memory design cannot
# express at runtime: only window 0 survives a map change.
#
# A process map installs the process's stack into window 1 and its text into
# window 2, and leaves only window 0 (kernel page 0x20) and window 3 (kernel
# page 0x23) as kernel.  So any code that writes a bank register, installs a
# map, or hands the CPU to a process has to be linked below 0x4000.  Code above
# that address is a banked-out page the moment the map changes, which is how a
# context switch turns into a jump into the boot stub.
#
# This is checked structurally, from the link map, rather than trusted to a
# comment or to the runtime guard.  muzix_zeta_caller_in_window0() is supposed to
# catch it, but it reads SP+2 on the assumption that its caller built no stack
# frame - and SDCC emits `push hl` / `push de` at the top of every forwarder in
# the file, so the word it inspects is saved register content, not a return
# address.  The check passed while the system rebooted, which is the whole
# reason this file exists.
#
# Usage: check_window0.rb <kernel.map>
#
# A failure means code grew out of window 0.  The fix is never to move the
# check: the caller has to move, or the module has to shrink.

# Modules that write the bank registers are derived from the source, not
# hardcoded.  A list in the checker is a list that can be forgotten: a new
# module that programs ports $78-$7B would not be in it, and would be linked
# wherever the linker liked.  The map alone cannot tell - the same registers are
# written from several files - so the sources are scanned for the writes and
# those modules are then required to be in window 0.
#
# A module that only *calls* a bank primitive is caught elsewhere:
# check_memory_manager.rb forbids the bank-switch primitives outside the
# platform layer.  Here the requirement is about where the code ends up, since a
# caller of a bank primitive in window 1 faults the moment the map changes.

repo = ARGV[1] || "."

# A direct write to one of the four bank select ports, in either dialect:
# C's z80_outb(0x79, ...) / z80_outb(MUZIX_ZETA_BANK_PORT_1, ...), and
# sdasz80's `out (0x79),a`.
BANK_WRITE_PATTERNS = [
  /z80_outb\(\s*(?:MUZIX_ZETA_BANK_PORT_[0-3]|0x7[89ab])\s*,/,
  /\bout\s*\(\s*0x7[89ab]\s*\)\s*,/
].freeze

# The platform sources are the only place a bank write may live, and they are
# matched by the scan itself, so nothing has to be maintained by hand.
bank_modules = {}
Dir.glob(File.join(repo, "**", "*.{c,s,S}")).each do |path|
  rel = path.sub(/\A#{Regexp.escape(repo)}\/?/, "")
  next if rel.include?("/test_") || File.basename(rel).start_with?("test_")
  next unless rel.start_with?("kernel/", "mm/", "fs/", "pm/", "lib/",
                              "platform/zeta-v2/")
  # Binary: some sources carry non-ASCII comments, and a regex match against
  # a string tagged US-ASCII raises rather than failing to match.
  text = File.read(path, mode: "rb")
  next unless BANK_WRITE_PATTERNS.any? { |re| text =~ re }
  bank_modules[rel] = true
end

# The map names objects by their .rel stem, so map a source file onto the module
# name the linker will print.
def module_stem(rel)
  base = File.basename(rel, ".*")
  case rel
  when %r{platform/zeta-v2/} then base
  else base
  end
end

# Assembler sources become their own module (bank_io.s -> muzix_zeta_bank_io),
# so the .module directive is authoritative for those.
asm_modules = {}
Dir.glob(File.join(repo, "platform", "zeta-v2", "*.s")).each do |path|
  m = File.read(path, mode: "rb")[/^\.module\s+(\w+)/, 1]
  asm_modules[File.basename(path)] = m if m
end

wanted = {}
bank_modules.each_key { |rel| wanted[module_stem(rel)] = rel }
asm_modules.each { |file, mod| wanted[mod] = file if bank_modules.key?(File.join("platform/zeta-v2", file)) }

# These do not write a bank register but must be window 0 all the same: the trap
# parks the user stack pointer and the resume point before the map changes, and
# the handoff installs the map and jumps.  Found by name, not by module, so a
# rename cannot drop them.
#
# muzix_tick is here for the same reason and it is here because of how it was
# nearly missed.  The IM1 handler is reached through a three-byte JP planted at
# 0038H, and the handler itself runs at an arbitrary instant - including in the
# middle of write_all_banks(), which programs window 0 first and then windows
# 1-3 one at a time.  So "the handler only reads its own code and a counter in
# window 3" is true, and the counter is at 0xFA26 in _DATA, and the handler was
# at 0xFA43 with it: a missing `.area _CODE` in tick.s put the whole module,
# code included, into _DATA.  Nothing failed.  The link succeeded, _CODE and
# _DATA did not overlap, the memory-manager rules were all satisfied, and the
# kernel layout gate passed with room to spare - because every one of those
# checks asks a question this layout answers correctly.  The machine would have
# taken its first tick by executing window 3, which is the process's own
# window 3 the moment a process is running.
#
# A rule that is true and unenforced is a comment.  This one is enforced.
PINNED_MODULES = %w[
  muzix_kernel_entry
  muzix_syscall_entry
  muzix_tick
].freeze

WINDOW_SIZE = 0x4000

# Individually named entry points.  Redundant with the module rule above, and
# kept anyway: if a module is ever renamed or split, these still fail rather
# than silently falling out of the check.
PINNED_SYMBOLS = %w[
  _muzix_z80_syscall_entry
  _muzix_zeta_enter_userspace
  _muzix_zeta_resume_from
  _zeta_resume_jump
  _zeta_enter_process
  _zeta_resume_process
  _zeta_load_map
  _zeta_kernel_enter
  _zeta_kernel_exit
  _muzix_zeta_caller_in_window0
  _muzix_tick_isr
  _muzix_tick_init
].freeze

map_path = ARGV[0] or abort "usage: check_window0.rb <kernel.map> [repo-root]"

errors = []
seen_pinned = {}
# A pinned symbol found in the map but NOT under a _CODE header is a distinct
# failure from a pinned symbol that is absent: it is code that got linked into
# a data area, which is exactly what a missing `.area _CODE` in a .s module
# does, and "not in the map at all" is a misleading thing to be told about it.
seen_outside_code = {}

area = nil
in_code = false

File.foreach(map_path, mode: "rb") do |line|
  # An area header: "_CODE   00000098   0000E2F5 = ...".  ASxxxx prints one
  # section per object module, so the area in force changes as we walk.
  if line =~ /\A(_[A-Z][A-Z_]*)\s+([0-9A-F]{4,})\s/
    area = Regexp.last_match(1)
    next
  end

  # A symbol line: "     000000B5  _name    module"
  next unless line =~ /\A\s+([0-9A-F]{4,8})\s+(\S+)\s+(\S+)/

  addr = Regexp.last_match(1).to_i(16)
  name = Regexp.last_match(2)
  mod = Regexp.last_match(3)

  # Only code.  These modules also own data objects, and a data symbol at
  # 0xFxxx is window 3 - a kernel page, and perfectly correct.
  unless area == "_CODE"
    seen_outside_code[name] = addr if PINNED_SYMBOLS.include?(name)
    next
  end

  if (wanted.key?(mod) || PINNED_MODULES.include?(mod)) && addr >= WINDOW_SIZE
    errors << format(
      "%s (%s) is linked at 0x%04X, window %d. It can change the map, and " \
      "window %d does not survive a map change. Move it below 0x%04X or make " \
      "it thinner.",
      name, mod, addr, (addr & 0xC000) >> 14, (addr & 0xC000) >> 14, WINDOW_SIZE
    )
  end

  if PINNED_SYMBOLS.include?(name)
    seen_pinned[name] = addr
    if addr >= WINDOW_SIZE
      errors << format(
        "%s is linked at 0x%04X, window %d. This is the window-0 handoff " \
        "path and it cannot be anywhere else.",
        name, addr, (addr & 0xC000) >> 14
      )
    end
  end
end

missing = PINNED_SYMBOLS.reject { |name| seen_pinned.key?(name) || seen_outside_code.key?(name) }
unless missing.empty?
  # A missing symbol means the map changed shape, or the module was dropped.
  # Silently passing would make the check decorative, so report it.
  errors << "these window-0 entry points are not in the map at all: " \
            "#{missing.join(', ')}"
end

unless seen_outside_code.empty?
  seen_outside_code.each do |name, addr|
    errors << format(
      "%s is linked at 0x%04X, which is window %d, and is not in _CODE at " \
      "all. A .s module that assembles its code into a data area is the " \
      "usual cause: check that the `.area _CODE` is present and comes after " \
      "the `.area` that holds the module's data.",
      name, addr, (addr & 0xC000) >> 14
    )
  end
end

if errors.empty?
  puts "  x window-0 handoff confined to window 0: OK"
  exit 0
end

warn "FAIL: code that can change the map is not confined to window 0"
errors.each { |e| warn "  #{e}" }
exit 1
