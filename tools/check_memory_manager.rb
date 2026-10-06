#!/usr/bin/env ruby
# frozen_string_literal: true

# Enforce the two memory-access rules from AGENTS.md at build time.
#
#   1. "All memory read/write operations and exec calls MUST go through the
#       memory manager. No direct memory access to banked regions or exec
#       without MM mediation."
#   2. "No trampolines or manual bank switching anywhere."
#
# The layout guard (tools/check_kernel_layout.rb) protects the *link*. Nothing
# protected the *architecture*: a new `out (0x79)` anywhere in kernel/ or fs/
# would have built cleanly. These checks fail the build instead.
#
# Usage: check_memory_manager.rb <project-root>

root = ARGV[0] or abort "usage: check_memory_manager.rb <project-root>"

# Files permitted to write bank registers: the memory-manager/platform layer
# itself, and the boot stub, which runs before any MM exists. Deliberately an
# explicit list rather than a directory, so that putting a bank write back into
# kernel_entry.s or a service handler is a build failure.
BANK_WRITE_ALLOWED = %w[
  platform/zeta-v2/kernel_bank_model.c
  platform/zeta-v2/bank_io.s
  platform/zeta-v2/rom_boot.s
].freeze

# Only real instructions count. A #define of the port number, a comment
# mentioning it, or a byte in a generated data blob are all fine - matching
# those would make the check noise nobody reads.
# NOTE: the alternation MUST be grouped. With bare `a|b|c` the | has the
# lowest precedence, so the pattern degenerates to matching the text "0x79"
# anywhere in the file.
BANK_PORTS = '(?:0x78|0x79|0x7a|0x7b|0x7A|0x7B|0x7C)'
WRITE_RE  = /\bout\s*\(\s*#?#{BANK_PORTS}\s*\)/.freeze
READ_RE   = /\bin\s+a\s*,\s*\(?\s*#?#{BANK_PORTS}/.freeze
WRITE_RE2 = /\bz80_outb\s*\(\s*#{BANK_PORTS}\s*,/.freeze

# Generated data, not code: muzix_romfs_data is a byte array of the ROMFS
# image, so any byte value in it is a false positive.
GENERATED = %w[lib/romfs_data.c].freeze

# Strip comments so a note about a port is not a violation. Assembly included:
# sdasz80 comments with `;`, and a .s file that explains in prose why it hands to
# zeta_copy_kernel_to_user() would otherwise read as a call to it.
def strip_comments(text, rel = nil)
  text = text.gsub(%r{/\*.*?\*/}m, " ").gsub(%r{//[^\n]*}, " ")
  text = text.gsub(%r{;[^\n]*}, " ") if rel && %w[.s .S .asm].include?(File.extname(rel))
  text
end

# A user pointer built from a syscall argument, e.g. (const char *)(uintptr_t)arg1
RAW_USER_PTR = /\(\s*(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*\s*\)\s*(?:\(uintptr_t\)\s*)?arg[123]/

SKIP_DIRS = %w[.git build .build test_host].freeze
# test_host/ is here for the same reason build/ is: it is not kernel code. It
# holds the host compiler's stand-ins for the platform copy primitives and the
# Z80 port primitives, so that a test in kernel/, fs/ or mm/ can be compiled and
# linked off the target. Nothing in it is in KERNEL_C_MODULES, SDCC never sees
# it, and it cannot reach the ROM - so the rules below, which are about what the
# kernel does at run time, do not apply to it. It did have to be named here: it
# defines all four zeta_copy_* primitives, and this check requires exactly one
# translation unit to own them, which is the right requirement for the kernel and
# the wrong one for a file that exists to be a second, non-kernel implementation.
SOURCE_EXT = %w[.c .s .h].freeze

def source_files(root)
  Dir.glob("**/*", File::FNM_DOTMATCH, base: root).map do |rel|
    next if rel.start_with?(".", "/")
    next unless SOURCE_EXT.include?(File.extname(rel))
    next if SKIP_DIRS.include?(rel.split("/").first)
    [rel, File.binread(File.join(root, rel)).force_encoding("UTF-8").scrub]
  end.compact
end

errors = []
files = source_files(root)

# --- rule 2: bank register writes only in the MM/platform layer ------------
files.each do |rel, text|
  next if BANK_WRITE_ALLOWED.include?(rel) || GENERATED.include?(rel)
  strip_comments(text, rel).each_line.with_index(1) do |line, n|
    if line =~ WRITE_RE || line =~ WRITE_RE2
      errors << "#{rel}:#{n}: bank register write outside the memory manager: #{line.strip}"
    end
  end
end

# --- rule 2: bank register reads are forbidden outright --------------------
files.each do |rel, text|
  next if GENERATED.include?(rel)
  strip_comments(text, rel).each_line.with_index(1) do |line, n|
    if line =~ READ_RE
      errors << "#{rel}:#{n}: bank register read (forbidden; use the zeta_state_t shadow): #{line.strip}"
    end
  end
end

# --- rule 1: no raw user pointers in the syscall layer ---------------------
# syscalls.c is the boundary. Anything derived from arg1/2/3 that is cast
# straight to a pointer is a userspace address the kernel dereferences with
# its own bank map installed.
syscalls = files.find { |rel, _| rel.end_with?("kernel/syscalls.c") }
if syscalls
  rel, text = syscalls
  strip_comments(text, rel).each_line.with_index(1) do |line, n|
    errors << "#{rel}:#{n}: raw user pointer, route through muzix_copy_user_* : #{line.strip}" if line =~ RAW_USER_PTR
  end
end

# --- rule 2: the bank-switch primitives are platform-private ----------------
# zeta_swap_region()/zeta_restore_region() write a bank register one window at a
# time without saving the rest of the map, and muzix_zeta_set_cached_banks()
# lets a caller install a map behind the platform layer's back. Neither had a
# caller the platform layer did not control, and every one that did was a bug:
# mm/mm_service.c swapped region 1 for the ROM read (so the bytes above the
# window belonged to the process's text page, not to the ROM), and
# kernel/exec_loader.c pushed a process map into the assembly shadow. Use
# zeta_copy_rom_page_to_kernel() / zeta_with_temp_map_impl() instead.
#
# Scoped to the OS subsystems and to .c call sites: a declaration in a header is
# not a bank switch, the emulator implements the hardware rather than switching
# it, and tools/bootloader.c runs before any memory manager exists.
BANK_PRIMITIVE = /\b(?:zeta_swap_region|zeta_restore_region|zeta_set_bank|zeta_map_kernel|zeta_map_process|muzix_zeta_set_cached_banks|muzix_zeta_copy_window3|muzix_zeta_switch_bank|muzix_zeta_enable_paging)\s*\(/
OS_SUBSYSTEMS = %w[kernel mm fs pm lib].freeze
files.each do |rel, text|
  next unless File.extname(rel) == ".c"
  next unless OS_SUBSYSTEMS.include?(rel.split("/").first)
  next if File.basename(rel).start_with?("test_")
  strip_comments(text, rel).each_line.with_index(1) do |line, n|
    next unless line =~ BANK_PRIMITIVE
    errors << "#{rel}:#{n}: manual bank switch outside the platform layer, " \
              "use the MM copy/temp-map primitives: #{line.strip}"
  end
end

# --- the platform copy primitives, derived from the sources ------------------
# zeta_copy_* is how the platform layer names the operations that move a byte in
# or out of a banked process window. Which functions those are is read off the
# sources rather than written out here: a primitive that is added to the platform
# layer is then covered by every rule below from the moment it is declared, and
# one that is removed cannot leave a stale entry asserting a check that no longer
# has anything to look at.
#
# A definition is a return type, then the name, at column 0. That is what
# separates it from a call: a call has no return type in front of it and is
# indented. Both spellings are matched because SDCC's own prototypes and sdasz80
# are not the only declarers.
PRIMITIVE_DEF = /^(?:[A-Za-z_]\w*\s+[\s*]*)+(zeta_copy_\w+)\s*\(/

# Which file names which primitive. Header declarations count: the platform
# header is how the primitives reach the memory manager, and a .c that defines
# one without declaring it anywhere is still a definition site.
primitive_sites = Hash.new { |h, k| h[k] = [] }
files.each do |rel, text|
  strip_comments(text, rel).each_line do |line|
    if line =~ PRIMITIVE_DEF
      primitive_sites[Regexp.last_match(1)] << rel
    end
  end
end
COPY_PRIMITIVES = primitive_sites.keys.sort

# The canonical definition is the one translation unit that defines all of them.
# Naming the file rather than hardcoding it keeps this rule from breaking when
# the platform layer is split or renamed, and the requirement is exact: a file
# that defines only some of the set is a separate, partial implementation, and
# is handled below as one.
canonical_def = COPY_PRIMITIVES.map { |fn| primitive_sites[fn].select { |r| r.end_with?(".c") } }
canonical_candidates = canonical_def.flatten.uniq.select do |rel|
  COPY_PRIMITIVES.all? { |fn| primitive_sites[fn].include?(rel) }
end

if COPY_PRIMITIVES.empty?
  errors << "no zeta_copy_* primitives were found; this rule is looking at nothing"
end

if canonical_candidates.size != 1
  errors << "cannot tell which file owns the platform copy primitives " \
            "(#{COPY_PRIMITIVES.join(', ')}): #{canonical_candidates.inspect}. " \
            "Exactly one translation unit is expected to define all of them."
end
canonical_rel = canonical_candidates.first

# A directory that defines its own copy of a primitive is a separate
# implementation with its own hardware model, not a caller of the platform's.
# platform/zeta-v2/test_kernel/bank_model.c is one: it is a standalone test
# kernel that reimplements the primitives rather than linking the real ones, so
# calls inside it resolve to itself. That is derived here and printed below -
# an exclusion nobody can see is an exclusion nobody maintains.
shadow_dirs = {}
primitive_sites.each_value do |rels|
  rels.each do |rel|
    dir = File.dirname(rel)
    next if dir == File.dirname(canonical_rel.to_s)
    next if shadow_dirs.key?(dir)
    shadow_dirs[dir] = rel
  end
end

# --- rule 1: the primitives are private to the memory manager ----------------
# AGENTS.md: "All memory read/write operations ... MUST go through the memory
# manager." The copy primitives program the bank registers, so a caller outside
# the memory manager is a caller outside the mediation the rule asks for - and
# the rule is about the *path*, not about the outcome. Every call that existed
# here passed &mm->zeta, the same zeta_state_t the MM passes, so the primitives'
# own region 0/3 refusals still applied and nothing was unsafe. What was missing
# is the thing this check is for: kernel/system_copy.c,
# kernel/system_service.c and kernel/syscalls.c reached the primitives directly
# while the MM's own wrappers sat beside them, and every one of those was correct
# by a shared safety mechanism while the architecture rule lived in a comment on
# each call site instead of in the build.
#
# The sanctioned callers are derived, not listed. A file is part of the memory
# manager if it *defines* a function on the MM's own API prefix, muzix_mm_ - a
# property of the source, so adding an MM copy module widens the rule by itself.
# A hardcoded list of allowed files would be a list that can be forgotten, which
# is exactly how the calls above were legal for as long as they were.
#
# Restricted to .c: a header cannot call anything, and one that merely declares
# an muzix_mm_* function must not inherit the right to program a bank register.
#
# And not test_*.c, for the same reason test_host/ is skipped above rather than
# for a second reason: kernel/test_syscall_setrtc.c defines muzix_mm_* of its own
# to stand in for the memory manager's copy entry points (see its header), and
# without this it would be reported as a memory-manager module and granted the
# right to call the bank primitives.  A test is not part of the memory manager.
MM_API_DEF = /^(?:[A-Za-z_]\w*\s+[\s*]*)+(muzix_mm_\w+)\s*\(/
mm_modules = files.select do |rel, text|
  next false if File.basename(rel) =~ /\Atest_/
  rel.end_with?(".c") && strip_comments(text, rel) =~ MM_API_DEF
end.map(&:first).sort

# The platform layer's own use of its own primitives is sanctioned explicitly
# rather than skipped quietly: the defining module is named here, and the count
# of call sites it actually has is printed under it below. Zero is the honest
# answer today - the primitives call internal helpers, not each other - and a
# nonzero count would be a visible, deliberate thing rather than an exception in
# a conditional nobody reads.
sanctioned = (mm_modules + [canonical_rel].compact).uniq.sort

PRIMITIVE_CALL = /\b(?:zeta_copy_\w+)\s*\(/
platform_internal_sites = 0

# A host test that stands in for the memory manager's copy entry points has to
# reach the host model of the primitives to do it.  kernel/test_syscalls.c
# forwards to the very zeta_copy_* in test_host/zeta_host.c precisely so that
# the window refusals and the staging cap its cases assert are the tree's own
# rather than a restatement of them made inside the test - a test that kept its
# own copy of the rule would agree with the code by construction.
#
# This is the same reasoning as the test_host/ skip above, applied to the rule
# the test_*.c carve-out in the MM-module derivation already carves out.  That
# carve-out was in one place and not the other: a test was held not to be the
# memory manager while still being required not to call what only the memory
# manager may call.
#
# It is deliberately narrow.  The exemption is derived from the file *defining*
# an muzix_mm_copy_* entry point of its own, so a test that uses the real
# memory manager and reaches past it is still a failure, and a test that only
# reads through the seam is unaffected either way.
MM_ENTRY_DEF = /^(?:[A-Za-z_]\w*\s+[\s*]*)+(muzix_mm_copy_\w+)\s*\(/
mm_host_stubs = files.select do |rel, text|
  File.basename(rel) =~ /\Atest_/ && strip_comments(text, rel) =~ MM_ENTRY_DEF
end.map(&:first).sort

files.each do |rel, text|
  next if sanctioned.include?(rel)
  next if mm_host_stubs.include?(rel)
  next if shadow_dirs.key?(File.dirname(rel))
  strip_comments(text, rel).each_line.with_index(1) do |line, n|
    next unless line =~ PRIMITIVE_CALL
    # A definition is not a call; a partial reimplementation is reported by the
    # shadow-directory derivation above instead.
    next if line =~ PRIMITIVE_DEF
    errors << "#{rel}:#{n}: platform copy primitive called outside the memory " \
              "manager, use the muzix_mm_* copy entry points: #{line.strip}"
  end
end
files.each do |rel, text|
  next unless sanctioned.include?(rel)
  next unless rel == canonical_rel
  platform_internal_sites = strip_comments(text, rel).each_line.count do |line|
    line =~ PRIMITIVE_CALL && line !~ PRIMITIVE_DEF
  end
end

# --- rule 1: the kernel's own windows must never be copy targets ------------
# Window 0 ($0000-$3FFF) is pinned to kernel page 0x20 - the boot stub, the
# syscall vector installed at 0x0030 and _CODE from 0x0098 - and window 3
# ($C000-$FFFF) is the kernel's _DATA and C stack. A copy aimed at either is a
# copy against kernel memory reached through a user-supplied address, so the
# copy primitives must refuse both. Every function that derives a window from a
# caller-supplied address has to reject them; check the primitive's own
# definition rather than any call site.
#
# The exemption is derived from the signature too, not named. A primitive that
# takes a `region` or a process map has a caller-chosen window, and both kernel
# windows have to be refused. One that takes neither - a page and an offset, the
# shape a ROM read has - is handed the window itself and has no caller-chosen one
# to refuse, so requiring it to is a false failure.
if canonical_rel
  rel, text = files.find { |r, _t| r == canonical_rel }
  body = strip_comments(text, rel)
  exempt = []
  COPY_PRIMITIVES.each do |fn|
    # The definition, from its signature to the closing brace at column 0.
    m = body.match(/^int\s+#{Regexp.escape(fn)}\s*(\([^)]*\))\s*\n\{(.*?)^\}/m)
    unless m
      errors << "#{rel}: #{fn} has no definition to check window 0/3 refusals against"
      next
    end
    if m[1] !~ /\bregion\b/ && m[1] !~ /process_map_t/
      exempt << fn
      next
    end
    %w[0 3].each do |region|
      unless m[2] =~ /region\s*==\s*#{region}\b/
        errors << "#{rel}: #{fn} does not refuse region #{region}; window #{region} " \
                  "is the kernel's own and a user address can reach it"
      end
    end
  end
end

# --- ratchet --------------------------------------------------------------
# The known rule-1 sites are baselined by their source text, not their line
# number, so the file can be edited around them. A new violation fails the
# build; a baselined one that has been fixed is reported so the list shrinks.
# Nothing is added to the baseline by accident - that is a deliberate edit.
baseline_file = File.join(root, "tools", "mm_violations.txt")
baseline = File.exist?(baseline_file) ? File.readlines(baseline_file).map(&:strip) : []

known, fresh = [], []
errors.each do |e|
  snippet = e.split(": ", 3).last.to_s.strip
  (baseline.any? { |b| !b.empty? && snippet.include?(b) } ? known : fresh) << e
end

unless fresh.empty?
  puts "  x #{fresh.size} NEW memory-manager violation(s):"
  fresh.each { |e| puts "      #{e}" }
  abort "FAIL: memory access is not mediated by the memory manager"
end

if known.empty?
  puts "  x memory-manager mediation: OK (no known violations remain)"
else
  puts "  x memory-manager mediation: #{known.size} known violation(s) still" \
       " baselined in tools/mm_violations.txt (must not grow)"
end
puts "  x no manual bank switching outside the memory manager: OK"
puts "  x no bank register reads: OK"
puts "  x no raw user pointers in the syscall layer: OK"
puts "  x no bank-switch primitives outside the platform layer: OK"
# The allowances this rule makes, printed. A derived rule whose exceptions
# cannot be seen is a rule whose exceptions stop being maintained.
puts "  x platform copy primitives are private to the memory manager: OK"
puts "      primitives derived from the sources: #{COPY_PRIMITIVES.join(', ')}"
puts "      memory manager modules derived from the sources: " \
     "#{mm_modules.empty? ? '(none)' : mm_modules.join(', ')}"
puts "      platform layer's own use, explicit: #{canonical_rel} " \
     "(#{platform_internal_sites} call site#{platform_internal_sites == 1 ? '' : 's'})"
unless shadow_dirs.empty?
  shadow_dirs.each do |dir, owner|
    puts "      separate implementation excluded, derived: #{dir}/ " \
         "(its own primitives in #{owner})"
  end
end
unless mm_host_stubs.empty?
  puts "      host stand-in for the MM copy entry points, derived: " \
       "#{mm_host_stubs.join(', ')}"
end
unless exempt.empty?
  puts "      no caller-chosen window to refuse, derived from the signature: " \
       "#{exempt.join(', ')}"
end
puts "  x every user-facing copy primitive refuses kernel windows 0 and 3: OK"
