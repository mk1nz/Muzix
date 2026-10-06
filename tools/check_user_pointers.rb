#!/usr/bin/env ruby
# frozen_string_literal: true

# Check that a user address cannot be used by the kernel without crossing a
# copy-in boundary.
#
# This is the Muzix analogue of MINIX's p_splimit.  MINIX keeps a per-process
# struct proc with a field naming the lowest legal stack address, so a process
# that passes something else gets refused by a value that lives in the data
# rather than in a review.  Muzix has no such field: a process address is just
# a 16-bit number in a syscall argument, and the only thing standing between it
# and a kernel dereference is that the author remembers to copy it first.
#
# Two rules, both in the syscall layer, because that is the only place a user
# address enters:
#
#   1. A syscall argument is never converted to a pointer type.  A cast to
#      char* or void* is the shape of the mistake: after it, nothing in the type
#      says the address was never checked.
#
#   2. A syscall argument reaching a call is either cast, or used as a value.
#      Uncast, it is indistinguishable from an address being handed to a
#      kernel function that will dereference it.  Requiring the cast makes the
#      author's intent explicit at the call site.
#
# The kernel-side landing buffers are checked to exist for the same reason
# exec_loader takes a path pointer: it walks it, so the pointer has to be the
# kernel's copy, not the process's.
#
# Usage: check_user_pointers.rb <repo-root>
#
# This is a ratchet over syntax, not a proof.  It sees casts, not dereferences,
# and it only covers the syscall layer - a pointer smuggled in through a struct
# field or an assembly argument would pass it.  What it does buy is that the
# dangerous form cannot be written by accident.

root = ARGV[0] or abort "usage: check_user_pointers.rb <repo-root>"

path = File.join(root, "kernel/syscalls.c")
abort "cannot read kernel/syscalls.c" unless File.file?(path)

errors = []
lines = File.readlines(path, mode: "rb")

# --- rule 1: no pointer cast of a syscall argument -------------------------
#
# (char *)arg1, (const char *)arg2, (void *)arg3, (uint8_t *)arg1 - all of
# these turn a number the kernel has never validated into something it will
# dereference.
# A pointer cast, optionally through an integer cast first: (char *)arg1 and
# (const char *)(uintptr_t)arg1 are the same mistake.  The inner group is a real
# alternation - an earlier version used a literal "|" here, which quietly matched
# nothing but the simplest form.
POINTER_CAST = /
  \(\s*(?:const\s+|volatile\s+)*[A-Za-z_][\w\s]*\*+\s*\)     # the pointer cast
  \s*
  (?:\(\s*(?:uintptr_t|intptr_t|unsigned\s+long|size_t|long)\s*\)  # through an
   |0x[0-9a-fA-F]+u?)\s*                                       # integer cast
  \barg[123]\b
/x

lines.each_with_index do |line, i|
  next unless line =~ POINTER_CAST
  next if line.lstrip.start_with?("*", "/*", "//")
  errors << "kernel/syscalls.c:#{i + 1}: a syscall argument is cast to a " \
            "pointer. That is what a user address looks like once the kernel " \
            "trusts it, and nothing in the type says it was never checked. " \
            "Copy the data in first - muzix_copy_user_string() for a string, " \
            "muzix_copy_user_to_current() for a value - and pass the kernel " \
            "buffer: #{line.strip}"
end

# --- rule 2: an argument reaching a call is cast or used as a value ---------
#
# Strip the casts and the boolean tests, then look for an argument still sitting
# bare inside a call's parentheses.
CASTED = /
  \((?:const\s+|volatile\s+)*[A-Za-z_][\w\s]*\)               # the cast
  \s*
  (?:\(\s*(?:uintptr_t|intptr_t|unsigned\s+long|size_t|long)\s*\))?  # optional
  \s*
  \(?                                                       # grouping "("
  \s*
  \barg[123]\b
/x

lines.each_with_index do |line, i|
  next if line.lstrip.start_with?("*", "/*", "//")

  # Blank out the argument once it has been cast, including the common
  # `(uint16_t)(arg2 + n * 2)` form, so what is left is an argument that went
  # into a call uncast.
  stripped = line.gsub(CASTED, " ")

  # A control-flow condition is not a call: `if (arg1)` and
  # `if (arg3 < 0 || arg3 > 256)` are value uses. Drop the opening paren of
  # if/while/switch/for so an argument there is no longer preceded by one.
  stripped = stripped.gsub(/\b(if|while|switch|for)\s+\(/, '\\1 ')

  # An argument sitting directly after "(" or "," is a call parameter.  A value
  # use does not: `!arg1` has a "!" in between, `arg3 < 0` is preceded by
  # whitespace and an operator, `if (arg2)` was a condition - which is why the
  # "!" is excluded and why only "(" and "," count.
  next unless stripped =~ /[(,]\s*arg[123]\s*[,)]/

  errors << "kernel/syscalls.c:#{i + 1}: a syscall argument reaches a call " \
            "without a cast, so the call cannot be told apart from one that " \
            "takes a process address. Cast it to say what it is - (int) for a " \
            "handle, (uint16_t) for a length: #{line.strip}"
end

# --- the kernel-side landing buffers exist ----------------------------------
syscalls = File.read(path, mode: "rb")
%w[k_path k_path2 k_argv_buf k_ioctl].each do |buf|
  next if syscalls =~ /^\s*char\s+#{buf}\[|^\s*uint8_t\s+#{buf}\[/m
  errors << "kernel/syscalls.c: the kernel-side landing buffer `#{buf}` is " \
            "gone. The filesystem entry points and exec_loader walk their path " \
            "and argv arguments themselves, so a user's pointer must never be " \
            "handed to them; it has to be copied into a kernel buffer first."
end

if errors.empty?
  puts "  x no user address used without crossing a copy-in boundary: OK"
  exit 0
end

warn "FAIL: a user address can reach the kernel without being copied in"
errors.each { |e| warn "  #{e}" }
exit 1
