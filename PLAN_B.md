# Plan B: Fork All Servers Before Scheduler Entry

> **Status header (2026-10-14):** the design below has landed.
> `kernel_main()` forks FS (slot 2), MM (slot 3), PM (slot 4) and the shell
> (slot 1) in that order and enters `muzix_kernel_loop_run()` exactly once,
> which never returns (`kernel/kernel_main.c:295-348`). The boot-phase-return
> machinery this plan set out to remove — `muzix_return_to_kernel_loop` and
> `muzix_kernel_sp_save` — has no references left in the tree, and the
> userspace `SYS_EXEC` handler now drives the same loader
> (`kernel/syscalls_ipc.c`, `muzix_handle_exec()`, `:1010`). What remains
> open: `g_loop.in_boot_phase` is still set at `kernel/kernel_main.c:347` but is
> read by nothing (`kernel/kernel_loop.h:24`), and the boot exec loads take the
> loader's direct-ROM path because loader IPC mode is enabled only after all
> four boot forks (`kernel/kernel_main.c:341`); the userspace `SYS_EXEC`
> handler likewise clears IPC mode (`kernel/syscalls_ipc.c:1100`). See
> `AUDIT_FIX_PLAN.md` for the wave-1
> notes. Line numbers below are the plan's original pre-fix references and now
> rot; where the text cites them, the current symbol is named beside them.

## Objective
Eliminate the broken "boot phase return" mechanism by forking all userspace servers (FS, MM, PM, Shell) in `kernel_main` BEFORE entering the scheduler, then entering `muzix_kernel_loop_run()` once as the permanent idle process.

## Current Problem
The "fork FS → run 10 scheduler iterations → return to kernel_main → fork others" design requires a boot phase return from syscall to `kernel_main`, which is broken due to stack corruption in the syscall return path.

## Target Architecture

```
kernel_main():
  1. Initialize all kernel subsystems (UART, CTC, MM, IPC, proc_table, etc.)
  2. Set up syscall handler
  3. Initialize Zeta state with kernel map
  4. FORK ALL SERVERS:
     - kernel_fork_server("fs_server", fs_argv, 2, 1)
     - kernel_fork_server("mm_server", mm_argv, 3, 2)
     - kernel_fork_server("pm_server", pm_argv, 4, 3)
     - kernel_fork_server("shell", shell_argv, 1, 4)
  5. Register all in IPC table
  6. Enable IPC mode for exec_loader
  7. g_loop.in_boot_phase = 1  (affects priority only, not return logic)
  8. muzix_kernel_loop_run(&g_loop)  // NEVER RETURNS - becomes idle process
```

## Files to Modify

### 1. `kernel/kernel_main.c`
**Changes** (the plan's "lines 232-242" / "lines 244-271" / "line 220" were
written against the pre-fix file; the current `kernel_main()` is 349 lines —
status verified 2026-10-14, symbols named):
- Remove the 10-iteration boot-phase scheduler loop in `kernel_main()` —
  LANDED: the loop is gone; the fork sequence now runs straight through
  (`kernel/kernel_main.c:295-341`)
- Move the MM, PM and shell forks before the FS fork's IPC registration —
  LANDED: order is FS -> MM -> PM -> shell, each `kernel_fork_server()`
  followed by its `muzix_ipc_register_server()`
  (`kernel/kernel_main.c:295-338`)
- Keep FS fork first (it's the dependency for others) — LANDED
  (`kernel/kernel_main.c:295-305`)
- `g_loop.in_boot_phase = 1` after all servers forked, not before the loop —
  LANDED: single assignment, now read by nothing (`kernel/kernel_main.c:347`)
- Single call: `muzix_kernel_loop_run(&g_loop)` at end — LANDED
  (`kernel/kernel_main.c:348`)
- `muzix_exec_enable_ipc()` after all forks — LANDED
  (`kernel/kernel_main.c:341`)

**Order of operations**:
```c
// 1. Fork FS server (must be first - others need it)
kernel_fork_server("fs_server", fs_argv, 2, 1);
muzix_ipc_register_server(&g_ipc, 2, 1);

// 2. Fork MM server
kernel_fork_server("mm_server", mm_argv, 3, 2);
muzix_ipc_register_server(&g_ipc, 3, 2);

// 3. Fork PM server
kernel_fork_server("pm_server", pm_argv, 4, 3);
muzix_ipc_register_server(&g_ipc, 4, 3);

// 4. Fork Shell
kernel_fork_server("shell", shell_argv, 1, 4);
muzix_ipc_register_server(&g_ipc, 1, 4);

// 5. Enable IPC for exec_loader (now all servers exist)
muzix_exec_enable_ipc();

// 6. Enter scheduler forever
g_loop.in_boot_phase = 1;
muzix_kernel_loop_run(&g_loop);
```

### 2. `kernel/kernel_loop.c`
**Changes** (the plan's "lines 381-396" pre-date the rewrite; that span is now
part of the idle-halt block in `muzix_kernel_loop_run()`
(`kernel/kernel_loop.c:368-390`). Status verified 2026-10-14):
- Remove boot phase return logic from `muzix_kernel_switchout()`:
  - Remove `extern uint8_t muzix_return_to_kernel_loop;` check — LANDED: the
    symbol does not exist anywhere in the tree
  - Remove `loop->state = MUZIX_KERNEL_LOOP_IDLE; return;` on that flag —
    LANDED
- Remove `in_boot_phase` special handling in `muzix_kernel_loop_run()` — LANDED:
  no read of `in_boot_phase` remains; only the write at
  `kernel/kernel_main.c:347`
- `muzix_kernel_switchout()` should only:
  - Check `should_idle()` → halt — LANDED: `muzix_proc_table_should_idle()`
    (`kernel/proc_table.c:236`)
  - Check finite deadlines → return if only infinite parked — LANDED:
    `muzix_proc_table_has_finite_deadline()` (`kernel/proc_table.c:271`)
  - Pick next → enter userspace — LANDED

### 3. `kernel/syscalls_ipc.c`
**Changes** (verified 2026-10-14):
- In `SYS_IPC_RECEIVE` handler: remove boot phase check — LANDED: no boot-phase
  check or `muzix_return_to_kernel_loop` write remains in the receive path
- When parking for receive, just park normally (no `muzix_return_to_kernel_loop = 1`)
  — LANDED: parking goes through `muzix_proc_table_park()`
- The scheduler will naturally pick next ready process — LANDED

### 4. `platform/zeta-v2/syscall_entry.s`
**Changes** (verified 2026-10-14):
- Remove `syscall_return_check_boot` entirely — LANDED: no such label and no
  `muzix_return_to_kernel_loop` reference remains in the file
- Normal syscall return always goes to `syscall_return_normal` — LANDED

### 5. `kernel/kernel_main.c` globals
**Remove** (the plan's "line 66" / "line 67" pre-date the rewrite; that span is
now the fork-stack comment and `extern muzix_fork_stack_len`
(`kernel/kernel_main.c:63-70`). Verified 2026-10-14):
- `extern uint8_t muzix_return_to_kernel_loop;` (line 66) — LANDED: symbol gone
- `uint16_t muzix_kernel_sp_save = 0;` (line 67) — LANDED: symbol gone
- Keep only if used elsewhere (check grep) — confirmed unused; nothing to keep

### 6. `kernel/kernel_loop.h` / `kernel/kernel_loop.c`
**Changes** (verified 2026-10-14):
- Remove `in_boot_phase` special handling in `muzix_kernel_loop_run()` — LANDED
- Remove `muzix_return_to_kernel_loop` manipulations — LANDED
- Keep `in_boot_phase` field if it affects priority queue (SERVER_Q vs USER_Q) —
  PARTIAL: the field still exists (`kernel/kernel_loop.h:24`) and is set once
  (`kernel/kernel_main.c:347`), but no code reads it, so it affects nothing;
  removing it is the remaining cleanup

### 7. `kernel/proc_table.c` / `kernel/proc_table.h`
**Changes**:
- Keep `muzix_proc_table_has_finite_deadline()` - still needed for `muzix_kernel_switchout()`
- Remove any boot-phase-specific logic if present

## Expected Behavior

1. **kernel_main** forks all 4 servers sequentially
2. Each `kernel_fork_server` calls `muzix_exec_load()` which does IPC reads from FS server
3. FS server is NOT running yet during these IPC reads - exec_loader must handle this
4. After all forks, `muzix_kernel_loop_run()` enters scheduler
5. **First scheduler pass**: picks FS server (slot 2, SERVER_Q) - it runs init, then parks on `ipc_receive()`
6. **Next pass**: picks MM server (slot 3) - loads, parks
7. **Next pass**: picks PM server (slot 4) - loads, parks
8. **Next pass**: picks Shell (slot 1, USER_Q) - runs interactive shell
9. Normal priority scheduling thereafter

## Critical Dependencies

**exec_loader IPC reads during boot** (before scheduler runs):
- `exec_ipc_open_path()`, `exec_ipc_read()`, `exec_ipc_fstat()`, `exec_ipc_close()`
- These call `muzix_ipc_sendrec()` directly to FS server (slot 2)
- FS server is NOT in scheduler yet - it's just been forked, not run
- **Solution**: exec_loader IPC must be synchronous and handle "server not ready" gracefully
- Current code already has wakeup fix: after `muzix_ipc_sendrec`, calls `muzix_proc_table_ready()` if result >= 10
- But FS server hasn't been scheduled yet, so it's not "parked" - it's just not run
- **Fix**: In `kernel_fork_server`, after `muzix_exec_load()`, the child process is in READY queue. The IPC send from exec_loader will find it not ready (not in IPC wait). This needs handling.

**Actually**: `muzix_exec_load()` runs the exec loader which does the IPC reads. The child process is created by `muzix_system_service_fork()` and made ready. The exec_loader runs in kernel context (not the child's context). So when exec_loader does `muzix_ipc_sendrec()`, it's the KERNEL sending to FS server. FS server is in ready queue but hasn't run yet. The IPC send should work - it will queue the message to FS server's mailbox. When FS server runs, it will find the message.

Wait, let me check: `muzix_ipc_sendrec` from kernel context (exec_loader) - does it work without scheduler?

Looking at `exec_ipc_sendrec`: it calls `muzix_ipc_sendrec(ipc, current_slot, server_slot, &msg)`. `current_slot` is the exec_loader's slot (which is kernel slot 0?). Actually exec_loader runs in `kernel_fork_server` which is called from `kernel_main` - not a process context.

This might be the issue. The exec_loader IPC calls need a valid sender slot.

Let me check the current implementation... The exec_loader uses `g_exec_slot` as current_slot. This is set to the target slot (the one being loaded). So when loading FS server (slot 2), it sends as slot 2. But slot 2 is the FS server itself - it's sending to itself?

Actually looking at `exec_ipc_open_path`: `int current_slot = g_exec_slot; int server_slot = 2;`. So it sends FROM the slot being loaded TO server_slot 2 (FS server). That makes sense - the loading process "asks" FS server to open the file.

But the process isn't running yet - it's just been created and made ready. The message will sit in FS server's mailbox. When scheduler picks FS server, it will receive the message.

This should work! The message is queued, FS server runs, does `ipc_receive`, gets the message, processes it, replies.

## Verification Steps

1. Build: `make rom` → all gates pass
2. Emulator test: run with sufficient cycles to see all servers start
3. Expected trace:
   ```
   KM
   ROMFS_BLOCK_COUNT=0300
   MNT
   SVC
   RDY
   IPC
   KERNEL: forking FS server
   ... exec load traces ...
   KERNEL: FS server forked
   KERNEL: forking MM server
   ... exec load traces ...
   KERNEL: MM server forked
   KERNEL: forking PM server
   ... exec load traces ...
   KERNEL: PM server forked
   KERNEL: forking shell
   ... exec load traces ...
   KERNEL: shell forked
   KERNEL: entering scheduler
   ENTER USER: slot=02 ep=8000 banks=20 25 24 23  (FS server)
   KM
   MNT
   SVC
   RDY
   IPC
   ENTER USER: slot=03 ep=8000 banks=...  (MM server)
   ...
   ENTER USER: slot=04 ep=8000 banks=...  (PM server)
   ...
   ENTER USER: slot=01 ep=8000 banks=...  (Shell)
   ```

## Risk Mitigation

1. **exec_loader IPC during boot**: Test that messages queue correctly to not-yet-running servers
2. **FS server must be first**: It's the dependency for all other loads
3. **Shell slot 1**: Currently shell is slot 1 (USER_Q), FS is slot 2 (SERVER_Q). Priority order should be SERVER_Q > USER_Q so FS/MM/PM run before shell.
4. **in_boot_phase flag**: Keep it if it affects priority (e.g., SERVER_Q processes get priority during boot)

## Agent Instructions

Follow Agents.md:
- Read AGENTS.md for project context
- Implement changes incrementally
- Run `make rom` and `make check` after each logical group of changes
- Test in emulator with sufficient cycles
- No conversational output - direct technical results only