#!/usr/bin/env ruby
# frozen_string_literal: true

# Check the userspace syscall context for the two failure modes that both cost
# real debugging time on this port, and that neither a link-map check nor a
# runtime guard can see.
#
# 1. A field the handler reads is never assigned anywhere.
#    muzix_handle_userspace_syscall() reaches the exec loader through
#    ctx->exec_loader. Nothing ever set that field -
#    muzix_zeta_syscall_runtime_init() fills in kernel and fs, and the platform
#    layer cannot see kernel/exec_loader.c. The handler's
#    `if (ctx->exec_loader && ...)` was therefore false, exec reported success
#    without loading anything, and `ls` printed the shell's banner because it
#    never ran. A NULL function pointer behind a struct field is invisible: the
#    code compiles, links, and does nothing.
#
# 2. The running process's identity is derived twice.
#    The prologue set ctx->pid from the process table, and a second block
#    immediately overwrote it from muzix_kernel_loop_current_pid() - a pid
#    cached once at boot. Every syscall was then attributed to the boot process,
#    so the child's exec loaded the new program into the parent's slot. The
#    duplication looks like redundant bookkeeping at the call site.
#
# Usage: check_syscall_context.rb <repo-root>
#
# This is a ratchet, not a proof. It reads assignments, not dataflow: it sees
# that one exists, never that it happens on every path.

root = ARGV[0] or abort "usage: check_syscall_context.rb <repo-root>"

errors = []

# --- the field list, from the struct itself ---------------------------------
header = File.read(File.join(root, "kernel/syscalls.h"))
struct = header[/^typedef struct [^{]*muzix_userspace_syscall_context\s*\{(.*?)^\}/m]
abort "could not find muzix_userspace_syscall_context_t in kernel/syscalls.h" if struct.nil?

fields = []
struct.each_line do |line|
  t = line.strip
  next if t.empty? || t.start_with?("typedef") || t.include?("}")
  # The name is the last word before any array suffix and the semicolon; a
  # pointer star sits between the type and the name.
  m = t.match(/\b(\w+)\s*(?:\[\w+\])?\s*;\z/)
  fields << m[1] if m
end
abort "parsed no fields from the context struct" if fields.empty?

# --- the handler, and its prologue ------------------------------------------
syscalls = File.read(File.join(root, "kernel/syscalls.c"))
body = syscalls[/^int32_t muzix_handle_userspace_syscall\(.*?^\}/m]
abort "could not find muzix_handle_userspace_syscall in kernel/syscalls.c" if body.nil?

# The prologue is everything before the first syscall-number dispatch. Identity
# or credentials set after that point would be conditional on which call was
# made, which is the bug this exists to catch.
prologue = body.split(/if \(syscall_num ==/).first

assigned = Hash.new { |h, k| h[k] = [] }

# Assignments in the handler prologue: the credential fields are refreshed from
# the process slot on every call, and that is a legitimate initialisation.
prologue.scan(/\bctx->(\w+)\s*=/).flatten.each do |field|
  assigned[field] << "prologue of muzix_handle_userspace_syscall"
end

# Assignments at startup. Both `->` and `.` reach the struct: the runtime uses
# a pointer, kernel_main.c names the global directly.
{
  "platform/zeta-v2/syscall_runtime.c" => /(?:->|\.)userspace\.(\w+)\s*=/,
  "kernel/kernel_main.c"                => /(?:->|\.)userspace\.(\w+)\s*=/,
  "platform/zeta-v2/syscall_entry.c"   => /\buserspace->(\w+)\s*=/
}.each do |rel, pattern|
  path = File.join(root, rel)
  next unless File.file?(path)
  File.foreach(path).with_index(1) do |line, n|
    line.scan(pattern).flatten.each { |f| assigned[f] << "#{rel}:#{n}" }
  end
end

fields.each do |field|
  next if assigned.key?(field)
  errors << "userspace context field `#{field}` is never assigned, in the " \
            "startup path or in the handler's prologue. The handler reads it, " \
            "and a field left unset makes the code through it silently do " \
            "nothing - which is how exec_loader went missing and exec reported " \
            "success without loading a program."
end

# --- the identity fields, exactly once in the prologue ----------------------
%w[pid ppid].each do |field|
  count = prologue.scan(/\bctx->#{field}\s*=/).size
  next if count == 1
  where = count.zero? ? "never" : "#{count} times"
  errors << "ctx->#{field} is assigned #{where} in the prologue of " \
            "muzix_handle_userspace_syscall(); it must be exactly once. The " \
            "running process's identity comes from the process table, and a " \
            "second source - the loop's boot-time pid cache - overwrites the " \
            "correct answer and misattributes every syscall to the boot " \
            "process."
end

if errors.empty?
  puts "  x userspace syscall context fully initialised, identity set once: OK"
  exit 0
end

warn "FAIL: the userspace syscall context is not self-consistent"
errors.each { |e| warn "  #{e}" }
exit 1
