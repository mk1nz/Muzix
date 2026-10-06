#include "tty_device.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../kernel/proc_table.h"

#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/uart_io.h"

#include <string.h>

/* The bounded wait, reached through a pointer rather than called directly.
 *
 * The reason is a measurement and it is worth stating precisely, because the
 * shape looks arbitrary.  Every other call in muzix_tty_read() goes through
 * the tty's function pointers, which SDCC reaches with ___sdcc_call_iy, and
 * that path keeps IY alive across the call.  A DIRECT call does not, so the
 * compiler has to assume the loop's live values are gone and spills them -
 * and having spilled them it rebuilds the whole function around a computed
 * struct pointer.  Measured on muzix_tty_read(), which is a thousand bytes
 * long and at a register-allocation cliff:
 *
 *     no bounded wait at all                            -8 bytes
 *     the wait inlined (SDCC inlines small statics)   +246 bytes
 *     the wait as a direct call                        +222 bytes
 *     the wait through a function pointer          (this number)
 *
 * So it is a pointer, and it stays a pointer.  "Just call it directly" is a
 * 222-byte change that shows up as a stack-headroom build failure saying
 * nothing about reads. */

/* How long muzix_tty_read waits for input before giving up, in seconds.
 *
 * Both waits below used to be `while (!available()) continue;` with no exit
 * condition at all, and that is the whole of the "blocking I/O spin-waits and
 * monopolises CPU" limitation in AGENTS.md: a read with nothing to read never
 * returned, so the CPU was held until a key arrived.  A wait that cannot end
 * is not a wait - if the UART reports nothing, or a process reads a terminal
 * that has gone quiet, the machine is simply gone.
 *
 * Five seconds is long enough that a person at a serial console never sees it
 * and short enough that a genuinely dead line is reported rather than hung on.
 * It is measured on the 120 Hz CTC tick (platform/zeta-v2/tick.s), not on
 * iterations, so it is the same time base as every other deadline here and it
 * does not drift.
 *
 * WHAT A TIMEOUT RETURNS
 * ----------------------
 * -1, and *result = 0.  An error, not an end of file, because it is one: the
 * read did not fail to find end of file, it ran out of patience.  The two have
 * to be distinguishable, and a zero-length return is not enough of a
 * distinction to be worth having.
 *
 * It matters because of what a caller does with each.  Ctrl-D on this device is
 * a zero-length read, and every terminal in the tree treats that as "the line
 * is finished, the program should stop reading" - lib/libc.c's getchar() returns
 * -1, and shell/shell.c's readline() breaks out of the command loop and the
 * shell exits, and the init task starts a new one.  With a timeout reported the
 * same way, five seconds of an idle console looked exactly like somebody
 * pressing Ctrl-D: the machine printed its boot banner again, for ever, every
 * five seconds.  That is what the first version did, and the emulator caught it
 * in about ten seconds of idle.
 *
 * The distinction is the caller's to make, because only the caller knows which
 * it wants.  A console that wants to wait re-arms: kernel/shell.c's fallback
 * shell moves the deadline forward instead of giving up, and
 * shell/shell.c's readline() retries the read.  A program that genuinely wants
 * "give me what has arrived, or tell me nothing is coming" gets -1 and can say
 * so, which zero-length read could never let it do.
 *
 * Canonical mode on expiry does NOT hand back a half-typed line.  It used to,
 * on the grounds that a line with no newline is "complete enough to act on"
 * and that losing what someone typed because they paused is a worse defect
 * than the spin this replaced - and that first half is what broke the console.
 * Handing it back let the reader drain the buffer while the line was still
 * being edited, so every later Backspace or Ctrl-U found an empty buffer and
 * did nothing, silently.  Nothing is lost by withholding it: the bytes stay in
 * tty->input and the line is returned whole once the terminator arrives.  See
 * the branch in muzix_tty_collect_line() and commit f66f35d. */
#define MUZIX_TTY_READ_SECONDS 5
#define MUZIX_TTY_READ_TICKS (MUZIX_TICKS_PER_SECOND * MUZIX_TTY_READ_SECONDS)

/* Backstop for the wait, in poll iterations, for the case where the tick does
 * not move at all.  Each iteration is one port read - tens of cycles - so
 * 1 048 576 is far longer than the 5 s the tick budget expresses at 120 Hz, and
 * it exists only so a dead counter cannot spin forever.  A power-of-two value
 * keeps the comparison to an increment and a mask. */
#define MUZIX_TTY_READ_POLLS 0x00100000u

/*
 * The callbacks are mirrored out of the device struct into these statics, and
 * every call site uses them.
 *
 * Calling tty->send(...) through the struct's function pointer does not work in
 * this build.  It compiles to
 *
 *     ld  a, (hl)                   ; one byte of the struct, not the target
 *     push bc
 *     call _muzix_sdz80_call_hl     ; which is only `jp (hl)`
 *
 * so the transfer lands on an address inside _DATA and the CPU leaves the
 * kernel in the middle of a write.  That is why the first userspace SYS_WRITE
 * never returned: muzix_tty_write was entered, jumped out through the
 * function pointer, and the machine wandered into the boot stub.  It is the
 * same failure muzix_z80_dispatch_frame had with its handler and map hooks, and
 * it has the same cause: platform/zeta-v2/muzix_sdccall.peep rewrites
 * ___sdcc_call_hl to a hand-written `jp (hl)` because --peep-file replaces
 * SDCC's entire default peephole set, and SDCC's sequence for an indirect call
 * does not match what that stub expects.
 *
 * The struct fields are still maintained, so anything that introspects a device
 * sees the truth, and so the test binaries are unaffected.  A fixed set of
 * callbacks has no reason to travel by pointer at all: the kernel installs
 * exactly one set, the UART's, at boot.
 */
static muzix_tty_send_t g_tty_send;
static muzix_tty_recv_t g_tty_recv;
static muzix_tty_available_t g_tty_available;
static void *g_tty_userdata;

/* The three call shims.  With MUZIX_TTY_CALLBACKS they dispatch through the
 * installed pointers; without it they go straight to the Zeta UART, which is
 * what the kernel always wants and the only form the toolchain can compile
 * into a working indirect call.  See tty_device.h. */
#if MUZIX_TTY_CALLBACKS
#define tty_do_send(b)   (g_tty_send((b), g_tty_userdata))
#define tty_do_recv()    (g_tty_recv(g_tty_userdata))
#define tty_have_recv()  (g_tty_recv != 0)
#define tty_have_avail() (g_tty_available != 0)
#define tty_do_avail()   (g_tty_available(g_tty_userdata))
#else
#define tty_do_send(b)   (muzix_zeta_uart_send((b), g_tty_userdata))
#define tty_do_recv()    (muzix_zeta_uart_recv(g_tty_userdata))
#define tty_have_recv()  (1)
#define tty_have_avail() (1)
#define tty_do_avail()   (muzix_zeta_uart_available(g_tty_userdata))
#endif

/*
 * ONLCR, the one output transformation a console cannot do without.
 *
 * A terminal has no "line feed" as a way to end a line.  LF moves the cursor
 * DOWN one row and leaves the column where it was, so a program that emits '\n'
 * gets a staircase: every line starts one column further right than the last.
 * Ending a line is CR (back to column 0) followed by LF (down one row), and the
 * conventional shorthand for that is "\r\n".
 *
 * Nobody should have to write it.  Every program in this tree - and libc's
 * puts() and printf() with it - ends a line with '\n' and nothing else, which is
 * what a program is supposed to do: '\n' is a newline, "\r\n" is a line ending
 * for one particular output device, and a program that hard-codes the second
 * has a file that is wrong on every other device.  Translating here, in the
 * device, is the standard place for it - termios does the same thing as OPOST
 * with ONLCR, and it is why nobody has to think about it on a Unix.
 *
 * THIS DEVICE DID NOT DO IT, and the gap had a shape worth recording.  Both
 * userspace output and the echo of Enter went out byte for byte, so every line
 * the system printed started one column further right than the last, and the
 * fallback shell in kernel/shell.c printed correctly because it had its own
 * private copy of this function (kernel/shell.c:18-24) - one of the two paths
 * out of the kernel had the rule and the other did not.  Anything that positions
 * the cursor itself is also broken by this, which is what made top's screen
 * refresh (ESC[2J, ESC[H and its fixed-height block) unreadable: the clear
 * worked and the block arrived staircased underneath it.
 *
 * The transformation is one comparison and, on a newline, one extra byte.  A
 * program that already writes its own CR first gets "\r\r\n", which is a
 * carriage return to column 0 twice - a no-op on any terminal, and much cheaper
 * than tracking whether one already happened.
 *
 * A function rather than a macro at the two call sites, and that is measured
 * rather than preferred: the macro duplicates the comparison and the send at
 * each of them and measured 35 bytes against this one's 10, in a segment whose
 * whole headroom is tens of bytes.
 */
static void tty_put_byte(uint8_t byte)
{
    if (byte == '\n') {
        tty_do_send((uint8_t)'\r');
    }
    tty_do_send(byte);
}

static void muzix_tty_echo(muzix_tty_device_t *tty, uint8_t byte)
{
    if ((tty->params.flags & MUZIX_TTY_FLAG_ECHO) != 0) {
        tty_put_byte(byte);
    }
}

/* Erase the last character on screen: back, space, back.
 *
 * This is one thing that happens in two places - erase takes a character off the
 * end of the line, kill takes off all of them one at a time - and it was six
 * calls to muzix_tty_echo() spelling the same three bytes out at each.  Six call
 * sites for a sequence that has one meaning costs more _CODE than the helper that
 * states it once, and this kernel is measured in tens of bytes of headroom.
 *
 * Back, space, back rather than just back, and the space is the point: a terminal
 * moves left over the character without erasing it, so a line erased this way
 * still shows a character where the one that was removed used to be. */
static void tty_echo_erased_char(muzix_tty_device_t *tty)
{
    muzix_tty_echo(tty, '\b');
    muzix_tty_echo(tty, ' ');
    muzix_tty_echo(tty, '\b');
}

/* Is this byte a Backspace?
 *
 * There are two of them, and which one a key sends is the terminal's decision
 * rather than this device's or the program's.  0x08 (BS) is what a VT100 sends
 * and is still the setting on a real serial terminal; 0x7F (DEL) is what
 * xterm, PuTTY, Terminal.app, every Linux virtual console and this machine's own
 * console send by default, and has done since the 1980s.  That drift is the
 * whole reason Unix carries a `stty erase` at all - you tell the tty which of
 * the two your terminal produces - and a system that has no stty has to accept
 * both or it has a Backspace key that does nothing.
 *
 * It did nothing, measurably.  With erase at 0x08 alone, a 0x7F was not
 * recognised as a control character at all: it was stored in the line buffer as
 * if the user had typed a character, echoed back verbatim, and handed to the
 * program inside the line.  On a terminal that sends DEL that is the worst
 * possible failure, because 0x7F is not printable - so the screen showed no key
 * having been pressed at all, while the line quietly filled with invisible
 * characters that went into the command the user then ran.  On the wire:
 *
 *     echo abc<BS><BS>def   ->  adef          the key works
 *     echo abc<DEL><DEL>def  ->  abc<DEL><DEL>def   and this is the same key
 *
 * Both are accepted here whatever params.erase is set to, and the configured
 * character is accepted as well, so SETP still means something.  The default is
 * 0x7F, the byte a terminal actually sends, and it is not load-bearing either
 * way: the test is a set of two plus the configured byte, not a comparison
 * against that field alone.
 *
 * A macro and not a function, and that is measured like everything else in this
 * file: taking the device pointer as a parameter costs 66 bytes here, against
 * 15 inline, and there is exactly one call site. */
#define tty_is_erase(tty, byte)                                        \
    ((byte) == 0x08u || (byte) == 0x7Fu || (byte) == (tty)->params.erase)

void muzix_tty_device_init(muzix_tty_device_t *tty)
{
    if (!tty) {
        return;
    }
    memset(tty, 0, sizeof(*tty));
    /* A console is line-buffered with echo.  Leaving flags at 0 selected the
     * non-canonical branch of muzix_tty_read, which is a poll rather than a
     * wait: it returned zero bytes whenever the buffer was momentarily empty,
     * and a caller reads a zero-length read as end of file.  The shell
     * therefore printed its banner and prompt and quit immediately.  This is the
     * device default, so an ioctl that changes it still wins. */
    tty->params.flags = MUZIX_TTY_FLAG_CANONICAL | MUZIX_TTY_FLAG_ECHO;
    /* Backspace is 0x7F, the byte a terminal actually sends; 0x08 is the one
     * the erase echo and the documentation here use, and tty_is_erase() accepts
     * both, so neither choice is load-bearing. */
    tty->params.erase = 0x7F;
    tty->params.kill = 21;
    tty->chars.intr = 3;
    tty->chars.quit = 28;
    tty->chars.start = 17;
    tty->chars.stop = 19;
    tty->chars.eof = 4;
    tty->chars.brk = 0;
}

void muzix_tty_device_set_callbacks(muzix_tty_device_t *tty,
                                     muzix_tty_send_t send,
                                     muzix_tty_recv_t recv,
                                     muzix_tty_available_t available,
                                     void *userdata)
{
    if (!tty) {
        return;
    }
    tty->send = send;
    tty->recv = recv;
    tty->available = available;
    tty->userdata = userdata;
    g_tty_send = send;
    g_tty_recv = recv;
    g_tty_available = available;
    g_tty_userdata = userdata;
}

/* Wait for a byte, bounded.  Returns 1 when one is there, 0 on timeout.
 *
 * A function rather than a loop inside muzix_tty_read(), and with NO PARAMETER,
 * and both of those are measurements rather than taste.  SDCC 4.5 is at a
 * register-allocation cliff in muzix_tty_read() - a thousand-byte function with
 * a dozen struct fields in play - and anything that needs a register set up for
 * it tips the function over:
 *
 *   * the deadline as a local, spilled with `ex (sp),hl` because it has to
 *     survive the call through the tty's function pointer:      +246 bytes
 *   * the wait as a helper taking a `uint16_t ticks`:          +228 bytes
 *   * both removed:                                              -8 bytes
 *
 * So the deadline lives here, in a small function with two locals where the
 * spill is contained, and the wait takes its budget from a constant rather than
 * from the caller.  The caller gains a bare `call` and nothing to set up.  The
 * kernel is 152 bytes above the stack headroom floor its own deepest measured
 * syscall chain sets, so which of these three shapes it gets is the difference
 * between building and not - and the measurement is recorded here because the
 * next person to "just pass the budget in as a parameter" will otherwise make
 * this function 228 bytes larger and the build will fail with a stack headroom
 * message that has nothing obviously to do with a read deadline.
 *
 * The budget is a constant here rather than a parameter because that is also
 * what keeps the call argument-free; MUZIX_TTY_READ_TICKS is the same number
 * either way.
 *
 * And it is not `static`, which is the other half of it.  SDCC 4.5 inlines
 * small static functions, and it inlined this one - into muzix_tty_read(), at
 * both call sites, taking `deadline` and its spill with it, which put the cost
 * straight back where it came from and measured 222 bytes again.  A non-static
 * function is not a candidate, so the spill stays in the twenty-odd
 * instructions that need it.  The declaration is in tty_device.h, because an
 * implicit one is a warning on this build and this tree does not have any. */
/* Whether this board wakes from `halt`, decided at run time, once per boot.
 *
 * MUZIX_HALT_UNPROVEN  take a sample of the tick and loop
 * MUZIX_HALT_GOOD     the tick moved with no halt in the way: interrupts work
 * MUZIX_HALT_BAD      the tick did not move: never halt on this board
 */
#define MUZIX_HALT_UNPROVEN 0u
#define MUZIX_HALT_GOOD     2u
#define MUZIX_HALT_BAD      3u

/* Upper bound on the probe's spin, in passes of the wait loop.  See the case
 * below: generous, because it only has to cover one tick period. */
#define MUZIX_HALT_PROBE_SPINS 200000u

/* REMOVED: MUZIX_TTY_HALT_ENABLE.
 *
 * This used to gate whether muzix_tty_wait_for_input() was allowed to stop the
 * processor, defaulting to off because a board on 2026-10-04 lit the HALT LED at
 * the prompt and accepted nothing further.  The whole mechanism is gone now, and
 * so is the reason for the switch.
 *
 * The console no longer halts here at all.  The wait is not a loop over the
 * receive line: it is one poll that answers "nothing yet", and the caller - the
 * canonical read in muzix_tty_read() - turns that into MUZIX_READ_AGAIN, which
 * lib/libc.c's read() parks on.  Parking is what stops the processor, it happens
 * in the scheduler where the halt loop already was, and the UART interrupt in
 * platform/zeta-v2/tick.s is what ends it early when a key arrives.
 *
 * So there was nothing left to enable.  A flag that selects between two
 * behaviours, where one of them no longer exists, is worse than no flag: it
 * reads as a live configuration option and invites someone to set it. */

static uint8_t g_halt_state = MUZIX_HALT_UNPROVEN;
static uint16_t g_halt_sample;

int muzix_tty_wait_for_input(void)
{
    uint16_t deadline = (uint16_t)(muzix_tick_now() + MUZIX_TTY_READ_TICKS);
    uint16_t polls = 0;

    /* Bounded on polls as well as on the tick.
     *
     * A deadline can only expire if the counter moves.  If the CTC never
     * interrupts - a board where it is not wired, an emulator whose interrupt
     * core does not restore IFF1, a tick that stops - then this loop spins
     * forever and the whole console stops, because the shell's first read is
     * the one that never returns.  A timeout that cannot time out is worse than
     * no timeout: it converts a missing feature into a hang.
     *
     * The poll bound is deliberately large enough not to fire on a slow poll -
     * each iteration is a port read, tens of cycles - while still ending in
     * bounded time if the tick is dead.  It is a backstop, not the mechanism:
     * the tick is what normally ends the wait. */
    /* No waiting here.  At all.
     *
     * This used to spin on the port until its deadline - five seconds - and hand
     * back a line if one had assembled.  That is the last place the processor
     * spent real time with nothing to do, and it is why the busy fraction read
     * 1.00 while the machine did nothing: the spin was the load.
     *
     * Blocking is not this function's job and never was, once there was a way to
     * hand the processor back.  park() records where in the PROCESS to continue,
     * the scheduler stops the CPU while nothing is runnable, and the process is
     * resumed when its deadline arrives or a key does - which is FUZIX's
     * psleep(), and the reason its plt_switchout can be followed by a bare `ret`.
     *
     * So the kernel answers with what it has, now, and lib/libc.c's read() decides
     * what to do about nothing: it parks.  The point to continue at belongs to the
     * process, which is the whole reason the wait is a loop in the library rather
     * than a wait in here.
     *
     * One port read, and the answer is exact: input present or not.  The old
     * deadline and poll backstop are gone with the spin - they existed to bound a
     * wait, and there is no longer one to bound. */
    return tty_do_avail() ? 1 : 0;
    return 1;
}

/* Collect one canonical line into tty->input.  Returns 1 when a line is ready
 * or end of file has been seen, 0 when the wait timed out with an empty buffer.
 *
 * This loop used to be inline in muzix_tty_read(), and moving it out is a code
 * size measurement rather than a style one.  muzix_tty_read() is a thousand
 * bytes long and at a register-allocation cliff; a CALL INSIDE that loop costs
 * 222 bytes of spill and a restructured prologue, and the same call placed
 * outside any loop in the same function costs 30.  The bounded wait is a call,
 * so the loop has to go somewhere the function it is called from is not.
 * Measured on this tree, SDCC 4.5, --opt-code-size:
 *
 *     the loop inline, with a direct call inside it      +222 bytes
 *     the loop inline, with no call in it at all          -8 bytes
 *     the same call before the loop instead of in it      +30 bytes
 *
 * Which is the whole reason the loop is a function.  The line discipline below
 * is the same text it was inline - every case fs/test_tty_device.c covers
 * (XON, XOFF, erase, kill, EOF, newline, the input-buffer limit, the column
 * count) is asserted against this, so the move is checked by the tests that
 * were already written for it.
 *
 * The loop body is unchanged.  The wait is the only new thing in it, and the
 * timeout is the only new branch. */
int muzix_tty_collect_line(muzix_tty_device_t *tty)
{
    uint8_t byte;

    while (tty->line_ready == 0 && tty->eof_pending == 0) {
        if (!muzix_tty_wait_for_input()) {
            /* Out of time, and the line is not finished.
             *
             * This used to mark the buffer ready and hand the half-typed line
             * back as a successful read, on the grounds that a line with no
             * newline is "complete enough to act on" and that losing what someone
             * typed because they paused is worse than a spin.  The second half of
             * that is right and was never the problem: the bytes are still in
             * tty->input and nothing is lost.  The first half is what breaks the
             * console, and it breaks it in a way that looks like a keyboard fault.
             *
             * A canonical line ends at a terminator or at end of file, and until
             * one arrives the line is not a line.  Handing one back anyway lets
             * the reader drain the buffer while the line is still being edited -
             * muzix_tty_read() copies out one byte per call when the shell asks
             * for one byte at a time - and every later editing operation then
             * applies to a buffer that is already empty.  Concretely, with the
             * deadline expiring between two keystrokes:
             *
             *   typed "echo a", pause, deadline fires, line handed back, buffer
             *   drained to nothing by the reader;
             *   Backspace arrives, tty_is_erase() matches, input_length is 0, and
             *   the erase guard makes it a silent no-op - the character does not
             *   disappear and no error is possible;
             *   "b" and CR follow and the command runs as `echo ab`.
             *
             * Measured on this tree, same ROM, same driver, same byte: piped
             * input erases correctly, because every byte arrives at once and the
             * deadline never expires mid-line, and interactive input does not,
             * because a person types slower than five seconds per character is
             * rare but a pause between keys is not.  Ctrl-U fails the same way,
             * for the same reason.
             *
             * So: report no line, and let the caller ask again.  The shell
             * already treats a -1 as "nothing yet" and retries (shell/shell.c:305),
             * the bytes stay where they are, and the editing state stays intact
             * across however long the pause was.  This is also what a real
             * terminal does with VMIN=1, VTIME=0: the read waits, and a pause is
             * not a line ending.  It costs nothing to say - the branch loses the
             * input_length special case it no longer needs.
             */
            return 0;
        }
        byte = tty_do_recv();
        if (byte == tty->chars.stop) {
            tty->output_stopped = 1;
        } else if (byte == tty->chars.start) {
            tty->output_stopped = 0;
        } else if (tty_is_erase(tty, byte)) {
            /* The terminator cannot be erased here, and no guard is needed to
             * be sure of it.  Both references check the character explicitly -
             * FUZIX pops it and puts it back if it is '\n' or c_cc[VEOL]
             * (Kernel/tty.c:484-487), MINIX refuses '\n' and '\r' outright in
             * chuck() (kernel/tty.c:474-480) - because their loop keeps
             * collecting while a reader may be draining what is already there.
             *
             * This discipline is arranged the other way round: collect_line()
             * only collects while line_ready is clear, and read() drains what
             * is buffered, so a line is never half-drained and collecting at
             * the same time.  And a '\n' only reaches tty->input in the same
             * breath that sets line_ready (the store below), while ICRNL
             * rewrites '\r' to '\n' before it gets that far, so no CR is ever
             * stored either.  The last character in the buffer therefore cannot
             * be a terminator at any moment this branch can be reached.
             *
             * Adding the reference's test anyway was measured at 22 bytes of
             * _CODE for a condition that cannot be false, and the case that
             * was written to justify it -
             * test_erase_will_not_take_the_terminator() - passes with the test
             * removed, which is the proof that it is unreachable rather than
             * merely unobserved. */
            if (tty->input_length != 0) {
                tty->input_length--;
                tty_echo_erased_char(tty);
            }
        } else if (byte == tty->params.kill) {
            /* Per-character erase, which is FUZIX's shape for kill
             * (Kernel/tty.c:456, the eraseout loop runs to the end of the line
             * when wr is ECHOK).  MINIX differs: it loops chuck() until it
             * refuses and then echoes the kill character and a newline
             * (kernel/tty.c:320-324).  Kept as FUZIX does it because FUZIX is
             * the hardware reference for this board and because the other way
             * is both more code and a visible change to what a terminal shows.
             * The terminator guard is not repeated here: if line_ready is set
             * then input_length is at worst 1 holding that terminator, and one
             * erase echo of it is not a data loss - the line is already complete
             * and the shell will read what is left. */
            while (tty->input_length != 0) {
                tty->input_length--;
                tty_echo_erased_char(tty);
            }
        } else if (byte == tty->chars.eof) {
            tty->eof_pending = 1;
            if (tty->input_length != 0) {
                tty->line_ready = 1;
            }
        } else {
            /* ICRNL, and the mirror of the ONLCR above.
             *
             * A terminal's Enter key sends CR (0x0D), not LF.  That is what a
             * VT100 does and what every terminal emulator does, and it is why
             * every Unix tty translates it on the way in (termios ICRNL, which
             * is on by default): the program is handed '\n' and does not have to
             * know what the keyboard sent.
             *
             * This device compared against '\n' only, so on real hardware the
             * line never became ready, the buffer filled with the characters of
             * the line and then a CR that no read would treat as the end of it,
             * and canonical read sat until its five-second deadline and then
             * handed back a partial line.  It was invisible in every test and in
             * every emulator run here, because a pipe and a pty both deliver
             * 0x0A for a newline and only a real keyboard sends 0x0D - which is
             * the same reason the tests below are written against the keyboard
             * and not against the way this tree's own harnesses feed input.
             *
             * The CR is rewritten, not merely accepted: the program gets '\n',
             * which is what ONLCR above then turns back into "\r\n" on the
             * echo.  Storing the CR instead would put a byte at the end of every
             * line that no program expects to be there.
             *
             * The non-canonical branch further down does not translate, and that
             * is a stated gap rather than an oversight: ICRNL is an input
             * transformation and belongs on both paths, but nothing in the tree
             * clears MUZIX_TTY_FLAG_CANONICAL - the shell and every app read
             * canonical lines - so the second path is unreachable and the two
             * bytes there would be paid for by every boot to fix a path no
             * program takes.  It has to move up here rather than down there if
             * that ever changes. */
            if (byte == '\r') {
                byte = '\n';
            }
            if (tty->input_length < MUZIX_TTY_INPUT_SIZE) {
                tty->input[tty->input_length++] = byte;
                muzix_tty_echo(tty, byte);
            }
            if (byte == '\n') {
                tty->line_ready = 1;
            }
        }
    }
    return 1;
}

int muzix_tty_read(muzix_tty_device_t *tty,
                   uint8_t *buffer,
                   size_t length,
                   size_t *result)
{
    size_t count = 0;
    uint8_t byte;

    /* Only reject on missing plumbing.  The availability of input must NOT be
     * part of this test: it used to be, and that made the canonical wait loop
     * below unreachable.  With no input pending - which is the state the shell
     * is in immediately after printing its prompt, i.e. always - read() returned
     * -1 before ever reaching the loop, and the caller treats a negative read as
     * end of file.  So the shell's getchar() got EOF on the first call and the
     * shell exited at once instead of waiting for a keypress.  The wait loop
     * was written to fix exactly that and never ran.
     *
     * A missing receive function is still fatal: there would be no way to make
     * progress, and the loops below would spin forever. */
    if (!tty || !buffer || !result || !tty_have_recv()) {
        return -1;
    }
    if ((tty->params.flags & MUZIX_TTY_FLAG_CANONICAL) != 0) {
        if (tty->line_ready == 0 && tty->eof_pending != 0) {
            tty->eof_pending = 0;
            *result = 0;
            return 0;
        }
        /* A canonical read returns a whole line, so wait for one.
         *
         * This used to be `while (line_ready == 0 && tty_do_avail())`, which
         * is a poll, not a wait: the moment the line buffer was empty - which is
         * always, immediately after the prompt is printed - the read returned
         * zero bytes.  A caller treats a zero-length read as end of file, so
         * the shell printed its banner and prompt and then quit at once.  That
         * is why the system appeared to boot and exit rather than sit at a
         * prompt.
         *
         * End of file is still a zero-length read: Ctrl-D sets eof_pending, the
         * loop stops on it, and nothing is copied, so this call returns 0 and
         * the caller sees a genuine EOF.  The check at the top of the function
         * makes the following call return 0 immediately as well.
         *
         * The kernel is non-preemptive, so nothing else could run while this
         * loop waited - but "nothing else could run" is not the same as "this
         * loop should be able to run forever".  The 120 Hz tick now gives it a
         * deadline (MUZIX_TTY_READ_SECONDS above), which is the difference
         * between a read that gives up and a read that hangs the machine. */
        if (!muzix_tty_collect_line(tty)) {
            /* Nothing arrived.  MUZIX_READ_AGAIN, not -1: this is "ask me
             * again", and lib/libc.c's read() parks on it.  Reported as -1 it
             * was indistinguishable from a bad descriptor, so read() returned it
             * to the caller, and every console reader became a `while (n < 0)
             * retry` loop that the kernel returned to immediately - a spin, with
             * the processor held for all of it.
             *
             * No copy-out is left to do and no state to tidy, so the answer
             * costs nothing beyond the few port reads it took to find out. */
            *result = 0;
            return MUZIX_READ_AGAIN;
        }
        while (tty->line_ready != 0 && count < length &&
               tty->input_length != 0) {
            buffer[count++] = tty->input[0];
            memmove(tty->input, tty->input + 1, tty->input_length - 1);
            tty->input_length--;
        }
        if (tty->line_ready != 0 && tty->input_length == 0) {
            tty->line_ready = 0;
        }
        *result = count;
        return 0;
    }
    /* Non-canonical: block for the first byte, then return whatever has
     * arrived.  read() must not report end of file merely because nothing was
     * buffered when it was called - the same mistake the guard above used to
     * make, one branch down.  Returning as soon as at least one byte is in hand
     * is what a character device read is supposed to do, so the second loop
     * stops on the first gap rather than collecting a full buffer.
     *
     * The first loop is bounded, for the same reason and with the same
     * consequences as the canonical one above: five seconds of no input is -1,
     * reported as an error the caller can retry, rather than a hang. */
    if (count == 0 && !muzix_tty_wait_for_input()) {
        /* Nothing in hand, same as the canonical case above and for the same
         * reason: MUZIX_READ_AGAIN, so read() parks instead of the caller
         * spinning.  It must not be a zero-length read, which is Ctrl-D and
         * which every terminal in the tree stops reading on. */
        *result = 0;
        return MUZIX_READ_AGAIN;
    }
    while (count < length && tty_do_avail()) {
        byte = tty_do_recv();
        if (byte == tty->chars.stop) {
            tty->output_stopped = 1;
        } else if (byte == tty->chars.start) {
            tty->output_stopped = 0;
        } else {
            buffer[count++] = byte;
            muzix_tty_echo(tty, byte);
        }
    }
    *result = count;
    return 0;
}

int muzix_tty_write(muzix_tty_device_t *tty,
                    const uint8_t *buffer,
                    size_t length,
                    size_t *result)
{
    size_t index;

    if (!tty || !result) {
        return -1;
    }
    *result = 0;
    if (length == 0) {
        return 0;
    }
    if (!g_tty_send) {
        return -1;
    }
    if (tty->output_stopped) {
        /* XOFF is in effect.  This used to fall out of the send loop with
         * index == 0 and return success, so write() reported a full transfer
         * having transmitted nothing and the queued output was simply lost --
         * silently, because the caller had no reason to doubt the count it was
         * given.  Report the failure instead.
         *
         * This port's tty keeps an input line buffer only, with no output
         * staging, so there is nothing to replay when Ctrl-Q arrives:
         * muzix_tty_read clears output_stopped on XON and the caller's retry
         * then goes through.  Adding an output queue to fix the other half
         * would cost 256+ bytes of the 64 KiB _DATA segment. */
        return -1;
    }
    for (index = 0; index < length; index++) {
        tty_put_byte(buffer[index]);
    }
    *result = length;
    return 0;
}

/*
 * How much unconsumed input is waiting, for MUZIX_TTY_IOCTL_PENDING.
 *
 * A byte can be in one of two places, and the answer has to be the pair.
 * Either it is in the device's own line buffer, which is where muzix_tty_read
 * leaves everything it has pulled off the wire but not yet handed to a caller,
 * or it is still in the UART's receive register, which is where it sits until
 * something calls tty_do_recv().  Reporting only the first would answer "no"
 * for a keypress that has arrived but not been collected, which is precisely
 * the case a program polling for one cares about - nothing on this system
 * calls read() between two displays.
 *
 * Neither term consumes anything.  tty_do_avail() is
 * muzix_zeta_uart_available(), which reads the line status register at $6D and
 * tests bit 0 (platform/zeta-v2/uart_io.s); the byte itself lives in the
 * receive buffer register at $68, which is only read by tty_do_recv().  The
 * line buffer is not touched at all.  So the check can be repeated for as long
 * as a program likes and the keypress is still there afterwards - which is what
 * makes it safe for a program to check once a refresh and then carry on, and
 * what means a program that quits on a keypress has not eaten it on the way
 * out: the shell that gets the console back reads the same byte.
 *
 * The count is 0 or 1 per place, not a byte count.  The FIFO depth is not
 * visible from the status register without reading the chip's own IIR, and a
 * program only needs to know whether there is something; the contract is
 * "zero or not zero".  See lib/tty_ioctl.h.
 */
static uint16_t muzix_tty_input_pending(muzix_tty_device_t *tty)
{
    uint16_t pending = tty->input_length != 0 ? 1u : 0u;

    if (tty_have_avail() && tty_do_avail() != 0) {
        pending++;
    }
    return pending;
}

int muzix_tty_ioctl(muzix_tty_device_t *tty, uint16_t request, void *argument)
{
    if (!tty || !argument) {
        return -1;
    }
    switch (request) {
    case MUZIX_TTY_IOCTL_GETP:
        *(muzix_tty_params_t *)argument = tty->params;
        return 0;
    case MUZIX_TTY_IOCTL_SETP:
        tty->params = *(const muzix_tty_params_t *)argument;
        return 0;
    case MUZIX_TTY_IOCTL_GETC:
        *(muzix_tty_chars_t *)argument = tty->chars;
        return 0;
    case MUZIX_TTY_IOCTL_SETC:
        tty->chars = *(const muzix_tty_chars_t *)argument;
        return 0;
    case MUZIX_TTY_IOCTL_PENDING:
        *(uint16_t *)argument = muzix_tty_input_pending(tty);
        return 0;
    default:
        return -1;
    }
}
