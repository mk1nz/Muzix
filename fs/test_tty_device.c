#include "tty_device.h"

#include <string.h>

typedef struct {
    uint8_t tx[32];
    uint8_t rx[32];
    uint8_t tx_count;
    uint8_t rx_pos;
    uint8_t rx_count;
} test_serial_t;

static void test_send(uint8_t byte, void *userdata)
{
    test_serial_t *serial = (test_serial_t *)userdata;
    serial->tx[serial->tx_count++] = byte;
}

static uint8_t test_recv(void *userdata)
{
    test_serial_t *serial = (test_serial_t *)userdata;
    return serial->rx[serial->rx_pos++];
}

static uint8_t test_available(void *userdata)
{
    test_serial_t *serial = (test_serial_t *)userdata;
    return serial->rx_pos < serial->rx_count;
}

/* Does the fake line let time pass?
 *
 * The tick is what bounds muzix_tty_read's two wait loops, and the loop that
 * waits for input is a loop: it asks whether a byte is there, and asks again,
 * and the only thing that ever makes it stop is the deadline.  A test that
 * queues nothing and calls read() therefore does not return - it spins asking
 * the same question - which is what this file's own header records having done
 * before it, for the canonical-line case.
 *
 * So the fake line advances the host tick every time it is asked whether a byte
 * is there, which is the smallest honest model of "the CPU kept running and
 * time passed": nothing arrives, so nothing arrives, but 120 of them go by.  One
 * tick per poll is the fine-grained version; the number below is the coarse one,
 * and only the total matters to the code under test. */
extern uint16_t muzix_host_tick;
extern void muzix_host_tick_set(uint16_t ticks);

static uint8_t g_ticks_per_poll;

static uint8_t test_available_ticking(void *userdata)
{
    muzix_host_tick = (uint16_t)(muzix_host_tick + g_ticks_per_poll);
    return test_available(userdata);
}

/* Put back the line discipline muzix_tty_device_init() installs.
 *
 * The ioctl block in test_tty_device() above sets erase and kill to 1 and 2 and
 * the special characters to 3,4,5,6,7,8 through SETP and SETC, and the read
 * cases below use 8, 21, 17, 19 and 4 - which are the device's own defaults,
 * not the values a moment earlier in the same function replaced them with.  Both
 * assumptions were live and neither had ever been run, so every case below that
 * used a special character was reading a byte the device no longer treated as
 * special: the "ab\010c\n" case wanted erase, which was 1, and the "ab\004"
 * case wanted EOF, which was 7.  Naming the values per case is the fix; sharing
 * one helper keeps it to one line each and leaves the numbers visible.
 *
 * The erase byte here is 0x7F, which is what muzix_tty_device_init() now sets and
 * what a terminal actually sends.  It is the one field in this helper that is no
 * longer load-bearing either way: the device accepts 0x08 as an erase whatever
 * this says, so the "ab\010c\n" case in test_tty_device() is now a check of that
 * and not of the configured value. */
static void use_default_line_discipline(muzix_tty_device_t *tty, uint16_t flags)
{
    muzix_tty_params_t params = {0x7F, 21, flags};
    muzix_tty_chars_t chars = {3, 28, 17, 19, 4, 0};

    if (muzix_tty_ioctl(tty, MUZIX_TTY_IOCTL_SETP, &params) != 0 ||
        muzix_tty_ioctl(tty, MUZIX_TTY_IOCTL_SETC, &chars) != 0) {
        return;
    }
}

/* Every read below terminates, and that is the device's requirement rather than
 * this file's convenience.
 *
 * muzix_tty_read on a canonical device waits for a whole line, and the wait loop
 * has no way out: it spins until a newline arrives, until Ctrl-D arrives, or
 * until the input function runs dry and returns 0.  This device's input function
 * is test_available(), which answers 0 once its queue is empty, so the third of
 * those is a bare infinite loop.
 *
 * Three cases in the version of this file before it was run asked for a
 * zero-length read with no line behind it - the first with "world" and no
 * newline, the other two with only an XOFF or an XON - and all three spun there
 * for ever.  Measured, not inferred: test_tty_pending() returns 0 and
 * test_tty_device() never returns, and numbering the reads one at a time puts
 * the first hang on the "world" read.
 *
 * The wait is deliberate and is not being worked around here.  The comment above
 * the loop in fs/tty_device.c records what the alternative cost: read used to be
 * a poll, it returned zero bytes whenever the line buffer happened to be empty -
 * which is the shell's state immediately after printing its prompt - a caller
 * reads a zero-length read as end of file, and the shell exited at once instead
 * of sitting at a prompt.  So every case below either supplies a terminator or
 * asks for the EOF.  A zero-length read here means end of file and end of file
 * is Ctrl-D; the "ab\004" case is the one that shows it, and it is the only way
 * to observe a zero.
 *
 * The callbacks were never the problem, so nothing here waits on a console: this
 * file installs test_send/test_recv/test_available itself, which is what
 * -DMUZIX_TTY_CALLBACKS=1 is for, and the host runner passes it.
 */
static int test_tty_device(void)
{
    muzix_tty_device_t tty;
    muzix_tty_params_t params = {1, 2, 0x1234u};
    muzix_tty_params_t loaded;
    muzix_tty_chars_t chars = {3, 4, 5, 6, 7, 8};
    muzix_tty_chars_t loaded_chars;
    test_serial_t serial;
    uint8_t input[5] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t output[5] = {0, 0, 0, 0, 0};
    size_t count;

    muzix_tty_device_init(&tty);
    memset(&serial, 0, sizeof(serial));
    memcpy(serial.rx, "world\n", 6);
    serial.rx_count = 6;
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);
    if (muzix_tty_write(&tty, input, sizeof(input), &count) != 0 ||
        count != sizeof(input) || memcmp(serial.tx, input, sizeof(input)) != 0) {
        return 1;
    }
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 5 || memcmp(output, "world", 5) != 0) {
        return 2;
    }
    /* The newline is the sixth byte of the line and the buffer asked for five,
     * so it is still there: a canonical read hands a line over in pieces when
     * the caller's buffer is short, and the piece left behind is a character the
     * user typed. */
    if (muzix_tty_read(&tty, output, 1, &count) != 0 ||
        count != 1 || output[0] != '\n') {
        return 3;
    }
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_SETP, &params) != 0 ||
        muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_GETP, &loaded) != 0 ||
        loaded.erase != 1 || loaded.kill != 2 || loaded.flags != 0x1234u ||
        muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_SETC, &chars) != 0 ||
        muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_GETC, &loaded_chars) != 0 ||
        loaded_chars.intr != 3 || loaded_chars.quit != 4 ||
        loaded_chars.start != 5 || loaded_chars.stop != 6 ||
        loaded_chars.eof != 7 || loaded_chars.brk != 8 ||
        muzix_tty_ioctl(&tty, 0xffffu, &loaded) == 0 ||
        muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_GETP, 0) == 0) {
        return 4;
    }
    use_default_line_discipline(&tty, MUZIX_TTY_FLAG_CANONICAL);
    memset(&serial, 0, sizeof(serial));
    memcpy(serial.rx, "ab\010c\n", 5);
    serial.rx_count = 5;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 3 || output[0] != 'a' || output[1] != 'c' ||
        output[2] != '\n') {
        return 5;
    }
    memset(&serial, 0, sizeof(serial));
    memcpy(serial.rx, "ab\025c\n", 5);
    serial.rx_count = 5;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 2 || output[0] != 'c' || output[1] != '\n') {
        return 6;
    }
    /* EOF is the only zero, so this is the only case that can ask for one. */
    memset(&serial, 0, sizeof(serial));
    memcpy(serial.rx, "ab\004", 3);
    serial.rx_count = 3;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 2 || output[0] != 'a' || output[1] != 'b' ||
        muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 0) {
        return 7;
    }
    use_default_line_discipline(&tty,
                                MUZIX_TTY_FLAG_CANONICAL | MUZIX_TTY_FLAG_ECHO);
    memset(&serial, 0, sizeof(serial));
    memcpy(serial.rx, "ab\010c\n", 5);
    serial.rx_count = 5;
    /* The erase is a back, a space and a back, echoed exactly as the user sees
     * it, and the line the read returns has the erased character gone.  The
     * echo is EIGHT bytes where this used to say seven, and the eighth is the
     * same deliberate change as case 11 above: the terminator goes out as
     * CR LF, because that is what ends a line on a terminal. */
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 3 || memcmp(output, "ac\n", 3) != 0 ||
        serial.tx_count != 8 || memcmp(serial.tx, "ab\010 \010c\r\n", 8) != 0) {
        return 8;
    }
    /* XOFF and XON are flow control, not input: the device eats them and never
     * hands them to a reader, which is what the line behind each one shows here.
     * The zero-length read the earlier version asked for after an XOFF was never
     * a thing this device promised - it waits - so what is checked instead is
     * that the flow-control byte is absent from the line and from the echo, and
     * that a write is refused rather than silently dropped while output is
     * stopped. */
    /* The flow-control byte is absent from the line and from the echo, and the
     * terminator in the echo is CR LF - the same ONLCR change as the two cases
     * above, which is why every echo in this file ends "\r\n" and not "\n". */
    memset(&serial, 0, sizeof(serial));
    serial.rx[0] = 19;
    serial.rx[1] = 'a';
    serial.rx[2] = '\n';
    serial.rx_count = 3;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 2 || output[0] != 'a' || output[1] != '\n' ||
        serial.tx_count != 3 || memcmp(serial.tx, "a\r\n", 3) != 0) {
        return 9;
    }
    /* Output is stopped now, so a write has to be refused rather than reported
     * as a full transfer having sent nothing.  This was checked inside the ||-chain
     * above as `muzix_tty_write(...) != 0`, which cannot hold: a non-zero return is
     * exactly what makes the chain fire and report a failure, so the case asserted
     * the opposite of itself and would have failed here as well as hanging. */
    if (muzix_tty_write(&tty, input, sizeof(input), &count) == 0 ||
        count != 0) {
        return 10;
    }
    memset(&serial, 0, sizeof(serial));
    serial.rx[0] = 17;
    serial.rx[1] = 'b';
    serial.rx[2] = '\n';
    serial.rx_count = 3;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
        count != 2 || output[0] != 'b' || output[1] != '\n' ||
        serial.tx_count != 3 || memcmp(serial.tx, "b\r\n", 3) != 0 ||
        muzix_tty_write(&tty, input, sizeof(input), &count) != 0 ||
        count != sizeof(input) ||
        memcmp(&serial.tx[serial.tx_count - sizeof(input)], input,
               sizeof(input)) != 0) {
        return 11;
    }
    return 0;
}

/* MUZIX_TTY_IOCTL_PENDING, checked against the two things it must be and the
 * one it must not be.
 *
 * The one it must not be is the important one: a check that consumed the byte
 * would leave a program that polls it unable to see its own keypress, and would
 * take from the shell the keypress that stopped it.  So each case below asks
 * twice - once to see the answer, once to prove the answer has not changed -
 * and then reads the line through the ordinary read() path and checks the
 * characters arrive in the order they were typed.  A request that consumed
 * cannot pass that. */
static int test_tty_pending(void)
{
    muzix_tty_device_t tty;
    test_serial_t serial;
    uint16_t pending;
    uint8_t output[4];

    muzix_tty_device_init(&tty);
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    /* Nothing anywhere: the answer is 0, and asking twice is 0 twice, and
     * nothing has been echoed, because asking is not reading. */
    pending = 0xffffu;
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
        pending != 0) {
        return 1;
    }
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
        pending != 0 || serial.tx_count != 0) {
        return 2;
    }

    /* One byte in the receive register and not yet collected.  The device must
     * see it - this is the whole point, a program polling between two displays
     * has nothing else looking - and asking again must find it still there. */
    serial.rx[0] = 'q';
    serial.rx_count = 1;
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
        pending == 0) {
        return 3;
    }
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
        pending == 0) {
        return 4;
    }
    /* Still nothing echoed: nothing has read the byte yet. */
    if (serial.tx_count != 0) {
        return 5;
    }

    /* A canonical read still gets the byte, in order, and echoes it once.  If
     * the request had taken the byte this read would block for ever or come
     * back short, and neither is a thing this test can wait for - it would be
     * the hang the whole design is avoiding. */
    serial.rx[serial.rx_count++] = '\n';
    {
        size_t count;

        if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0 ||
            count != 2 || output[0] != 'q' || output[1] != '\n') {
            return 6;
        }
    }
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
        pending != 0) {
        return 7;
    }

    /* A byte already collected into the device's own line buffer and not yet
     * handed to a caller: a canonical read with a short buffer takes the line
     * in pieces and leaves the rest behind.  That leftover is a keypress the
     * user made, so it has to count as pending - and asking must not drain it,
     * which the next read shows. */
    memset(&serial, 0, sizeof(serial));
    memcpy(serial.rx, "ab\n", 3);
    serial.rx_count = 3;
    {
        size_t count;
        uint8_t one[1];

        if (muzix_tty_read(&tty, one, sizeof(one), &count) != 0 ||
            count != 1 || one[0] != 'a') {
            return 8;
        }
        if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
            pending == 0) {
            return 9;
        }
        if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0 ||
            pending == 0) {
            return 10;
        }
        /* The buffer still holds what those two calls looked at: the second
         * read comes back with the 'b' the first one left.  And the echo is
         * four bytes, all from the two reads - asking about input added nothing
         * to the output, so the request neither drained the buffer nor
         * disturbed the line discipline.
         *
         * FOUR, where this used to say three, and the fourth is the point rather
         * than an accident: the terminator arrives here as '\n' and the wire
         * carries "\r\n" (ONLCR, muzix_tty_echo -> tty_put_byte).  A terminal
         * ends a line with CR then LF, and an LF alone leaves the cursor one
         * column further right on every line. */
        if (muzix_tty_read(&tty, one, sizeof(one), &count) != 0 ||
            count != 1 || one[0] != 'b' ||
            serial.tx_count != 4 || serial.tx[0] != 'a' ||
            serial.tx[1] != 'b' || serial.tx[2] != '\r' ||
            serial.tx[3] != '\n') {
            return 11;
        }
    }

    /* With no availability function installed the request must still answer
     * rather than call through a null pointer - which is what a device with the
     * callbacks half-installed would do, and what the header's own note about
     * the kernel never installing one would otherwise hide. */
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv, 0, &serial);
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, &pending) != 0) {
        return 12;
    }

    /* A NULL argument is refused, like every other request's. */
    if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_PENDING, 0) == 0) {
        return 13;
    }
    return 0;
}

/* The two deadlines, which are the whole point of the tick being here.
 *
 * Neither of these loops could be reached before: without a tick there was
 * nothing to bound them with, and a test that reached them anyway would have
 * spun.  Both cases below are the ones that used to be hangs, and the return
 * value is the part that matters - see the note on MUZIX_TTY_READ_SECONDS in
 * tty_device.c for why it is MUZIX_READ_AGAIN and not a zero-length read.
 *
 * That the answer is MUZIX_READ_AGAIN rather than -1 is itself load-bearing, and
 * these assertions are what pin it.  lib/libc.c's read() parks on exactly this
 * value; reported as -1 it was indistinguishable from a bad descriptor, read()
 * could not park on it, and every console caller became a spin.  A test that
 * only asserted "negative" would have let that come straight back. */
static int test_tty_read_timeout(void)
{
    test_serial_t serial;
    muzix_tty_device_t tty;
    uint8_t byte;
    size_t got = 0;
    int rc;

    memset(&serial, 0, sizeof(serial));

    /* Canonical, with an empty line buffer. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available_ticking, &serial);
    muzix_host_tick_set(0);
    g_ticks_per_poll = 1;
    rc = muzix_tty_read(&tty, &byte, 1, &got);
    g_ticks_per_poll = 0;
    if (rc != MUZIX_READ_AGAIN) {
        return 31;
    }
    if (got != 0) {
        return 32;
    }
    /* And essentially NO time has gone by.  That is the stronger version of what
     * this used to assert.
     *
     * It asserted "five seconds have gone by, which is 600 ticks at 120 Hz, and
     * no more than five seconds" - which bounded a wait.  There is no longer a
     * wait here: fs/tty_device.c answers with what it has, now, and lib/libc.c's
     * read() parks when the answer is nothing.  A read with no input costs a
     * handful of port reads.
     *
     * The bound is not zero because the test's own `available` callback advances
     * the tick by one per call, so the port reads themselves tick it - which is
     * also why this assertion can see a spin at all: a spin is 600 of those, and
     * anything near 600 here means it is back.  Every tick that passes with
     * nothing to do is a tick the machine reported as busy while doing nothing,
     * and making that figure honest is the whole point. */
    if (muzix_host_tick > 8u) {
        return 33;
    }

    /* Non-canonical, same thing.  A raw-mode read has no line to salvage, so
     * this is the plain "nothing arrived" case and the same -1. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available_ticking, &serial);
    {
        muzix_tty_params_t params = {8, 21, 0};
        if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_SETP, &params) != 0) {
            return 35;
        }
    }
    muzix_host_tick_set(0);
    g_ticks_per_poll = 1;
    rc = muzix_tty_read(&tty, &byte, 1, &got);
    g_ticks_per_poll = 0;
    if (rc != MUZIX_READ_AGAIN) {
        return 36;
    }
    if (got != 0) {
        return 37;
    }

    /* And the case that matters most: a deadline bounds how long a read may
     * wait, and a byte that is already queued must not be ignored for the rest
     * of the budget.  It is collected - the tick advances inside the poll - and
     * then the read has to come back WITHOUT it, because a canonical line with
     * no terminator is not a line.
     *
     * It used to come back with it.  That is the defect this now pins: the
     * caller drained the buffer while the line was still being edited, and every
     * later Backspace or Ctrl-U then found an empty buffer and did nothing,
     * silently.  Measured as "erases over a pty, does not over a pipe", because
     * a pipe delivers the whole line at once and the deadline never expires
     * inside one. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available_ticking, &serial);
    muzix_host_tick_set(0);
    g_ticks_per_poll = 1;
    serial.rx[0] = 'k';
    serial.rx_count = 1;
    rc = muzix_tty_read(&tty, &byte, 1, &got);
    g_ticks_per_poll = 0;
    if (rc != MUZIX_READ_AGAIN) {
        return 39;
    }
    if (got != 0) {
        return 40;
    }

    /* And nothing was lost, which is the whole reason for the change: the byte
     * is still in the line buffer, and the line comes back intact as soon as
     * the terminator arrives.  This is the assertion that replaces the old
     * "a partial line is handed back as a successful read" - the data survives,
     * it is simply not delivered early. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available_ticking, &serial);
    muzix_host_tick_set(0);
    g_ticks_per_poll = 1;
    serial.rx[0] = 'a';
    serial.rx[1] = 'b';
    serial.rx[2] = '\n';
    serial.rx_count = 3;
    {
        uint8_t whole[3];

        rc = muzix_tty_read(&tty, whole, sizeof(whole), &got);
        if (rc != 0) {
            return 41;
        }
        if (got != 3 || memcmp(whole, "ab\n", 3) != 0) {
            return 42;
        }
    }
    return 0;
}

/* What a real keyboard and a real terminal actually exchange, in both directions.
 *
 * WHY THIS FILE HAD NO CASE FOR EITHER
 * ------------------------------------
 * Every other case here feeds the receive side '\n' (0x0A) and every one of
 * them passed on a device that could not talk to a terminal at all.  The reason
 * is the harness, not the code under test: a pipe and a pty both deliver LF for
 * a newline, and this tree's own emulator bridges forward whatever the pty hands
 * them, so nothing in any test in this file ever put a CR (0x0D) on the wire -
 * which is the only thing a real terminal's Enter key sends.  A pipe also
 * swallows the difference on the way out, because what came back was never
 * compared against a carriage return.
 *
 * So the two defects this covers were both invisible to every test that existed,
 * and both were visible at a keyboard:
 *
 *   - output.  Every byte a program wrote, and every byte of the echo, went out
 *     as written.  A '\n' is line feed, which moves the cursor down one row and
 *     leaves the column alone, so every line the system printed started further
 *     right than the last.  The wire wants CR LF.
 *   - input.  Only LF ended a line, and a terminal sends CR, so a typed line
 *     never became ready: canonical read waited out its five-second deadline and
 *     handed back a partial line.
 *
 * The pair is one mechanism seen from two ends - termios calls them ONLCR and
 * ICRNL - so they are one function here too.  Both directions are checked in the
 * same case because either one alone would pass on the harness that exists and
 * still be wrong on the machine.
 */
static int test_terminal_line_endings(void)
{
    muzix_tty_device_t tty;
    test_serial_t serial;
    uint8_t output[8];
    size_t count = 0;

    /* OUT: what a program writes is what the terminal has to be able to render.
     * One newline in, CR LF out, and every other byte untouched - a write that
     * mangled something else would be just as wrong as one that forgot the CR. */
    muzix_tty_device_init(&tty);
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    if (muzix_tty_write(&tty, (const uint8_t *)"a\nb", 3u, &count) != 0 ||
        count != 3u) {
        return 1;
    }
    if (serial.tx_count != 4u || serial.tx[0] != 'a' ||
        serial.tx[1] != '\r' || serial.tx[2] != '\n' ||
        serial.tx[3] != 'b') {
        return 2;
    }

    /* IN: the keyboard's Enter is CR, and a canonical line ends on it.  If this
     * read waited for its deadline instead the program would see a partial line
     * five seconds late, every time.
     *
     * This case also covers the echo of the terminator, which is why there is no
     * separate case that calls the echo path directly: it is a static function,
     * and a terminal pressing Enter is a write of the terminator by the
     * keyboard rather than by a program, so the input path is the honest place
     * to watch it.  Below, the echo of that CR comes back out as CR LF - which is
     * the same wire as the program's own output above. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    serial.rx[0] = 'h';
    serial.rx[1] = 'i';
    serial.rx[2] = '\r';
    serial.rx_count = 3;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
        return 3;
    }
    /* The program is handed LF, not the CR the keyboard sent: ICRNL rewrites the
     * byte rather than only recognising it.  Handing a program a line that ends
     * in CR is how a shell that compares the last byte to '\n' breaks while
     * looking perfectly correct. */
    if (count != 3u || output[0] != 'h' || output[1] != 'i' ||
        output[2] != '\n') {
        return 4;
    }
    /* And the echo of that terminator is CR LF: without the CR the cursor is
     * left standing at the end of the line the user just typed, which is the
     * other half of the staircase and the half a user notices first. */
    if (serial.tx_count != 4u || serial.tx[0] != 'h' ||
        serial.tx[1] != 'i' || serial.tx[2] != '\r' ||
        serial.tx[3] != '\n') {
        return 5;
    }

    /* A bare LF still ends a line, because something that is not a keyboard can
     * send one - a pipe, a pty, a program writing to the console.  ICRNL adds a
     * terminator the device did not have; it must not take the old one away.
     * This is the case every other test in this file already covered, and it is
     * why none of them caught the missing CR: they all arrive this way. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    serial.rx[0] = 'o';
    serial.rx[1] = '\n';
    serial.rx_count = 2;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
        return 6;
    }
    if (count != 2u || output[0] != 'o' || output[1] != '\n') {
        return 7;
    }
    return 0;
}

/* The Backspace key, which is two bytes and neither of them was enough.
 *
 * WHY THIS IS A SEPARATE FUNCTION
 * -------------------------------
 * The device took 0x08 as its erase byte and nothing else, and 0x08 is what a
 * VT100 sends.  Every terminal a person is likely to be sitting at right now -
 * xterm, PuTTY, Terminal.app, the Linux virtual console - sends 0x7F instead,
 * and that is what this tree's own emulator hands the kernel when a host pty
 * delivers a Backspace.  So the key that deletes a character is the key most
 * users press, and on the default terminal it did nothing at all.
 *
 * Nothing was visible when it failed, and that is what makes it worth its own
 * function rather than another case inside the erase one.  An unrecognised 0x7F
 * was stored as ordinary text and echoed verbatim; 0x7F is not a printable
 * character, so the screen showed no key having been pressed while the line
 * quietly filled with bytes that then went into the command.  On the wire, from
 * the same ROM:
 *
 *     echo abc<BS><BS>def    ->  adef                  works
 *     echo abc<DEL><DEL>def   ->  abc<DEL><DEL>def     the same key, doing nothing
 *
 * So the two assertions that matter are not "does 0x08 erase" - which the rest
 * of this file already covers - but "does 0x7F erase" and "does the program ever
 * see a 0x7F".  The second is the one that would catch a fix which recognised
 * the key and then stored it anyway.
 *
 * The first three cases below install no line discipline through the ioctl, and
 * that is the point rather than laziness: what was wrong was the byte
 * muzix_tty_device_init() ships, so a case that sets its own erase through SETP
 * proves nothing about it.  The helper above would have overwritten the very
 * default under test and the case would have passed against the broken device.
 * Only the last case uses SETP, because that one is about SETP.
 */
static int test_backspace_erases(void)
{
    muzix_tty_device_t tty;
    test_serial_t serial;
    uint8_t output[8];
    size_t count = 0;

    /* The byte a modern terminal sends.  Two of them take off the two characters
     * before them, and what is left is what the program is given. */
    muzix_tty_device_init(&tty);
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    serial.rx[0] = 'a';
    serial.rx[1] = 'b';
    serial.rx[2] = 0x7F;
    serial.rx[3] = 0x7F;
    serial.rx[4] = 'c';
    serial.rx[5] = '\n';
    serial.rx_count = 6;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
        return 100;
    }
    /* A canonical read hands back the line with its terminator, so "c" plus the
     * newline - two bytes, not one. */
    if (count != 2u || output[0] != 'c' || output[1] != '\n') {
        return 101;
    }
    /* The erase echo is back, space, back - the space is what removes the glyph,
     * because a terminal moves left over a character without touching it.  So
     * the wire is the two characters, six bytes for two erases, the character
     * typed after them, and CR LF for the terminator the line ends on.  A DEL
     * echoed verbatim would show up in this count as a 0x7F instead. */
    if (serial.tx_count != 11u || serial.tx[0] != 'a' || serial.tx[1] != 'b' ||
        serial.tx[2] != '\b' || serial.tx[3] != ' ' || serial.tx[4] != '\b' ||
        serial.tx[5] != '\b' || serial.tx[6] != ' ' || serial.tx[7] != '\b' ||
        serial.tx[8] != 'c' || serial.tx[9] != '\r' || serial.tx[10] != '\n') {
        return 102;
    }

    /* And the other one still works, which the case above cannot show because
     * the discipline it installed sets erase to 0x7F.  This one is the old
     * default and the byte a real serial terminal sends. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    serial.rx[0] = 'a';
    serial.rx[1] = 0x08;
    serial.rx[2] = 'b';
    serial.rx[3] = '\n';
    serial.rx_count = 4;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
        return 103;
    }
    if (count != 2u || output[0] != 'b' || output[1] != '\n') {
        return 104;
    }

    /* Erase on an empty line must be silent rather than a wedge.  This is the
     * case that hangs if the loop below is written as "always echo three
     * bytes": there is nothing to move the cursor back over, and a terminal
     * that has already wrapped would be walked off the left edge. */
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_init(&tty);
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    serial.rx[0] = 0x7F;
    serial.rx[1] = 0x7F;
    serial.rx[2] = '\n';
    serial.rx_count = 3;
    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
        return 105;
    }
    if (count != 1u || output[0] != '\n' || serial.tx_count != 2u ||
        serial.tx[0] != '\r' || serial.tx[1] != '\n') {
        return 106;
    }

    /* A character set through SETP is still a character that erases, so the
     * ioctl keeps meaning something now that the device no longer compares
     * against that field alone.  0x0B is free: it is not one of the six
     * special characters above. */
    {
        muzix_tty_params_t params = {0x0B, 21, MUZIX_TTY_FLAG_CANONICAL |
                                                MUZIX_TTY_FLAG_ECHO};

        memset(&serial, 0, sizeof(serial));
        muzix_tty_device_init(&tty);
        muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                       test_available, &serial);
        if (muzix_tty_ioctl(&tty, MUZIX_TTY_IOCTL_SETP, &params) != 0) {
            return 107;
        }

        serial.rx[0] = 'a';
        serial.rx[1] = 0x0B;
        serial.rx[2] = 'b';
        serial.rx[3] = '\n';
        serial.rx_count = 4;
        if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
            return 108;
        }
        if (count != 2u || output[0] != 'b' || output[1] != '\n') {
            return 109;
        }
    }
    return 0;
}

/*
 * Backspace must not eat the line terminator - and it cannot, which is the
 * point of the case.
 *
 * Both references check explicitly.  FUZIX pops the character and puts it back
 * when it is '\n' or c_cc[VEOL] (Kernel/tty.c:484-487); MINIX refuses '\n' and
 * '\r' outright in chuck() (kernel/tty.c:474-480).  Neither check is needed
 * here, and this is the case that establishes why rather than asserting it.
 *
 * The difference is organisational.  FUZIX and MINIX keep collecting input
 * while a reader may be draining what is already queued, so the last character
 * in the queue can be a terminator that a reader has not taken yet, and they
 * have to look.  This device collects only while line_ready is clear and
 * drains in read(), so the two never overlap; and a '\n' only enters the
 * buffer in the same breath that sets line_ready, with ICRNL having already
 * rewritten any '\r'.  The last character cannot be a terminator at any point
 * this branch is reachable.
 *
 * The state below is the one that would expose a difference: the shell reads
 * one byte per syscall, so a line longer than its buffer is handed out in
 * pieces and the terminator is left in the buffer with line_ready set.  The
 * Backspace queued behind it is not read at all, because collect_line() only
 * collects while line_ready is clear, and the read returns the terminator
 * intact.
 *
 * This test passes with no guard in the driver and with a guard in it, and the
 * guard measured 22 bytes of _CODE.  It is kept as the invariant it documents:
 * if someone reorders collect_line() and read() so that collection resumes
 * while a line is partly drained, this case is what notices.
 */
static int test_erase_will_not_take_the_terminator(void)
{
    muzix_tty_device_t tty;
    test_serial_t serial;
    uint8_t output[4];
    size_t count = 0;
    size_t tx_before;

    muzix_tty_device_init(&tty);
    memset(&serial, 0, sizeof(serial));
    muzix_tty_device_set_callbacks(&tty, test_send, test_recv,
                                   test_available, &serial);

    /* "ab" then the terminator, all queued: the line is complete before the
     * first read even starts. */
    serial.rx[0] = 'a';
    serial.rx[1] = 'b';
    serial.rx[2] = '\n';
    serial.rx_count = 3;

    /* Ask for two of the three.  The terminator is now the last thing in the
     * buffer and line_ready is set - the window this case is about. */
    if (muzix_tty_read(&tty, output, 2u, &count) != 0) {
        return 120;
    }
    if (count != 2u || output[0] != 'a' || output[1] != 'b') {
        return 121;
    }

    /* Now the Backspace, queued behind it. */
    serial.rx[3] = 0x7F;
    serial.rx_count = 4;
    tx_before = serial.tx_count;

    if (muzix_tty_read(&tty, output, sizeof(output), &count) != 0) {
        return 122;
    }
    /* The terminator survived: this read returns it rather than timing out. */
    if (count != 1u || output[0] != '\n') {
        return 123;
    }
    /* And nothing was echoed for the Backspace, because nothing was removed. */
    if (serial.tx_count != tx_before) {
        return 124;
    }
    return 0;
}

int main(void)
{
    /* Both phases run.  test_tty_device() used to be unreachable: its first case
     * queued "world" with no newline and read it, and a canonical console waits
     * for a whole line, so it spun there for ever - measured, not inferred, and
     * for the same reason the reachable checks were hoisted into test_tty_pending()
     * in the first place.  Every read in that function now has a terminator or
     * asks for the EOF; see the comment above it for which three cases could not
     * return and why that is the device's semantics rather than a fault.
     *
     * So there is no longer a reason to run one phase first, and doing so would
     * hide a failure in the other: the exit code is the failing case's number. */
    int status = test_tty_pending();

    if (status != 0) {
        return status;
    }
    status = test_tty_device();
    if (status != 0) {
        return status;
    }
    status = test_tty_read_timeout();
    if (status != 0) {
        return status;
    }
    status = test_backspace_erases();
    if (status != 0) {
        return status;
    }
    status = test_erase_will_not_take_the_terminator();
    if (status != 0) {
        return status;
    }
    return test_terminal_line_endings();
}
