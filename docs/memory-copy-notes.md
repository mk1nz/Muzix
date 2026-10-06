# User-memory copy semantics and the Muzix Zeta backend

## MINIX reference behavior

MINIX `SYS_COPY` identifies source and destination processes, segments,
virtual addresses, and a byte count. The system task resolves each address
through the process memory maps before copying. MINIX is reference material
here; Muzix does not implement the complete MINIX message ABI or all of its
copy semantics.

## Muzix implementation

Muzix routes user-memory transfers through its memory-manager copy interface.
The MM layer validates and resolves process mappings, then calls the Zeta
platform primitives for banked access. Those primitives temporarily install a
map and restore the complete kernel map before returning. The bank selectors
are write-only in the strict hardware model; saved software state, not port
readback, is used for restoration.

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
