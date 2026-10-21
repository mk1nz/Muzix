#!/usr/bin/env ruby
# frozen_string_literal: true
#
# check_proc_identity.rb - the process-identity gate.
#
# Refuses the arithmetic that made one identity look like two.
#
# `muzix_ipc_proc_t` lives in procs[], and procs[] does not index from zero:
# the first NR_TASKS entries hold the negative system-task numbers, so a
# process number nr sits at NR_TASKS + nr.  That `NR_TASKS +` is an ARRAY
# OFFSET.  It is not a property of the process, and it must never be added to
# a number that is about to be used as a process number.
#
# Five places did exactly that, each plausibly, each wrongly, each invisibly,
# and between them they cost days of chasing what looked like unrelated
# filesystem and IPC failures.  The full list and the exact damage each one
# did is in the long note above MUZIX_PROC_FS in include/muzix_ipc.h.
#
# The rule this gate enforces:
#
#   1. muzix_ipc_proc() in kernel/ipc.c is the ONLY function allowed to index
#      procs[] or to add NR_TASKS to anything.  Everything else takes and
#      returns process numbers.
#   2. Server numbers are the MUZIX_PROC_* constants.  Nobody computes one
#      from an id or a slot.
#
# It runs from `make check` (target test-proc-identity).
#
# Written as a gate rather than a note because every one of those five was
# correct when read in isolation.  They were only wrong in combination, which
# is exactly the thing a reader cannot see and a grep can.

require "find"

ROOT = File.expand_path("..", __dir__)

# kernel/ipc.c owns the arithmetic and is exempt from the arithmetic rules.
# Everything else is checked.  This is deliberately a small allow-list: a new
# file that needs the offset should add a call to muzix_ipc_proc(), not add
# itself here.
ARITHMETIC_EXEMPT = ["kernel/ipc.c"].freeze

# Direct indexing of the table with a NON-CONSTANT index.  `procs[26]` is the
# array's own declaration and is fine; `procs[ipc_slot]` is a bypass of the
# accessor and is not.
#
# The guard used to be a negative lookahead on the first character -
# `procs\s*\[\s*(?![0-9\s])`, "a bracket whose content starts with a digit is
# fine" - and the bypass that shape invited was `procs[10 + nr]`: the content
# starts with a digit, so the lookahead passed, and the offset arithmetic sat
# right behind it.  The guard is now about the CONTENT instead of its first
# character: a bracket holding the offset constant combined with an arithmetic
# operator is the offset arithmetic, whether or not it begins with a digit.
# A bare literal index stays clean - `procs[26]` is the array's own
# declaration - because a literal is at least a thing a grep can find, and
# widening past the documented bypass is how this gate would start crying wolf.
PROCS_INDEX = /procs\s*\[[^\]]*\b(?:NR_TASKS|10)\b\s*[+-]/.freeze

# A name that denotes a slot, a server, a process number or a task: one of the
# stems below, or a word made of underscore-separated components one of which is
# a stem.  `slot`, `ipc_slot`, `server_id`, `proc_nr`, `srv`, `srv_nr`, `who`.
#
# It is stems rather than an exact whitelist (current_slot|kernel_slot|slot|
# proc_nr|nr|id) because a whitelist is bypassed by any name it did not think
# of: `10 + srv` and `NR_TASKS + ipc_nr` both went straight through.  And it is
# stems rather than "any identifier", which is the discipline that keeps decimal
# arithmetic out - `result = result * 10 + (*nptr - '0')` in lib/libc.c,
# `value = value * 10 + (s[i] - '0')` in apps/date.c and `return 10 + status`
# in fs/test_romfs_format.c all add ten to something that is not a slot, and
# none of `digit`, `status`, `rc`, `moved`, `result`, `value` or `offset`
# contains a stem.  Measured across every .c/.h in the tree on 2026-10-14, the
# stem rule matches nothing that is there today.
SLOT_NAME = '(?:\w+_)*(?:slot|srv|nr|proc|who|id)(?:_\w+)*'

# The spelling that would reintroduce the bug, in EITHER operand order:
#
#   NR_TASKS + srv   - nobody writes this except by mistake; the only
#                      legitimate use is inside muzix_ipc_proc(), which is
#                      exempt.
#   10 + srv         - the exact shape all five bugs took.
#   slot + 10        - the same sum with its operands swapped, which the old
#                      pattern could not see at all: it required the constant
#                      first, so a reversed spelling passed.
#
# Matching ONLY a slot-shaped identifier is the whole point: a gate that fires
# on `result * 10 + (*nptr - '0')` in libc gets switched off, and then it
# protects nothing.
SLOT_OFFSET = /
  \b(?:NR_TASKS|10)\s*\+\s*\b(?:#{SLOT_NAME})\b
  |
  \b(?:#{SLOT_NAME})\b\s*\+\s*\b(?:NR_TASKS|10)\b
/x

# Deprecated spelling that must not gain NEW callers.  The declaration in the
# header and the compat wrapper in kernel/ipc.c are the migration itself, not
# new callers, and are excluded by the two literal strings below.
DEPRECATED = /\bmuzix_ipc_get_proc\s*\(/.freeze
DEPRECATED_DEF = /(?:^|\s)muzix_ipc_proc_t\s*\*\s*muzix_ipc_get_proc\s*\(|return muzix_ipc_proc\(/.freeze

# A local re-definition of a server number - the shell.c shape.  A server
# number must come from the header.
#
# Two families, both of which refuse a literal value:
#
#   (a) the historical one: a name containing FS/MM/PM and ending in SLOT,
#       which is what the shell.c bug spelled - `#define
#       FS_SERVER_IPC_SLOT 11`.  SLOT must be the last word: MUZIX_FS_CACHE_SLOTS
#       in fs/block_cache.h and MUZIX_FS_INODE_SLOTS in fs/inode_table.h are
#       cache and inode sizes, not identities, and an S on the end is what
#       keeps them clean.
#   (b) the one that went the other way round: FS_SERVER, MM_SERVER or
#       PM_SERVER as the leading words of the name.  The old pattern required an
#       SLOT suffix, so `#define FS_SERVER 2` - the same local literal without
#       the suffix - passed.
#
# `#define MUZIX_PROC_FS 2` in include/muzix_ipc.h is the sanctioned spelling
# and matches neither: the name carries no _SERVER word and no SLOT suffix.
# `#define FS_SERVER_IPC_SLOT MUZIX_PROC_FS` is the sanctioned alias and matches
# only when the value is a digit.
REDEFINE_PROC = /#define\s+(?:\w*(?:FS|MM|PM)\w*(?:IPC_)?SLOT|(?:FS|MM|PM)_SERVER\w*)\s+\d+/.freeze

def rb_files
  out = []
  Find.find(ROOT) do |path|
    next unless path.end_with?(".c", ".h")
    next if path.include?("/.git/")
    next if path.include?("/.kilo/")
    next if path.include?("/build/")
    next if path.include?("/oldOrphanedCodeBackup/")
    out << path
  end
  out
end

def rel(path)
  path.delete_prefix("#{ROOT}/")
end

# Strip a trailing /* ... */ so documentation naming the arithmetic does not
# read as an offence.  A line that is entirely a comment is skipped too.
def code_of(line)
  ln = line.sub(%r{/\*.*\*/\s*$}, "").strip
  return nil if ln.empty? || ln.start_with?("*") || ln.start_with?("//") || ln.start_with?("/*")
  ln
end

def offenders
  bad = []
  rb_files.each do |path|
    r = rel(path)
    exempt = ARITHMETIC_EXEMPT.include?(r)
    File.readlines(path, chomp: true, encoding: Encoding::UTF_8).each_with_index do |line, i|
      code = code_of(line)
      next unless code
      lineno = i + 1

      if !exempt && code =~ SLOT_OFFSET
        bad << [r, lineno, "adds the array offset to a process number - use muzix_ipc_proc()", line.strip]
      end
      if !exempt && code =~ PROCS_INDEX
        bad << [r, lineno, "indexes procs[] with a variable - use muzix_ipc_proc()", line.strip]
      end
      if !exempt && code =~ REDEFINE_PROC
        bad << [r, lineno, "redefines a server process number - use MUZIX_PROC_* from include/muzix_ipc.h", line.strip]
      end
      if code =~ DEPRECATED && code !~ DEPRECATED_DEF
        bad << [r, lineno, "new caller of the deprecated muzix_ipc_get_proc() - use muzix_ipc_proc()", line.strip]
      end
    end
  end
  bad
end

if ARGV[0] == "--selfcheck"
  puts "check_proc_identity: #{rb_files.size} source files scanned"
  puts "  arithmetic-exempt: #{ARITHMETIC_EXEMPT.join(", ")}"
  puts "  deprecated callers (informational, not failures):"
  total = 0
  rb_files.each do |path|
    r = rel(path)
    File.readlines(path).each_with_index do |line, i|
      total += 1 if line =~ DEPRECATED
    end
  end
  puts "    #{total}"
  exit 0
end

puts "Checking process identity..."
bad = offenders

if bad.empty?
  puts "  x one process number, one owner of the array offset: OK"
  puts "  x no new deprecated accessor callers: OK"
  puts "All process-identity checks passed."
  exit 0
end

puts
puts "#{bad.size} process-identity violation(s):"
puts
bad.group_by { |r, _, _, _| r }.each do |file, rows|
  puts "#{file}:"
  rows.each do |_, lineno, why, line|
    puts "  #{lineno}: #{why}"
    puts "    #{line}"
  end
  puts
end
puts "The process number IS the kernel proc-table slot.  muzix_ipc_proc() in"
puts "kernel/ipc.c is the only code that may compute the procs[] array offset,"
puts "and server numbers come from MUZIX_PROC_* in include/muzix_ipc.h."
puts
puts "include/muzix_ipc.h documents all five sites that each invented this"
puts "arithmetic by hand, and what each one broke."
exit 1
