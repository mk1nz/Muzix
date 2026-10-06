#include "trace.h"
#include "uart_io.h"

/*
 * Trace storage.  Kept in _DATA alongside the rest of the kernel globals; at
 * 512 bytes it is deliberately small, because every byte spent here is a byte
 * not available for code (see trace.h).
 */
static uint8_t g_trace_ring[MUZIX_TRACE_RING_SIZE];
static uint16_t g_trace_head;  /* next write position */
static uint32_t g_trace_total; /* bytes written since last clear */
static uint8_t g_trace_started;

/* Stack high-water mark; see trace.h for why this exists. 0xFFFF means "never
 * marked", which is above any real SP so the first mark always records. */
static uint16_t g_stack_min_sp = 0xFFFFu;
static uint16_t g_stack_sp;
static uint8_t g_stack_marked;

static void uart_putc(uint8_t c)
{
    muzix_zeta_uart_send(c, 0);
}

static void uart_write(const char *s)
{
    while (*s) {
        uart_putc((uint8_t)*s++);
    }
}

/* One copy of the nibble-to-ASCII conversion, shared by the UART and the ring
 * writers.  The two used to carry byte-identical bodies. */
static uint8_t hex_digit(uint8_t nibble)
{
    static const char hex[] = "0123456789ABCDEF";

    return (uint8_t)hex[nibble & 0x0F];
}

static void uart_hex8(uint8_t v)
{
    uart_putc(hex_digit((uint8_t)(v >> 4)));
    uart_putc(hex_digit(v));
}

static void uart_hex32(uint32_t v)
{
    uart_hex8((uint8_t)(v >> 24));
    uart_hex8((uint8_t)(v >> 16));
    uart_hex8((uint8_t)(v >> 8));
    uart_hex8((uint8_t)v);
}

void muzix_trace_putc(uint8_t c)
{
    g_trace_ring[g_trace_head] = c;
    g_trace_head = (uint16_t)((g_trace_head + 1u) % MUZIX_TRACE_RING_SIZE);
    /* 32 bits, and it deliberately does not saturate.  A counter that sticks
     * at 0xFFFF makes muzix_trace_total() stop counting and caps
     * muzix_trace_dropped() at 65407, so a run long enough to matter reports
     * the loss least accurately - and once the low word is what a wrapped
     * counter is made of, muzix_trace_dropped() can read 0 for a ring that has
     * overwritten everything in it. */
    g_trace_total++;
    g_trace_started = 1;
    /* Mirror to the console.  kernel_main's uart_puts() feeds this function, so
     * without the mirror every boot message went into a 128-byte ring and was
     * overwritten before anyone could read it - which is why the first exec
     * failure reported nothing but "EXEC FAIL A". */
    muzix_zeta_uart_send(c, 0);
}

void muzix_trace_write(const char *s)
{
    if (!s) {
        return;
    }
    while (*s) {
        muzix_trace_putc((uint8_t)*s++);
    }
}

void muzix_trace_hex8(uint8_t v)
{
    muzix_trace_putc(hex_digit((uint8_t)(v >> 4)));
    muzix_trace_putc(hex_digit(v));
}

void muzix_trace_clear(void)
{
    uint16_t i;

    for (i = 0; i < (uint16_t)MUZIX_TRACE_RING_SIZE; i++) {
        g_trace_ring[i] = 0;
    }
    g_trace_head = 0;
    g_trace_total = 0;
    g_trace_started = 0;
}

uint32_t muzix_trace_total(void)
{
    return g_trace_total;
}

uint32_t muzix_trace_dropped(void)
{
    if (g_trace_total <= (uint32_t)MUZIX_TRACE_RING_SIZE) {
        return 0;
    }
    return g_trace_total - (uint32_t)MUZIX_TRACE_RING_SIZE;
}

void muzix_trace_stack_report(void)
{
    uart_write("stack min SP ");
    uart_hex32(g_stack_min_sp);
    uart_write(", headroom ");
    uart_hex32((uint32_t)(0xFFFEu - g_stack_min_sp));
    uart_write("\r\n");
}

void muzix_trace_stack_mark(void)
{
    /* Named stack_pointer, not sp: the assembly below reads the Z80's SP
     * register, and a local called `sp` a few lines away means two different
     * things under the same name.  cppcheck resolved `add hl, sp` to this
     * variable and correctly reported it uninitialised - in the assembly it is
     * the register - but the ambiguity was the compiler's to see, and renaming
     * is the honest fix rather than a suppression.
     *
     * The block also clobbers HL, A, DE and BC without telling the compiler.
     * That is safe here because the only value it reads is the static
     * g_stack_sp, which lives in memory, and stack_pointer is written after
     * the block rather than across it.  Keep it that way: adding a local that
     * must survive the block would need it pushed first. */
    uint16_t stack_pointer;

#ifdef __SDCC
    // The high byte goes through a reloaded pointer rather than an
    // absolute-plus-offset address, `ld (_g_stack_sp + 1), a`.  The offset form
    // reads as a comma expression to cppcheck, and the suppression that silences
    // it cannot go inside an __asm block - so the offset is removed instead of
    // hidden.  sdasz80 has no `ld (de),l`, so HL is reloaded to point at the
    // second byte; this runs on a diagnostic path, so the two extra
    // instructions do not matter.
    __asm
        ld      hl, #4
        add     hl, sp
        ld      a, l
        ld      (_g_stack_sp), a
        ld      a, h
        ld      hl, #_g_stack_sp
        inc     hl
        ld      (hl), a
    __endasm;

    stack_pointer = g_stack_sp;
#else
    /* The same quantity, read the way a C compiler can read it: the address of
     * a local is where this frame is, which is what the block above is reaching
     * for when it reads SP.  Without this the whole of trace.c is invisible to a
     * host test, and trace.c is what muzix_trace_dump() and the shell's `trace`
     * command are.
     *
     * The number is not the Z80 SP and is not claimed to be.  It has to be a
     * stack address that moves the same way when the frame grows, because the
     * only thing muzix_trace_stack_mark() does with it is keep the minimum. */
    stack_pointer = (uint16_t)(uintptr_t)&stack_pointer;
#endif
    if (!g_stack_marked || stack_pointer < g_stack_min_sp) {
        g_stack_min_sp = stack_pointer;
    }
    g_stack_marked = 1;
}

void muzix_trace_dump(void)
{
    uint32_t total = muzix_trace_total();
    uint32_t dropped = muzix_trace_dropped();
    uint16_t start;
    uint16_t i;

    uart_write("--- trace: ");
    uart_hex32(total);
    uart_write(" bytes");
    if (dropped != 0) {
        uart_write(", oldest ");
        uart_hex32(dropped);
        uart_write(" overwritten");
    }
    uart_write(" ---\r\n");

    if (!g_trace_started) {
        uart_write("(empty)\r\n");
        return;
    }

    /* Once the ring has wrapped, the oldest surviving byte is at the head. */
    start = (total < (uint32_t)MUZIX_TRACE_RING_SIZE) ? 0 : g_trace_head;

    for (i = 0; i < (uint16_t)MUZIX_TRACE_RING_SIZE; i++) {
        uint8_t c = g_trace_ring[(uint16_t)((start + i) % MUZIX_TRACE_RING_SIZE)];

        uart_putc(c ? c : (uint8_t)' ');
    }
    uart_putc((uint8_t)'\r');
    uart_putc((uint8_t)'\n');
}
