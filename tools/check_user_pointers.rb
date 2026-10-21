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
# address enters.  That layer is TWO files: kernel/syscalls.c is still the
# boundary - it owns muzix_copy_user_*() and the kernel-side landing buffers -
# but the user-syscall dispatcher and every handler that touches a user address
# moved to kernel/syscalls_ipc.c with the FS/MM/PM routing (the deferred
# copy-out, fork, exec, the six IPC copies).  A pointer derived from an argument
# in the dispatcher is the same dereference it always was in syscalls.c, so both
# files are scanned.  check_syscall_context.rb made the same move; this gate
# kept the narrower list, and the file list and the rule that reads it drifted
# apart.
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

SYSCALL_FILES = %w[kernel/syscalls.c kernel/syscalls_ipc.c].freeze

errors = []
sources = {}
SYSCALL_FILES.each do |rel|
  path = File.join(root, rel)
  abort "cannot read #{rel}" unless File.file?(path)
  sources[rel] = File.readlines(path, mode: "rb")
end

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
   |0x[0-9a-fA-F]+u?)?\s*                                      # integer cast
  \barg[123]\b
/x

sources.each do |rel, lines|
  lines.each_with_index do |line, i|
    next unless line =~ POINTER_CAST
    next if line.lstrip.start_with?("*", "/*", "//")
    errors << "#{rel}:#{i + 1}: a syscall argument is cast to a " \
              "pointer. That is what a user address looks like once the kernel " \
              "trusts it, and nothing in the type says it was never checked. " \
              "Copy the data in first - muzix_copy_user_string() for a string, " \
              "muzix_copy_user_to_current() for a value - and pass the kernel " \
              "buffer: #{line.strip}"
  end
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

sources.each do |rel, lines|
  lines.each_with_index do |line, i|
    next if line.lstrip.start_with?("*", "/*", "//")

    # The syscall layer's own dispatcher family is not a violation.  In
    # syscalls_ipc.c the routing calls - muzix_handle_ids(ctx, syscall_num,
    # arg1), muzix_handle_signal(ctx, arg1, arg2) and the six others - hand the
    # raw arguments to the per-syscall handler, whose parameters ARE the trap's
    # int32_t values: a pid, a length, a mask, a slot.  That is the ABI being
    # continued one link further down, and the number of such call sites is the
    # dispatcher's normal shape, not an accident.  Rule 2 was calibrated against
    # kernel/syscalls.c, whose boundary function reads `(void)arg1;` and routes
    # nothing, so it never had to face them; widening the file list without this
    # exemption reports eight deliberate sites on the first run, which is how a
    # gate gets switched off.  It is narrow on purpose: the exemption names the
    # dispatcher family that receives the tuple by contract, so a copy helper
    # (muzix_copy_user_*), a data path or any other callee still fails, and rule
    # 1 above still refuses a cast anywhere, including in these calls.
    next if line =~ /\b(?:muzix_handle_\w+|muzix_dispatch_userspace_syscall)\s*\(/

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

    errors << "#{rel}:#{i + 1}: a syscall argument reaches a call " \
              "without a cast, so the call cannot be told apart from one that " \
              "takes a process address. Cast it to say what it is - (int) for a " \
              "handle, (uint16_t) for a length: #{line.strip}"
  end
end

# --- the kernel-side landing buffers exist ----------------------------------
# Both files are read for the buffers: they lived in syscalls.c when this check
# was written, and a buffer that moved with the dispatcher has to be found in
# its new home or the check reports a regression that is not one.
syscalls = SYSCALL_FILES.map { |rel| File.read(File.join(root, rel), mode: "rb") }.join
server_path = File.join(root, "servers/fs/fs_server.c")
server_code = File.file?(server_path) ? File.read(server_path, mode: "rb") : ""

%w[k_path k_path2 k_argv_buf k_ioctl].each do |buf|
  next if syscalls =~ /^\s*char\s+#{buf}\[|^\s*uint8_t\s+#{buf}\[/m
  # In IPC mode, the server uses muzix_copy_from_user for these
  # Check that the server copies user pointers for path/argv/ioctl arguments
  server_handles = case buf
                   when 'k_path', 'k_path2'
                     server_code =~ /muzix_copy_from_user.*msg->m1_p1.*path/
                   when 'k_argv_buf'
                     server_code =~ /muzix_copy_from_user.*msg->m1_p1.*argv/ || server_code =~ /muzix_copy_from_user.*msg->m1_p2.*argv/
                   when 'k_ioctl'
                     server_code =~ /muzix_copy_from_user.*msg->m1_p1.*ioctl/
                   end
  next if server_handles
  errors << "kernel/syscalls.c: the kernel-side landing buffer `#{buf}` is " \
            "gone from the syscall layer, and no server copies the caller's " \
            "pointer for it. The filesystem entry points and exec_loader walk " \
            "their path and argv arguments themselves, so a user's pointer must " \
            "never be handed to them; it has to be copied into a kernel buffer " \
            "first."
end

if errors.empty?
  puts "  x no user address used without crossing a copy-in boundary: OK"
  exit 0
end

warn "FAIL: a user address can reach the kernel without being copied in"
errors.each { |e| warn "  #{e}" }
exit 1
