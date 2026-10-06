# Deleted tests, and why

Six test files asserted behaviour the code deliberately moved away from. They
were kept failing, and a permanently red gate is worse than a missing one: it
reads as a broken build and teaches people to ignore the runner.

They were removed on 2026-09-30, after each reason below was measured rather
than assumed. If any of the behaviour they assert is ever wanted back, this file
is the record of what was given up and why.

## `mm/test_mproc_model.c` — wanted the window-1 bug back

Asserted the page map `{0,1,2,3}`. `muzix_mproc_to_zeta_map()` derives it from
`exec_loader.c` and returns `{0x20, stack, text, 0x23}`. The old map banked a
process's own text into window 1, which is how a copy sent the TTY the
process's instructions to execute. The comment at `mm/mproc_model.c:141` records
that as the fix. The test asserted the defect.

## `mm/test_memory_map.c` — same map, and it tests code not in the image

Same `{0,1,2,3}` expectation, plus a `umap` case expecting
`muzix_umap(DATA, 0x200) == 0x1200`, which is a base plus an offset.
`mm/memory.c` is not in `KERNEL_C_MODULES`, so neither `muzix_proc_to_zeta_map`
nor `muzix_umap` exists in the image this test claims to cover.

## `kernel/test_bootstrap.c`, `test_init_task.c`, `test_kernel_loop.c` — a chain nothing calls

All three fail the same way: `muzix_bootstrap_current_pid()` is not the
registered pid after a run, because `runtime_state.pick_next()` returns early on
a NULL table, and the table pointer is injected by `muzix_kernel_startup_init()`
(`kernel/startup.c:25`), which these tests bypass. `muzix_bootstrap_run()` and
the `muzix_init_task_*` half are called from nowhere; `startup.c` registers
through `muzix_proc_table_*` directly. `muzix_kernel_loop_run` IS live, but the
loop in the image is driven with `startup.current_pid` and a table pointer that
`startup_init` sets, and the test supplies neither.

Three copies of a chain that was replaced. `test_kernel_loop.c` was the only one
with a live subject, and the part that is live is exercised by the running
system rather than by this test.

## `fs/test_allocator_store.c` — wanted the mount-time safety gate removed

Asserted that `muzix_fs_allocator_load()` accepts a zone bitmap in which the
metadata blocks are free. The test builds one with
`muzix_fs_zone_allocator_init(&source, 32, 4)`, which zeroes `used`, allocates
a zone, so bits 0-3 read clear and the loader refuses it — measured `rc = -1`.
**The code is right.** Every writer of that block in the tree reserves the
zones: `muzix_fs_volume_reserve_metadata()` (`fs/volume.c:64`) on the format
path, `tools/make_minixfs.rb:106-114` on the image path, and
`muzix_fs_volume_flush` (`fs/volume.c:45`) is the only caller of
`muzix_fs_allocator_save`, so a reserved bitmap is the only bitmap this code can
persist. `fs/volume.c:122-125` turns a refusal into a failed mount, next to a
second geometry cross-check against the superblock (`fs/volume.c:131-135`).

Measured: the same save/load round-trips and reads zone 4 as used once zones 0-3
are reserved; format produces bits 0-4 set and the loader accepts it; mount
returns -1 on an under-reserved bitmap; and the shipped
`build/muzix-minifs.bin` has bits 0-4 set with `first_data_zone 5` in both block
2 and the superblock.

The assertion would have been that the mount-time safety gate is absent.

## What is worth keeping

`mm/mproc_model.c` and `mm/memory_map.c` name real functions, so the subject is
still worth a test — a new one, written against the current map. That is new
coverage rather than resurrected coverage, and it is not in the runner yet.
