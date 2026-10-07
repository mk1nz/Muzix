/*
 * host-test-not-portable: this module is the Z80 context switch, and it reaches
 * userspace and the process entry through an SDCC inline assembly block.  That
 * block is not valid host C, so the file has never compiled in the host pool.
 *
 * It is the second file in the tree to have that property, and the first time it
 * cost anything: an inline `__asm ... __endasm;` in kernel/kernel_loop.c on
 * 2026-10-04 removed THAT module from the host build, and with it sixteen tests
 * that had been running on every build - because every one of them links
 * kernel_loop.c.  The host gate printed a tidy reason and reported PASS.
 * tools/host_tests.rb now fails the build for any pool module that stops compiling
 * without declaring itself here, which is why this line exists and is worth its
 * length: the honest declaration is what distinguishes "this was always
 * impossible" from "this stopped working".
 *
 * What is NOT true, and would be the wrong thing to write here, is that the module
 * could not be made portable.  It is assembly because it hands control to the
 * process and does not return, which is not expressible in C at all - not a host
 * limitation, a property of the thing being done.  That is also why
 * muzix_zeta_enter_userspace() is the one the scheduler is told to use, and why
 * the chain that calls it is abandoned rather than returned from.
 */
#include "context_switch.h"
#include "bank_io.h"
#include "z80_io.h"
#include "uart_io.h"
#include "trace.h"

const process_map_t g_kernel_map = {
    .pages = {0x20, 0x21, 0x22, 0x23}
};

/* Traces for the ways a map install or a copy is refused.  Every byte of
 * _CODE is taken from the 64 KiB budget shared with _DATA, so these are terse;
 * the header spells out which check each one comes from.  "win0" is the window
 * tools/check_window0.rb, which proves from the link map that every module
 * which can change a map is linked below 0x4000. "w0 map" is a copy aimed at the
 * kernel's own region 0. */
#define ZETA_TRACE_WINDOW3 "zeta: w3 map\r\n"
#define ZETA_TRACE_WINDOW  "zeta: win0 map\r\n"
#define ZETA_TRACE_WINDOW0 "zeta: w0 map\r\n"

/* Reentrant context stack for nested temp map operations. */
#define ZETA_CONTEXT_DEPTH_MAX 8
static zeta_saved_context_t g_context_stack[ZETA_CONTEXT_DEPTH_MAX];
static uint8_t g_context_depth;

void zeta_context_stack_init(void)
{
    /* Idempotent and safe to call at any time, including with temp-map
     * operations still in flight.  A slot leaked by a callback that entered a
     * process and never came back is reclaimed here rather than wedging every
     * later call; the bank map left installed is the one that callback switched
     * to, and the kernel loads the kernel map explicitly before doing anything
     * else. */
    g_context_depth = 0;
}

/* Single atomic bank write - ONLY place that touches hardware registers.
 * Region 0 (port 0x78) is ALWAYS kept at kernel bank 0x20 for syscall vector
 * at 0x0030.  Only regions 1,2,3 (ports 0x79,0x7A,0x7B) are switched per
 * process.
 *
 * Interrupt state is left completely alone.  The obvious DI/EI wrapper cannot
 * save IFF: DI clears IFF2 as well as IFF1, so there is no copy left to put
 * back, and the unconditional EI on the way out silently ended the caller's
 * critical section on every call.  All five public entry points document that
 * interrupts must already be disabled, which makes the wrapper redundant; see
 * context_switch.h.
 */
static void write_all_banks(zeta_state_t *state, const uint8_t pages[4])
{
    /* Region 0 fixed to kernel bank 0x20 */
    state->bank_reg[0] = 0x20;
    z80_outb(MUZIX_ZETA_BANK_PORT_0, state->bank_reg[0]);
    /* Regions 1,2,3 from map */
    state->bank_reg[1] = pages[1];
    z80_outb(MUZIX_ZETA_BANK_PORT_1, state->bank_reg[1]);
    state->bank_reg[2] = pages[2];
    z80_outb(MUZIX_ZETA_BANK_PORT_2, state->bank_reg[2]);
    state->bank_reg[3] = pages[3];
    z80_outb(MUZIX_ZETA_BANK_PORT_3, state->bank_reg[3]);
}

static void load_map(zeta_state_t *state, const process_map_t *map)
{
    write_all_banks(state, map->pages);
}

/* Each public entry point that installs a map has to test this in its *own*
 * frame, before delegating: the bank registers are write-only, so the only way
 * to know whether the caller survives the switch is to look at the address it
 * is about to return to.  A caller living in window 1, 2 or 3 fetches its next
 * instruction from the page it just mapped - user memory - and jumps
 * somewhere arbitrary.  That stayed hidden by accident of link order. */
static void trace_map_refused(void)
{
    muzix_trace_write(ZETA_TRACE_WINDOW);
}

/* Install a map, or report that it was refused.
 *
 * The return value is the point. This used to be void and its callers all
 * reported success, so muzix_system_service_load_map / _load_kernel_map /
 * _context_switch returned OK while changing no bank register at all: five
 * "switch to kernel" call sites in kernel/{scheduler,process_state,
 * runtime_state}.c and the yield path in kernel_loop.c all believed they had
 * switched maps. A caller that cannot install a map now has to be able to say
 * so. */
int zeta_load_map(zeta_state_t *state, const process_map_t *map)
{
    if (!state || !map) {
        return -1;
    }
    load_map(state, map);
    return 0;
}

static void save_map(zeta_state_t *state, zeta_saved_context_t *saved)
{
    saved->pages[0] = state->bank_reg[0];
    saved->pages[1] = state->bank_reg[1];
    saved->pages[2] = state->bank_reg[2];
    saved->pages[3] = state->bank_reg[3];
}




void zeta_kernel_enter(zeta_state_t *state,
                       zeta_saved_context_t *saved_user_context)
{
    if (!state || !saved_user_context) {
        return;
    }
    save_map(state, saved_user_context);
    load_map(state, &g_kernel_map);
}

/* Install `target_map` and transfer control to the entry point stashed in
 * _g_enter_entry_asm (a window-0 cell in kernel_entry.s). Does not return.
 *
 * MUST compile without a stack frame.  `load_map` changes windows 1-3 to the
 * process's pages, so anything this function touches afterwards has to be in
 * window 0.  That rules out: locals (the frame is in window 3), any _DATA
 * static, and a normal epilogue - `ld sp,ix / pop ix / ret` would pop the
 * return address out of the process's page.  Hence no locals, and the jump is
 * inline rather than a return.
 *
 * Called only from kernel_entry.s's window-0 assembly. */
/* Resume a process at a saved point rather than at a fixed entry.
 *
 * This is what a context switch needs and what zeta_enter_process() below
 * cannot express. Exec enters a program at its entry point with a fresh stack;
 * a switch has to come back to the instruction after a syscall the process made
 * earlier, on the stack it was using then, and hand back the value that
 * syscall was supposed to return.
 *
 * The three words are read from window-0 cells that kernel_entry.s fills in
 * before calling, not passed as arguments. `pc` is an address in the process's
 * own text page, so it means nothing once a different map is installed, and
 * this function is exactly the moment that happens - a C parameter could not be
 * read at that point anyway. The cells live in _CODE, so window 0 keeps mapping
 * them across the switch.
 *
 * Never returns. */
void zeta_resume_process(zeta_state_t *state, const process_map_t *target_map)
{
    if (!state || !target_map) {
        muzix_zeta_uart_send((uint8_t)'!', 0);
        for (;;) { }
    }
    load_map(state, target_map);
    /* Returns to its caller rather than jumping: the jump needs to reload SP and
     * PC, and neither SDCC's inline assembler nor a C->assembly call will do
     * that here - the inline assembler rejects `ld sp,hl` outright, and a C
     * function calling a .s routine gets mangled with a double underscore. So
     * the caller in kernel_entry.s does the jump instead, immediately after
     * this returns, with the map already installed and the window-0 cells
     * filled. It never comes back either way. */
}

void zeta_enter_process(zeta_state_t *state, const process_map_t *target_map)
{
    /* Both refusals used to be a bare `for(;;){}`, so a rejected handoff was an
     * invisible hang with no output at all. Report the reason to the console
     * before spinning; the trace ring is not dumped on this path. */
    if (!state || !target_map) {
        muzix_zeta_uart_send((uint8_t)'!', 0);
        for (;;) { }
    }
    load_map(state, target_map);
    __asm
        ld hl, (_g_enter_entry_asm)
        jp (hl)
    __endasm;
}

void zeta_kernel_exit(zeta_state_t *state,
                      const zeta_saved_context_t *saved_user_context)
{
    if (!state || !saved_user_context) {
        return;
    }
    load_map(state, (const process_map_t *)saved_user_context);
}

/* Take a context-stack slot, install target_map, run func(arg), restore.
 *
 * Window 3 ($C000-$FFFF) is the kernel's own window: the C stack, every
 * automatic variable and every static in _DATA live there.  A map that replaces
 * pages[3] therefore hides the return address of the code that is running, let
 * alone the staging buffer and the callback's context struct.  Userspace keeps
 * its stack at 0xFFF0, so region 3 is live for every process: before this
 * check, a region-3 copy wrote the kernel's staging buffer into the process's
 * page 3 and read that same page back as if it held kernel data.
 *
 * Moving the staging buffer cannot fix this - the callback's *frame* is in
 * window 3 too, not just the buffer - so region-3 maps are rejected instead.  A
 * real region-3 copy needs a leaf assembly primitive that reads its descriptor
 * from a window-0-mapped location and never returns while the kernel stack is
 * hidden.  bank_io.s was that home: muzix_zeta_copy_window3() existed for it,
 * was never called, and was removed on 2026-10-02 with the rest of that
 * module's dead entry points, so there is now nowhere for it to live.
 */
/* Forward declarations: map_call_impl below dispatches on a callback identity,
 * and the callbacks it selects are defined further down this file. */
static int copy_page_to_kernel_callback(void *arg);
static int copy_user_to_kernel_callback(void *arg);
static int copy_kernel_to_user_callback(void *arg);
static int copy_rom_to_kernel_callback(void *arg);

static int map_call_impl(zeta_state_t *state,
                         const process_map_t *target_map,
                         zeta_callback_id_t cb,
                         void *arg)
{
    uint8_t slot;
    int result;

    if (!state || !target_map || cb == ZETA_CB_NONE) {
        return -1;
    }
    if (target_map->pages[3] != state->bank_reg[3]) {
        muzix_trace_write(ZETA_TRACE_WINDOW3);
        return -1;
    }
    if (g_context_depth >= ZETA_CONTEXT_DEPTH_MAX) {
        return -1;
    }

    /* Claim the slot and drop the depth *before* the callback.  It may enter a
     * process and never come back; if the depth were released only on the
     * return path, one such callback would leak the slot permanently and wedge
     * every later temp-map operation with a stale, unrestored map.  The map
     * left installed is then the one the callback switched to, which is the
     * right end state for a callback that has left the kernel for good. */
    slot = g_context_depth++;
    save_map(state, &g_context_stack[slot]);
    load_map(state, target_map);

    /* Dispatch on an identity, not through a function pointer.  All three
     * callbacks live in this file, so the pointer bought nothing - and it did
     * not survive the toolchain: platform/zeta-v2/muzix_sdcccall.peep rewrites
     * ___sdcc_call_iy to a hand-written `jp (iy)` because --peep-file replaces
     * SDCC's whole default peephole set, and the sequence SDCC emits for
     * `func(arg)` leaves IY holding something else.  The MM-mediated copy of a
     * userspace byte therefore jumped out of the kernel into a data address the
     * first time it ran, which is why nothing the TTY ever transmitted was the
     * process's output. */
    switch (cb) {
    case ZETA_CB_COPY_PAGE_TO_KERNEL:
        result = copy_page_to_kernel_callback(arg);
        break;
    case ZETA_CB_COPY_USER_TO_KERNEL:
        result = copy_user_to_kernel_callback(arg);
        break;
    case ZETA_CB_COPY_KERNEL_TO_USER:
        result = copy_kernel_to_user_callback(arg);
        break;
    case ZETA_CB_COPY_ROM_TO_KERNEL:
        result = copy_rom_to_kernel_callback(arg);
        break;
    default:
        result = -1;
        break;
    }

    load_map(state, (const process_map_t *)&g_context_stack[slot]);
    g_context_depth = slot;
    return result;
}

zeta_callback_id_t zeta_callback_from_fn(int (*func)(void *))
{
    if (func == copy_page_to_kernel_callback) {
        return ZETA_CB_COPY_PAGE_TO_KERNEL;
    }
    if (func == copy_user_to_kernel_callback) {
        return ZETA_CB_COPY_USER_TO_KERNEL;
    }
    if (func == copy_kernel_to_user_callback) {
        return ZETA_CB_COPY_KERNEL_TO_USER;
    }
    if (func == copy_rom_to_kernel_callback) {
        return ZETA_CB_COPY_ROM_TO_KERNEL;
    }
    return ZETA_CB_NONE;
}

/* The window-0 test lives in these two forwarders, not in map_call_impl,
 * because map_call_impl builds a stack frame of its own (--stack-auto) and the
 * guard would then read its saved registers instead of a return address. The
 * check has to run before any frame is built.
 *
 * HOWEVER: the comment that used to sit here claimed these forwarders are
 * frameless, so that SP+2 really is the caller's return address. That is false
 * - context_switch.lst shows `push hl` / `push de` at the top of both, emitted
 * by SDCC to preserve the register arguments across the guard call. SP+2 is
 * therefore saved register content, and the address the guard inspects is
 * whatever the calling convention left in DE.
 *
 * The check does pass on the live path, because DE holds a parameter whose high
 * byte happens to clear bits 14-15 - but that is a property of the call site,
 * not of the function, and it would change silently if a caller's arguments
 * changed. A map switch is the one operation that genuinely needs a window-0
 * caller, and that is a layout problem rather than a guard problem: the kernel
 * spans all four windows, so only the first 16 KB can survive a map switch.
 * See muzix_kernel_loop_yield() in kernel/kernel_loop.c. */
int zeta_cross_bank_call_impl(zeta_state_t *state,
                          const process_map_t *target_map,
                          zeta_callback_id_t cb,
                          void *arg)
{
    if (!muzix_zeta_caller_in_window0()) {
        trace_map_refused();
        return -1;
    }
    return map_call_impl(state, target_map, cb, arg);
}

int zeta_with_temp_map_impl(zeta_state_t *state,
                        const process_map_t *temp_map,
                        zeta_callback_id_t cb,
                        void *arg)
{
    if (!muzix_zeta_caller_in_window0()) {
        trace_map_refused();
        return -1;
    }
    return map_call_impl(state, temp_map, cb, arg);
}

/* Single-region atomic swap for copy operations (no callback, no context stack) */


/* Helper: create temp map with single region swapped (for copy operations) */

/* Staging buffer for copy operations - must be in kernel data region (region 3) */
static uint8_t g_copy_staging[MUZIX_ZETA_COPY_STAGING];

/* Temp map for a single-region copy: keep everything currently installed and
 * remap only the one window the copy needs. */
static void copy_temp_map(const zeta_state_t *state,
                          uint8_t page,
                          uint8_t region,
                          process_map_t *out)
{
    if (region >= 4 || page > 0x3F) {
        return;
    }
    out->pages[0] = state->bank_reg[0];
    out->pages[1] = state->bank_reg[1];
    out->pages[2] = state->bank_reg[2];
    out->pages[3] = state->bank_reg[3];
    out->pages[region] = page;
}

/* --- Copy operations using callback mechanism (execute in region 0) --- */

static int copy_page_to_kernel_callback(void *arg)
{
    struct {
        zeta_state_t *state;
        uint16_t address;
        uint8_t *dst;
        size_t len;
    } *ctx = arg;
    uint16_t i;

    for (i = 0; i < ctx->len; i++) {
        ctx->dst[i] = zeta_read_visible(ctx->state, ctx->address + i);
    }
    return 0;
}

int zeta_copy_page_to_kernel(zeta_state_t *state,
                              uint8_t region,
                              uint8_t page,
                              uint16_t offset,
                              uint8_t *dst,
                              size_t len)
{
    if (!state || !dst || region > 3) {
        return -1;
    }
    /* offset + len has to stay inside the one 16 KiB window, otherwise the copy
     * runs off the end of the mapped page and into the next region's. */
    if ((uint32_t)offset + (uint32_t)len > (uint32_t)ZETA_BANK_SIZE) {
        return -1;
    }
    /* Region 0 is the kernel's window and write_all_banks() rewrites pages[0]
     * back to 0x20 whatever the temp map asked for.  A region-0 read therefore
     * cannot honour its `page` argument at all: it returned the kernel's own
     * page 0x20 and the caller saw it as the requested page's contents. */
    if (region == 0) {
        muzix_trace_write(ZETA_TRACE_WINDOW0);
        return -1;
    }
    /* Region 3 is the kernel's own window - see map_call_impl(). */
    if (region == 3) {
        muzix_trace_write(ZETA_TRACE_WINDOW3);
        return -1;
    }

    process_map_t temp_map;
    copy_temp_map(state, page, region, &temp_map);

    struct {
        zeta_state_t *state;
        uint16_t address;
        uint8_t *dst;
        size_t len;
    } ctx = { state, (uint16_t)((uint16_t)region * ZETA_BANK_SIZE + offset),
              dst, len };

    return zeta_with_temp_map_impl(state, &temp_map,
                                   ZETA_CB_COPY_PAGE_TO_KERNEL, &ctx);
}

/* Read one ROM page into a kernel buffer, with the page already banked into
 * `region`. Runs under the temp map, so window 3 is still the kernel's page and
 * both the staging words (in _DATA) and the destination stay reachable. */
static int copy_rom_to_kernel_callback(void *arg)
{
    struct {
        uint16_t src;      /* absolute address inside the banked window */
        uint8_t *dst;
        uint16_t len;
    } *ctx = arg;

    muzix_zeta_staged_src = ctx->src;
    muzix_zeta_staged_dst = (uint16_t)(uintptr_t)ctx->dst;
    muzix_zeta_staged_len = ctx->len;
    muzix_zeta_ldir_staged();
    return 0;
}

int zeta_copy_rom_page_to_kernel(zeta_state_t *state,
                                 uint8_t page,
                                 uint16_t offset,
                                 uint8_t *dst,
                                 size_t len)
{
    process_map_t temp_map;
    struct {
        uint16_t src;
        uint8_t *dst;
        uint16_t len;
    } ctx;

    if (!state || !dst || len == 0) {
        return -1;
    }
    /* One window only. A read that starts past its end, or that would run past
     * it, has to be refused: the bytes above the window belong to the next one,
     * which during a syscall is the process's text page, and reading them as
     * ROM would hand the kernel the user's own instructions. */
    if (offset >= ZETA_BANK_SIZE ||
        (uint32_t)offset + (uint32_t)len > (uint32_t)ZETA_BANK_SIZE) {
        return -1;
    }
    if (len > 0xffffu) {
        return -1;
    }

    /* Region 1 is the staging window. The kernel map is already installed when
     * this runs (the syscall entry installs it before dispatching), so a temp
     * map differing only in region 1 keeps window 3 - the kernel stack, _DATA
     * and the staging words - exactly where the running code expects it. */
    copy_temp_map(state, page, 1, &temp_map);

    ctx.src = (uint16_t)((uint16_t)1 * ZETA_BANK_SIZE + offset);
    ctx.dst = dst;
    ctx.len = (uint16_t)len;

    return zeta_with_temp_map_impl(state, &temp_map,
                                   ZETA_CB_COPY_ROM_TO_KERNEL, &ctx);
}

static int copy_kernel_to_user_callback(void *arg)
{
    struct {
        zeta_state_t *state;
        uint16_t user_addr;
        size_t len;
    } *ctx = arg;
    uint16_t i;

    for (i = 0; i < ctx->len; i++) {
        zeta_write_visible(ctx->state, ctx->user_addr + (uint16_t)i,
                           g_copy_staging[i]);
    }
    return 0;
}

int zeta_copy_kernel_to_user(zeta_state_t *state,
                              const process_map_t *dst_map,
                              uint16_t user_addr,
                              const uint8_t *src,
                              size_t len)
{
    uint16_t i;
    uint8_t region;

    if (!state || !dst_map || !src || len > sizeof(g_copy_staging)) {
        return -1;
    }

    region = (uint8_t)(user_addr >> 14);
    if (region > 3 ||
        len > (size_t)(ZETA_BANK_SIZE - (user_addr & (ZETA_BANK_SIZE - 1)))) {
        return -1;
    }
    /* Region 0 is the kernel's own window, exactly like region 3 below: it
     * holds the boot stub, the syscall vector installed at 0x0030 and _CODE
     * from 0x0098.  write_all_banks() pins pages[0] to 0x20 unconditionally, so
     * the map the caller passed cannot make region 0 a process page - a
     * destination below 0x4000 was therefore always a write into kernel code,
     * whatever dst_map said.  src_vir/dst_vir is caller-supplied, so the only
     * safe answer to a region-0 request is to refuse it. */
    if (region == 0) {
        muzix_trace_write(ZETA_TRACE_WINDOW0);
        return -1;
    }
    /* Staging into the process's own page 3 would overwrite the kernel buffer
     * the data came from, and the callback frame would live there too. */
    if (region == 3) {
        muzix_trace_write(ZETA_TRACE_WINDOW3);
        return -1;
    }

    /* Stage before the switch: while window 3 still maps kernel page 0x23 both
     * the source and the staging buffer are reachable. */
    for (i = 0; i < len; i++) {
        g_copy_staging[i] = src[i];
    }

    process_map_t temp_map;
    copy_temp_map(state, dst_map->pages[region], region, &temp_map);

    struct {
        zeta_state_t *state;
        uint16_t user_addr;
        size_t len;
    } ctx = { state, user_addr, len };

    return zeta_with_temp_map_impl(state, &temp_map,
                                   ZETA_CB_COPY_KERNEL_TO_USER, &ctx);
}

static int copy_user_to_kernel_callback(void *arg)
{
    struct {
        zeta_state_t *state;
        uint16_t user_addr;
        size_t len;
    } *ctx = arg;
    uint16_t i;

    for (i = 0; i < ctx->len; i++) {
        g_copy_staging[i] = zeta_read_visible(ctx->state,
                                              ctx->user_addr + (uint16_t)i);
    }
    return 0;
}

int zeta_copy_user_to_kernel(zeta_state_t *state,
                              const process_map_t *src_map,
                              uint16_t user_addr,
                              uint8_t *dst,
                              size_t len)
{
    uint16_t i;
    uint8_t region;

    if (!state || !src_map || !dst || len > sizeof(g_copy_staging)) {
        return -1;
    }

    region = (uint8_t)(user_addr >> 14);
    if (region > 3 ||
        len > (size_t)(ZETA_BANK_SIZE - (user_addr & (ZETA_BANK_SIZE - 1)))) {
        return -1;
    }
    /* Region 0 is the kernel's window: the boot stub, the syscall vector at
     * 0x0030 and _CODE from 0x0098.  Reading below 0x4000 returned kernel code
     * to userspace as if it were the process's own memory.  See the matching
     * check in zeta_copy_kernel_to_user(). */
    if (region == 0) {
        muzix_trace_write(ZETA_TRACE_WINDOW0);
        return -1;
    }
    /* Same window-3 rule as above: the callback would read the process's page
     * 3 and drop the result straight into the kernel's staging buffer. */
    if (region == 3) {
        muzix_trace_write(ZETA_TRACE_WINDOW3);
        return -1;
    }

    process_map_t temp_map;
    copy_temp_map(state, src_map->pages[region], region, &temp_map);

    struct {
        zeta_state_t *state;
        uint16_t user_addr;
        size_t len;
    } ctx = { state, user_addr, len };

    if (zeta_with_temp_map_impl(state, &temp_map,
                              ZETA_CB_COPY_USER_TO_KERNEL, &ctx) != 0) {
        return -1;
    }

    for (i = 0; i < len; i++) {
        dst[i] = g_copy_staging[i];
    }
    return 0;
}
