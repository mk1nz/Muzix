#include "rtc_ds1302.h"
#include "z80_io.h"

/*
 * DS1302 driver.
 *
 * The chip is three-wire on one port, so every register access is a command
 * byte and then 0 or more data bytes, all bit-banged, least significant bit
 * first - the chip shifts each bit in from the top, so that is the order the
 * master sends.
 *
 * The port byte is held in a static rather than passed in a register: how
 * SDCC assigns a small integer argument is a property of the calling convention
 * rather than something to rely on, and getting it wrong moves the clock line
 * when the data line was meant to move.  Loading it by name inside the __asm
 * block is unambiguous.
 *
 * This used to be six small helpers - a bit out, a bit in, a command, a send,
 * a receive, and the chip-enable edges around them - and the kernel could not
 * afford them: each one is a call frame, and the kernel has a hard limit of a
 * few hundred spare bytes before the C stack runs into _DATA.  There are three
 * now: one pin change, and the two directions built on it.
 *
 * THE PIN SEQUENCE HERE IS FUZIX'S, BIT FOR BIT.
 *
 * FUZIX drives this board and this chip in production. Its board primitives
 * and transfer sequence informed this implementation; comments below explain
 * the relevant hardware behavior and deviations.
 *
 * The previous version of this driver instead computed a whole fresh port byte
 * for each pin change, and on the read path it wrote CE|RELEASE with the data
 * bit low - which is not the byte FUZIX writes, because FUZIX's read-modify-
 * write leaves whatever DATA the last command bit set, and the last bit of the
 * clock-burst read command 0xBF is a 1.  So the master held its own data line
 * low on the line the chip was driving.  That is invisible in the emulator,
 * which models the release as a direction and never looks at bit 7 again, and
 * it is exactly the kind of thing agreement in the emulator cannot rule out.
 * The shadow below is what makes the sequence expressible at all: without a
 * record of the port, "leave data as it was" is not something a function can
 * ask for.
 *
 * No bank registers are touched here and nothing is allocated.
 */

/* Our own copy of the port register.
 *
 * FUZIX calls this `rtc_shadow` and gives the reason on the line above it:
 * "we can't read back the latch contents, so we must keep a copy"
 * (ds1302-n8vem.s:33).  The port is write-only for everything the driver owns -
 * an `in` returns the chip's data bit and whatever the hardware happens to put
 * on the other seven - so any bit this driver does not itself track is lost
 * across the next `out`, and a byte recomputed from scratch each time has no
 * way to carry the ones it did not think about.
 *
 * Zero at reset, which is what FUZIX's is: the first pin change is the one
 * that raises chip enable, and FUZIX's first access does the same. */
static uint8_t muzix_zeta_rtc_shadow;

/* What the last `in` returned, held so the bit loop can test it without a
 * second port read.  A cell of its own rather than a reuse of the shadow:
 * reading the port must not overwrite what the driver believes it wrote. */
static uint8_t muzix_zeta_rtc_in_byte;

/* The clock-burst read command, in ROM rather than on the stack: it is a
 * constant, and the kernel has no bytes to spare. */
static const uint8_t muzix_zeta_rtc_read_cmd = MUZIX_DS1302_BURST_READ;

/* The port number again, spelled for the assembler.
 *
 * MUZIX_DS1302_PORT carries a `u` suffix, which is right for C and wrong here:
 * SDCC expands the macro into the __asm block textually, and sdasz80 has no `u`
 * on an immediate, so `out (#0x70u), a` fails to assemble. */
#define MUZIX_DS1302_PORT_ASM 0x70

/*
 * The two names for bit 5.
 *
 * rtc_ds1302.h calls it MUZIX_DS1302_RD, "the read direction", which is how the
 * chip's datasheet describes it.  On this board the bit is a control for the
 * data pin's driver and 1 releases it, which is FUZIX's PIN_DATA_HIZ
 * (ds1302-n8vem.s:26) and the reason its set_driven() routine `or`s it onto a
 * shadow rather than storing a direction.  Same bit, same polarity, and the
 * value 0x20 in both trees - so these are not new constants, only the two
 * ends of the same control, named for which end is being asked for.
 */
#define MUZIX_DS1302_DRIVE   0u
#define MUZIX_DS1302_RELEASE 1u

#ifndef __SDCC
/* The host seam, and the only thing in this file that differs between a Z80 and
 * a host build.
 *
 * Two `out (#0x70),a` and `in a,(#0x70)` are the whole of this driver's contact
 * with the board, and they sit in the middle of the logic under test: the hour
 * conversion is reached through the bit-banged burst read, so a host test can
 * only drive it by supplying the wire.  Rather than lift the conversion out into
 * something a test could call - which is how a second copy of it appears, and a
 * second copy passes on exactly the bugs a first copy was written for - the two
 * instructions become calls to the port primitives the rest of the tree already
 * uses, and test_host/z80_io_host.c decides where they go.
 *
 * z80_outb() is the tree's own port write and is already stubbed for the host.
 * z80_inb() is declared here rather than in z80_io.h because that header carries
 * no z80_inb() on purpose: the bank select registers are write-only and must
 * never be read back.  The DS1302's data line at $70 is not one of them - it is
 * the one port whose bit 0 a master has to sample - so this driver needs a read
 * and the Z80 build gets it from its own `in`.  Nothing above reaches either
 * name when __SDCC is defined, so there is no undefined symbol in the ROM. */
uint8_t z80_inb(uint8_t port);
#endif

/*
 * One pin change: read-modify-write the shadow, then put the whole byte out.
 *
 * This is FUZIX's setpin (ds1302-n8vem.s:64-74), which loads the shadow, clears
 * the pin's bit, sets it again if the argument says so, writes the register and
 * saves the shadow.  FUZIX takes the mask and the set-bit in a register pair
 * assembled at each call site; a mask and a flag are used here instead, because
 * how SDCC passes two small integers is a property of the calling convention
 * rather than something to rely on.
 */
static void muzix_zeta_rtc_pin(uint8_t mask, uint8_t set)
{
    uint8_t value = muzix_zeta_rtc_shadow;

    value = (uint8_t)(set ? (value | mask) : (value & (uint8_t)~mask));
    muzix_zeta_rtc_shadow = value;
#ifdef __SDCC
    __asm
        ld      a, (_muzix_zeta_rtc_shadow)
        out     (#MUZIX_DS1302_PORT_ASM), a
    __endasm;
#else
    z80_outb(MUZIX_DS1302_PORT_ASM, muzix_zeta_rtc_shadow);
#endif
}

/*
 * Clock `count` bytes out, least significant bit first, opening a chip-enable
 * window if one is not already open.
 *
 * This adapts FUZIX's byte-send sequence plus the chip-enable assertion its
 * callers put in front of it. FUZIX has that pair
 * written out twice because it calls send_byte once per byte; here one window
 * has to cover a command byte AND the value that belongs to it, so the window
 * is opened here instead, and a caller sends both bytes in one call.  See
 * muzix_zeta_rtc_set below for why that matters.
 *
 * Chip enable is left raised: a transfer ends with the clock low and chip
 * enable low, and both callers do that - a read through muzix_zeta_rtc_recv,
 * a write through the loop in muzix_zeta_rtc_set.
 *
 * This used to close the window as well as open it, which put the close in the
 * wrong place for a read: muzix_zeta_rtc_get() calls this to clock out its
 * command byte and then calls muzix_zeta_rtc_recv() for the data, so the two
 * halves of one read had the bus parked between them.  The host model reports
 * that as seven bytes read back as zero - `Sun Jan 0 00:00:00 2000` - and
 * three tests turned red when it was done.  They were right.
 *
 * Keeping one copy of the bit loop also matters - the kernel has tens of bytes
 * of headroom below _DATA's page, not hundreds, and a second copy of a loop
 * this size is about fifty of them.
 */
static void muzix_zeta_rtc_send(const uint8_t *src, uint8_t count)
{
    uint8_t i;
    uint8_t j;

    /* Chip enable low then high, so that a transfer is always opened by a
     * rising edge even when the previous one left the line high.  The chip
     * needs that to be an edge: it starts a command on the transition. */
    muzix_zeta_rtc_pin(MUZIX_DS1302_CE, 0u);
    muzix_zeta_rtc_pin(MUZIX_DS1302_CE, 1u);

    /* ds1302_set_driven(true), ds1302.c:28: the master owns the data line for
     * the command byte and for anything it writes.  This is a read-modify-write
     * against the shadow, so whatever the shadow already held for the other
     * pins - clock still low - survives. */
    muzix_zeta_rtc_pin(MUZIX_DS1302_RD, MUZIX_DS1302_DRIVE);

    for (i = 0; i < count; i++) {
        for (j = 0; j < 8u; j++) {
            /* ds1302_set_clk(false), then ds1302_set_data(bit), then
             * ds1302_set_clk(true) - ds1302.c:32-36, in that order, because
             * "for data input to the chip the data must be valid on the rising
             * edge of the clock".  Least significant bit first, the chip
             * shifting each bit in from the top. */
            muzix_zeta_rtc_pin(MUZIX_DS1302_CLK, 0u);
            muzix_zeta_rtc_pin(MUZIX_DS1302_DATA,
                               (uint8_t)((src[i] >> j) & 0x01u));
            muzix_zeta_rtc_pin(MUZIX_DS1302_CLK, 1u);
        }
    }
}

/*
 * Read `count` data bytes back, after the command byte has been clocked out and
 * with chip enable still raised.
 *
 * This adapts FUZIX's byte-receive sequence. The order is the other way round
 * from a write: the chip has the bit on the line
 * before the clock edge that shifts the next one in, so the clock is dropped,
 * the bit is sampled, and only then is the clock raised again.
 */
static void muzix_zeta_rtc_recv(uint8_t *dst, uint8_t count)
{
    uint8_t i;
    uint8_t j;
    uint8_t value;
    /* ds1302_set_driven(false), ds1302.c:45: the master lets go of the data
     * line and the chip owns it for the rest of the transfer.
     *
     * FUZIX does this against a shadow that still carries the last data bit
     * the command set - and the last bit clocked out of the clock-burst read
     * command 0xBF is a 1, because the command goes out least significant bit
     * first.  So FUZIX leaves the data line high as well as released, and the
     * port during the read is CE|CLK|DATA|RELEASE.  This driver used to write a
     * freshly computed CE|RELEASE with the data bit low, holding its own data
     * line low on the line the chip was driving; that was the whole difference
     * from FUZIX, and the shadow is what makes it expressible, because
     * "leave data as it was" cannot be asked of a function that is handed the
     * byte to write.
     *
     * The release also happens with the clock still high, which is where FUZIX
     * does it: the chip's output pin is high-impedance on the rising edge, so
     * that is the one moment the master is not fighting it. */
    muzix_zeta_rtc_pin(MUZIX_DS1302_RD, MUZIX_DS1302_RELEASE);

    for (i = 0; i < count; i++) {
        value = 0u;
        for (j = 0; j < 8u; j++) {
            /* ds1302_set_clk(false); ds1302_get_data(); ds1302_set_clk(true)
             * - ds1302.c:51-56.  The byte is assembled exactly as FUZIX
             * assembles it: shift what has been collected down one and put the
             * bit just read in at the top.  The bits arrive least significant
             * first, so the first one read ends up in bit 7.
             *
             * The read of the chip's bit is spelled out here rather than
             * called.  A call in this loop has to preserve the accumulator and
             * the four loop registers across it, and the kernel has about
             * fifteen bytes of _CODE left below _DATA's page: measured, the
             * function plus its call and those saves came to 18.  This is the
             * same trade the six-helpers version of this file made, for the
             * same reason, and there is no duplication to avoid - the read
             * happens once per bit and only here.
             *
             * The whole port byte lands in muzix_zeta_rtc_in_byte and only bit
             * 0 is of interest.  That is FUZIX's PIN_DATA_IN and the bit its
             * get_data masks (ds1302-n8vem.s:37-41). */
            muzix_zeta_rtc_pin(MUZIX_DS1302_CLK, 0u);
            value = (uint8_t)(value >> 1);
#ifdef __SDCC
            __asm
                in      a, (#MUZIX_DS1302_PORT_ASM)
                ld      (_muzix_zeta_rtc_in_byte), a
            __endasm;
#else
            muzix_zeta_rtc_in_byte = z80_inb(MUZIX_DS1302_PORT_ASM);
#endif
            if (muzix_zeta_rtc_in_byte & MUZIX_DS1302_IN) {
                value = (uint8_t)(value | 0x80u);
            }
            muzix_zeta_rtc_pin(MUZIX_DS1302_CLK, 1u);
        }
        dst[i] = value;
    }

    /* ds1302_set_clk(false) then ds1302_set_ce(false), ds1302.c:90-91: the
     * clock is parked low and chip enable ends the transaction.  Dropping
     * enable is also what lets the next transfer's rising edge be seen as one,
     * which is why muzix_zeta_rtc_send starts by dropping it too rather than
     * assuming it found the port idle. */
    muzix_zeta_rtc_pin(MUZIX_DS1302_CLK, 0u);
    muzix_zeta_rtc_pin(MUZIX_DS1302_CE, 0u);
}

/*
 * BCD to binary and back.
 *
 * Neither uses `/` or `%`.  The kernel links no C library, so a divide or a
 * modulo on integers compiles to a call to __divuchar, __divsint or __moduchar,
 * none of which are there and the link fails with them undefined - measured, by
 * adding the operators and reading the linker's complaints.
 */
static uint8_t muzix_zeta_rtc_bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10u) + (value & 0x0Fu));
}

static uint8_t muzix_zeta_rtc_bin_to_bcd(uint8_t value)
{
    uint8_t tens = 0u;

    while (value >= 10u) {
        value = (uint8_t)(value - 10u);
        tens++;
    }
    return (uint8_t)((tens << 4) | value);
}

int muzix_zeta_rtc_get(muzix_rtc_time_t *time)
{
    uint8_t *b = (uint8_t *)time;
    uint8_t i;

    /* No NULL check on `time`.  The only caller is the syscall layer, which has
     * already refused a null user pointer, and the kernel cannot spare the
     * branch.  A new caller outside the kernel has to check its own. */

    /* One burst transfer brings all seven registers out in chip order: seconds,
     * minutes, hours, date, month, day of week, year.  0xBF is the chip's
     * clock-burst read, and unlike the burst WRITE it is complete at seven
     * bytes - the read stops when the master stops clocking, and it reads the
     * registers seconds to year rather than "all of them".  This is FUZIX's
     * read, 0x81 | 0x3E (ds1302.c:83), and FUZIX reads the clock on every tick.
     *
     * muzix_zeta_rtc_send() opens the chip-enable window and
     * muzix_zeta_rtc_recv() closes it at the end of the last byte, which is
     * FUZIX's shape (ds1302.c:82 and ds1302.c:90-91): one window around the
     * command and the data that belongs to it.
     *
     * The seven bytes land straight in the caller's struct and stay there,
     * unconverted, unless the answer is accepted below.  That is the point of
     * it: a read that is refused has still been told something, and "the chip
     * sent ff ff ff" says no chip, an hour byte of 18 says the board is in
     * 12-hour mode, and anything else is a wire that did not do what was asked
     * of it.  Before this the only report was that it had failed, which is
     * exactly the case a person debugging on real hardware cannot work from -
     * see the failure path in the GETRTC handler in kernel/syscalls.c, which
     * hands the struct over either way for this reason. */
    muzix_zeta_rtc_send(&muzix_zeta_rtc_read_cmd, 1u);
    muzix_zeta_rtc_recv(b, 7u);

    /* The chip's register order and the struct's differ in the last two
     * positions only - the chip counts seconds, minutes, hours, date, month,
     * day of week, year, the struct has year before weekday - so they are
     * swapped here and the seven values then decode in one loop.  The swap is
     * done before the checks rather than after, so a refused read reports the
     * seven fields in the struct's order instead of the chip's; b[0] to b[2]
     * are the same either way, and those are the ones a person reads.
     *
     * This used to be seven separate `bcd_to_bin` calls, one per field.  Each
     * cost a call, a save and a restore of the two registers the struct pointer
     * is being carried in, and its own address arithmetic: 179 bytes of function
     * for what is one loop over seven adjacent bytes, 125 of them once the loop
     * is in.  The checks below came out of the fifty-four the difference paid
     * for; see rtc_ds1302.h for what is still left and what is not. */
    i = b[5];
    b[5] = b[6];
    b[6] = i;

    /* No chip on the port leaves the data line high, so every byte of the
     * transfer comes back 0xFF.  Seconds is the field to look at, and the
     * limit is the BCD literal 0x59, not the decimal 59: this test runs on the
     * byte as the chip sent it rather than on a decoded copy, and 0x42 - the
     * chip's forty-two seconds - is 66 as a number.  Measured against a
     * decimal 59 that refuses every reading from BCD 0x40 upwards, which is a
     * clock that reports "no chip" for the whole of the fortieth through the
     * fifty-ninth second - twenty seconds of every minute - and works for the
     * rest.  It is a failure that looks like a missing part.
     *
     * 0x59 is the right limit for its own sake as well: it is the largest BCD
     * second there is, so everything above it is either not BCD or is the
     * all-ones signature.  Bit 7 is the chip's own clock-halt flag and is
     * masked off, so a halted clock is read rather than refused - and the mask
     * is of the copy being tested, not of the caller's, so the caller still
     * learns that it was halted. */
    if ((b[0] & 0x7Fu) > 0x59u) {
        return -1;
    }

    /* The hour register carries its own format in bit 6, and the board this was
     * last run on had it clear - a 12-hour clock, which this driver used to
     * refuse rather than decode.  That was the right call in the abstract (in
     * 12-hour mode bit 5 is the PM flag, not the tens of the hour, so 1 PM
     * reads as 21 and 12 AM as 12) and the wrong one in practice: refusing cost
     * date, uptime and top the clock entirely, on hardware whose clock was
     * answering correctly all along.
     *
     * So decode it.  When the chip SETS bit 7: bit 5 is PM, bits 4-0 are the
     * hour, 1-12 in BCD, and 12 means midnight or noon rather than 12 o'clock.
     * Bit 7 is the mode bit itself and is masked off with the rest.
     *
     * The test was on bit 6, which the datasheet marks as a reserved 0 and which
     * no board sets, so every real board took this branch and every real board
     * reported itself as a 12-hour clock.  Nothing broke, because the conversion
     * below is skipped on a board declared 24-hour and both branches ended on the
     * same `& 0x3F`; but the mode bit was never consulted, so a board that really
     * WAS in 12-hour mode could not be detected either. */
    if ((b[2] & MUZIX_DS1302_HOUR_12_24) != 0u) {
        uint8_t v  = (uint8_t)(b[2] & 0x1Fu);
        uint8_t pm = (uint8_t)((b[2] & MUZIX_DS1302_HOUR_PM) != 0u);

        /* Is v a 12-hour hour at all?  They are 01-09, 10, 11 and 12 in BCD.
         * The set has to be named: these are BCD bytes, so 0x10 is 16 and a
         * plain `v <= 0x12` would admit 0x0A, which is not a time.
         *
         * Only consulted when the declared mode is 12-hour.  For 20:00 to 23:59
         * the byte cannot be told apart from 12 PM to 3 PM - 0x21 is both 21:00
         * and 1 PM - so with a 24-hour board the range test below would
         * misread every hour from 20 to 23 by twelve, which is what it did. */
        if (!MUZIX_RTC_HOUR_24_HOURS &&
            ((uint8_t)(v - 0x01u) <= 0x08u ||
             (uint8_t)(v - 0x10u) <= 0x02u)) {
            uint8_t hour = muzix_zeta_rtc_bcd_to_bin(v);

            if (pm) {
                hour = (uint8_t)(hour + 12u);
            }
            if (hour > 23u) {
                return -1;
            }
            /* Through binary, because adding twelve in BCD is not a single
             * addition: 0x09 + 0x12 is 0x1B, and 9 PM is 0x21.  Doing it in
             * binary and converting back is the only form that is right for
             * every hour from 4 PM to 9 PM, which is the whole range where the
             * tens moves from 1 to 2. */
            b[2] = muzix_zeta_rtc_bin_to_bcd(hour);
            goto hour_converted;
        }

        /* Not a 12-hour hour, so this is a 24-hour clock whose mode bit was
         * never set, and bit 5 is the tens of the hour: BCD "20" is 0x20 and
         * "10" is 0x10.  This board returns exactly that - 0x20 for 20:49 and
         * 0x10 for 10:34 - and reading 0x20 as a PM flag over a zero hour is
         * what made a clock that was answering every time look absent. */
        {
            uint8_t h24 = (uint8_t)(b[2] & 0x3Fu);
            if (h24 > 0x23u) {
                return -1;
            }
            b[2] = h24;
        }
    }

    /* Mode bit set: strip it and let the BCD loop below do the rest. */
    b[2] = (uint8_t)(b[2] & 0x3Fu);
hour_converted:

    /* Bit 7 of seconds is the chip's clock-halt flag, not part of the value, and
     * it is cleared here rather than in the two branches above so that every
     * accepted reading gets it, whichever way the hour was decoded. */
    b[0] = (uint8_t)(b[0] & 0x7Fu);
    for (i = 0; i < 7u; i++) {
        b[i] = muzix_zeta_rtc_bcd_to_bin(b[i]);
    }
    return 0;
}

int muzix_zeta_rtc_set(muzix_rtc_time_t *time)
{
    /* The seven values in CHIP order, ready to be encoded one register at a
     * time: seconds, minutes, hours, date, month, day of week, year.  That is
     * not the struct's order - the struct puts the weekday last - so the bytes
     * are placed rather than walked, and the command for each is derived from
     * the index.
     *
     * One register per transfer, and NOT one eight-byte frame behind command
     * 0xBE - that is what `date MMDDhhmmYYYY` ran into on the board.  See
     * MUZIX_DS1302_BURST_WRITE in rtc_ds1302.h for the datasheet's own
     * statement of what a burst write owes.  Note what that fix and the one below
     * have in common: both are about a register the person counting a date and
     * time does not count, and on this board the first was found by the board
     * and the second by reading FUZIX.
     *
     * Write protect IS cleared, and it is cleared FIRST rather than eighth.
     * That is the second thing that has to be true before a set takes at all,
     * and the reason is FUZIX on this same board.
     *
     * Bit 7 of the write-protect register rejects every write to the clock
     * registers and to RAM.  FUZIX treats it as a real protection rather than a
     * hint: rtc_nvwrite() reads 0x8F, clears the bit if it is set, does its
     * write and puts the bit back, and FUZIX's one clock write opens with
     * ds1302_write_register(0x8E, 0x00) for exactly
     * this reason (ds1302_discard.c:25-27).
     *
     * This driver used to leave the register alone, on the grounds that a DS1302
     * ships with the bit clear and nothing else here writes it.  On the board
     * that was wrong, and the way it was wrong is the reason the emulator could
     * not have caught it: a protected chip reads perfectly forever and is written
     * not at all, so `date` reported "the clock did not take the new time" and
     * printed the time the chip already had, and every host test passed because
     * every host test set a chip whose protection was clear.  The model stored
     * the bit and never consulted it; it does now, and test_rtc_ds1302.c arms it
     * before a set so that a driver which does not clear it fails there.
     *
     * FIRST, though the chip numbers it eighth, and the order is the whole
     * content of the fix: the bit is a blanket over the other seven, so a clear
     * that arrives after them clears nothing.  Writing it as "the eighth
     * register, where the chip puts it" is the obvious way to do this and it does
     * not work - which is what the new host case said when it was first written
     * that way, and the reason it is worth having.
     *
     * It is left CLEAR afterwards, deliberately.  Restoring it, as FUZIX's
     * nvwrite does, is right for a caller's scratch register and wrong for a
     * clock: a system that ships a `date` command wants a clock it can set, and
     * re-arming the protection would make every set a one-shot that the next one
     * cannot get past.  So a second set has to work as well as the first, and
     * the host case asks for exactly that.
     *
     * Writing seconds is what starts the chip's oscillator if it was stopped -
     * the clock-halt bit is bit 7 of the seconds register and a write clears it -
     * and seconds is written first here, as it is in the chip's own register
     * order, so the clock is running by the time the last register is stored.
     *
     * There is no range check on the fields here, and that is a measured
     * decision rather than an oversight.  _CODE has tens of bytes of headroom
     * below _DATA's page, not hundreds, and a range check measured 69.  An
     * out-of-range value would not fail on the chip: bin_to_bcd turns 60
     * seconds into 0x60 and the clock counts on through a time that was never
     * one, so the check is enforced one level up, in set_rtc() in lib/libc.c,
     * which is where a program is and where there is room for it.  See
     * rtc_ds1302.h. */
    uint8_t values[8];
    uint8_t pair[2];
    uint8_t i;

    /* Index 0 is write protect, cleared; the seven time fields follow in the
     * chip's own order.  The struct's order is not this one either - the struct
     * puts the weekday last - so the bytes are placed rather than walked. */
    values[0] = 0x00u;      /* write protect, off - see the note above */
    values[1] = muzix_zeta_rtc_bin_to_bcd(time->second);
    values[2] = muzix_zeta_rtc_bin_to_bcd(time->minute);
    /* Bit 7 of the hours register is the 12/24 select and it is INVERTED: high
     * selects 12-hour.  24-hour mode is therefore a ZERO there, so nothing is
     * ORed in, and bit 6 is reserved and also left at zero.  The whole hour
     * register is the BCD value.  See MUZIX_DS1302_HOUR_12_24 for what this used
     * to get wrong. */
    values[3] = muzix_zeta_rtc_bin_to_bcd(time->hour);
    values[4] = muzix_zeta_rtc_bin_to_bcd(time->day);
    values[5] = muzix_zeta_rtc_bin_to_bcd(time->month);
    /* There is no "leave it alone" for this register now, and there was not one
     * before either: a caller that did not name a weekday (zero) gets Sunday,
     * the chip's first day.  Stated here because it is the one field of a set
     * that can come back different from what was asked. */
    values[6] = muzix_zeta_rtc_bin_to_bcd(time->weekday ? time->weekday : 1u);
    values[7] = muzix_zeta_rtc_bin_to_bcd(time->year);

    /* Eight transfers of two bytes each, which is FUZIX's ds1302_write_register
     * (ds1302.c:62-69) done to the eight clock registers in a loop.
     *
     * The registers are NOT walked in the chip's own order, and that is the fix.
     * Write protect is register 7 and the bit in it is a blanket over the other
     * seven, so it is written FIRST and the time registers after: the order is
     * 7, 0, 1, 2, 3, 4, 5, 6, which is (i + 7) & 7.  Walking 0 to 7 is the
     * obvious thing to write and it does not work - every time register is
     * refused, and only then is the protection lifted, which is the same as never
     * lifting it.  The host case in test_rtc_ds1302.c is the version of this that
     * fails, and it is why the order is arithmetic rather than a table of register
     * numbers: a table is 8 bytes of _DATA, and _DATA is the scarcer of the two
     * here - it is what the 1024-byte stack floor is measured against.
     *
     * The command and the value go out in ONE send() call on purpose: send()
     * opens the chip-enable window, so two calls would drop and raise chip
     * enable between the command byte and the value that belongs with it, and
     * the chip would take the command as a transfer of its own and the value as
     * the start of another.  The two bytes have to be adjacent in one buffer for
     * that, which is the whole reason for `pair` and not two send() calls. */
    for (i = 0u; i < 8u; i++) {
        pair[0] = MUZIX_DS1302_CMD_CLOCK_WRITE(
            (uint8_t)((i + MUZIX_DS1302_REG_WP) & 0x07u));
        pair[1] = values[i];
        muzix_zeta_rtc_send(pair, 2u);

        /* End the transfer: clock low, then chip enable low.  That is FUZIX's
         * read (ds1302.c:90-91) rather than its write (ds1302.c:67-68, chip
         * enable first); both end it, and this one order is used for both
         * directions here so that no transfer ends with the clock running. */
        muzix_zeta_rtc_pin(MUZIX_DS1302_CLK, 0u);
        muzix_zeta_rtc_pin(MUZIX_DS1302_CE, 0u);
    }

    /* Read the chip back over the caller's struct, so that what the caller
     * holds afterwards is what the chip actually says and not what it asked
     * for.  Comparing the two field by field is what this did first; it cost
     * about 150 bytes and the comparison belongs in the caller anyway, which is
     * the one that knows what it was trying to do.  `date` does exactly that:
     * it saves what it asked for, and compares.  A set that went nowhere
     * therefore shows the old time and is reported, not swallowed.
     *
     * There is no chip-enable drop between the last write and this read, and
     * there does not need to be: the loop above ends every transfer with the
     * clock low and chip enable low, so this read opens a fresh window with a
     * rising edge. */
    (void)muzix_zeta_rtc_get(time);
    return 0;
}
