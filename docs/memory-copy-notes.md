# User-memory copy semantics and the Muzix Zeta backend

## MINIX reference behavior

MINIX `SYS_COPY` identifies source and destination processes, segments,
virtual addresses, and a byte count. The system task resolves each address
through the process memory maps before copying. MINIX's SYS_COPY is the
reference model here; Muzix implements its own MM server with a page allocator
and copy interface (not the full MINIX copy semantics).

## Muzix implementation

Muzix routes user-memory transfers through its memory-manager copy interface.
The MM layer validates and resolves process mappings, then calls the Zeta
platform primitives for banked access: `zeta_copy_user_to_kernel`,
`zeta_copy_kernel_to_user`, `zeta_copy_page_to_kernel` and
`zeta_copy_rom_page_to_kernel`, defined in
`platform/zeta-v2/context_switch.c` and reached only through the
`muzix_mm_*` entry points in `mm/` — `tools/check_memory_manager.rb` makes any
other caller a build failure. Those primitives temporarily install a
map and restore the complete kernel map before returning. The bank selectors
are write-only in the strict hardware model; saved software state, not port
readback, is used for restoration. As of 2026-10-14 `kernel/rom_read.c` no
longer writes a bank register itself; it routes through
`muzix_mm_copy_rom_page_to_kernel()` and `muzix_mm_copy_kernel_to_map()`.

The production path is implemented in `mm/mm_copy.c` and
`platform/zeta-v2/context_switch.c`. The similarly named adapter under
`platform/zeta-v2/test_kernel/` is a standalone test model and is not the
production copy path.

The invariant is that logical segment addresses, physical page IDs, and
CPU-visible window addresses remain distinct. Higher-level code must use the
MM interface rather than manipulate bank registers or interpret a user
pointer as a physical address.

FUZIX's Zeta bank-switching conventions were consulted for the hardware
backend, but the production copy path and its process mappings are Muzix code;
the FUZIX process model is not used here. Selected low-level Z80 saved-stack
and switch-out patterns elsewhere in the kernel were adapted from FUZIX and
are listed in [`hardware-notes.md`](hardware-notes.md).
