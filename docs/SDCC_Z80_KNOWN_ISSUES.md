# SDCC Z80 Known Issues

Documented SDCC compiler and code-generation problems encountered during
Muzix development, with verified workarounds. Intended as a practical
reference for anyone writing C code for the Z80 target.

## Environment

- SDCC version: 4.5.0 #15242 (Mac OS X x86_64)
- Compiler flags: `-mz80 --opt-code-size` (kernel), `-mz80` (userspace)
- Target: Zilog Z80, 8-bit registers, 16-bit address space

---

## Issue 1: Indexed access to `struct dirent` character array

### Original C expression

```c
struct dirent entry;
int di;
/* ... */
entry.name[di]          /* indexed access into entry.name[15] */
```

### Observed generated Z80 assembly

From `apps/cgi_index.lst` (compiled with `-mz80 --opt-code-size`):

```asm
    ; apps/cgi_index.c: if (*entry.name == '.')
    ld      a, -28 (ix)   ; loads entry.inode high byte, NOT entry.name[0]
    sub     a, #0x2e
```

The struct `dirent` layout is:

| Offset | Field | Size |
|--------|-------|------|
| 0      | `inode` | uint16_t (2 bytes) |
| 2      | `name`  | char[15] |

So `entry.name[0]` is at `ix - 27`, `entry.name[1]` at `ix - 26`, etc.
SDCC emitted `LD A, -28(ix)`, which reads `entry.inode`'s high byte at
offset -28 instead of `entry.name[0]` at offset -27 — an off-by-one error
in computing the address of `entry.name`.

### Why the address was incorrect

SDCC appears to compute `&entry.name` as `&entry + 1` instead of
`&entry + sizeof(entry.inode)` (= `&entry + 2`). The result is that every
indexed access `entry.name[di]` reads one byte too early, corrupting the
first name byte read.

### Corrected C expression

```c
/* Pointer parameter instead of indexed struct member access */
static int is_skip_entry(const char *name) {
    if (name[0] == 0) return 1;
    if (name[0] == '.' && name[1] == 0) return 1;
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) return 1;
    return 0;
}

/* call site: */
is_skip_entry(entry.name)
```

### Corresponding correct assembly

From the fixed build:

```asm
    ; is_skip_entry(entry.name) - name arrives as a pointer parameter
    ld      l, -10 (ix)   ; loads entry.name address low byte
    ld      h, -9  (ix)   ; loads entry.name address high byte
    call    _is_skip_entry
```

Inside `is_skip_entry`, the parameter `name` is accessed via `ix`-relative
offsets, which SDCC computes correctly for pointer parameters.

### Status: Confirmed compiler/code-generation issue

Confirmed by direct comparison of the assembly listing before and after the
workaround. The pre-fix code reads `entry.inode` high byte where
`entry.name[0]` should be; the post-fix code reads `entry.name[0]` correctly.

### Conditions / limitations

- Observed with `-mz80 --opt-code-size` on SDCC 4.5.0.
- Not all SDCC array indexing is affected — this is specific to
  indexed access into a `char[]` member of a struct where the preceding
  field is a multi-byte scalar (`uint16_t inode`).
- Other array indexing patterns in the same file were not observed to fail.

---

## Issue 2: Multiple `send()` calls with runtime arguments inside a loop

### Observed pattern

Repeated calls inside a loop of the form:

```c
send(sock, entry.name, cgi_strlen(entry.name));
```

where `sock` is a stack-relative `int`, `entry.name` is a stack-relative
`char*`, and `cgi_strlen(entry.name)` is a runtime `uint16_t`.

### Observed symptoms

- After the first one or two loop iterations, subsequent directory-entry
  names were transmitted as all-zero (NUL) bytes.
- The failure point changed depending on surrounding code size and
  structure: sometimes entry 2 failed, sometimes entry 3, etc.
- With identical source changes, the symptom shifted between iterations.

### Generated assembly evidence

No complete assembly trace was captured for this issue. Individual
`send()` call sites were inspected in `apps/cgi_index.lst`; SDCC's
register allocation for the three runtime arguments varied between call
sites depending on surrounding code, but no single instruction sequence
was isolated as the definitive cause.

### Workaround

Build the complete response in a local buffer and issue one `send()`
call at the end:

```c
char html[512];
uint16_t html_len = 0;
char *p = html;

/* Build the entire response byte by byte ... */
while (*s) { *p++ = *s++; html_len++; }

/* Single send at the end */
send(g_cgi_sock, (const uint8_t *)html, html_len);
```

### Buffer size and bounds considerations

- The buffer must be large enough for the complete response. In
  `apps/cgi_index.c`, `char html[512]` is used; the response is ~450 bytes.
- No bounds checking is performed during the build phase; the caller must
  ensure the buffer is oversized for the maximum expected response.
- `html_len` is tracked incrementally and used as the `send()` length,
  so only the actually-built portion is transmitted.

### Status: Observed workaround, underlying cause unconfirmed

The symptoms are confirmed (NUL bytes where directory names should be,
failure point shifts with code layout). The specific compiler mechanism
(SDCC register allocation or argument setup for multi-argument runtime
`send()` calls) was not isolated from the generated assembly, so the
underlying cause remains unconfirmed.

### Conditions / limitations

- Observed with `-mz80 --opt-code-size` on SDCC 4.5.0.
- The workaround (single-buffer build + single send) was verified to
  produce correct output for all five `/doc` directory entries across
  repeated requests.
- Does not imply that all multi-argument `send()` calls are affected —
  only repeated calls inside a loop with runtime local variables.

---

## Coding Guidelines for Muzix Z80 C Code

### Checklist

- **Inspect generated assembly** when runtime behavior contradicts correct
  C semantics. Use `apps/*.lst` (SDCC listing output) or `build/kernel.map`
  to verify what SDCC actually emits.
- **Prefer explicit pointer arithmetic** where a verified compiler issue
  exists (e.g., pass `entry.name` as a `const char *` parameter instead of
  using `entry.name[di]` indexed access).
- **Keep runtime argument handling simple** in critical loops when a
  reproducible issue has been observed. Fewer runtime arguments per call
  reduces the chance of register-allocation confusion.
- **Validate buffer sizes and pointer boundaries.** When building responses
  in a local buffer, ensure the buffer is oversized for the maximum expected
  content and track the length incrementally.
- **Add regression tests** for every confirmed workaround. The
  `kernel/test_rom_readdir.c` host test verifies directory-entry indexing
  independently of the Z80 target.
- **Avoid unnecessary compiler workarounds without evidence.** Do not
  restructure code based on suspicion alone; confirm the failure by
  observing the wrong bytes at runtime and inspecting the assembly listing.

### SDCC version note

These issues were observed with SDCC 4.5.0 #15242 using `-mz80`. Future
SDCC versions may behave differently. Re-verify any workaround if the
toolchain is upgraded.

---

## Related files

- `apps/cgi_index.c` — contains both workarounds (pointer parameter for
  `is_skip_entry`, single-buffer HTML build + single `send()`).
- `apps/cgi_index.lst` — SDCC assembly listing showing the off-by-one
  addressing in the pre-fix code.
- `kernel/rom_read.c` — `muzik_rom_readdir()`, verified correct and not
  part of the bug.
- `kernel/test_rom_readdir.c` — host test verifying directory-entry
  indexing independently of the Z80 target.
