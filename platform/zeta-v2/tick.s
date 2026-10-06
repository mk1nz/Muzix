.module muzix_tick

.globl _muzix_tick_init
.globl _muzix_tick_now
.globl _muzix_tick_isr
.globl _muzix_tick_expired
.globl _muzix_tick_add_process_time
.globl _muzix_tick_probe
.globl _muzix_tick_stamp_store
.globl _muzix_uart_rx_isr
.globl _muzix_tick_cycles
.globl _muzix_tick_calibrate_ctc3
.globl _muzix_tick_calibration_status
.globl _muzix_tick_calibration_elapsed_ticks
.globl _muzix_ctc_ch0
.globl _muzix_ctc_ch1
.globl _muzix_uart_rx_count
.globl _muzix_uart_rx_pending
; The counter itself, read inline by the bounded wait in uart_io.s and through
; muzix_tick_now() by everything in C.
.globl _muzix_tick_count

; ---------------------------------------------------------------- ports ----
;
; The Zeta SBC V2 has a Zilog CTC at $20-$23, one port per channel, the same
; wiring also used by FUZIX.
; Channel 1 is the tick.  Channel 0 carries the vector base (below) and channel 2
; is the UART's interrupt controller.
;
; THE BOARD WIRES THE CTC AS A VECTORED INTERRUPT CONTROLLER, WHICH MEANS IM2.
; From the Zeta SBC V2 README: "Zeta SBC V2 uses CTC as the source of vectored
; interrupts.  For interrupts to function properly Z80 needs to be set to
; interrupt mode 2."  Channel 2's CLK/TG input is the UART's interrupt output,
; so a UART RX interrupt becomes a CTC channel-2 terminal count, which the
; daisy chain turns into a vector byte.  FUZIX's port for this board's
; family does the same thing -
; FUZIX's working platform code sets
; `ld i,a` / `im 2` and installs a real table at 0x80 - so this follows a
; working scheme on the same hardware and the same author's notes.
MUZIX_CTC_CH0    .equ  0x20
MUZIX_CTC_CH1    .equ  0x21
MUZIX_CTC_CH2    .equ  0x22
MUZIX_CTC_CH3    .equ  0x23

; The UART's receive buffer register.  Same port as the transmit holding
; register - $68 is DLL under DLAB and RBR without it (uart_io.s:37-38), and the
; divisor is written once at init with DLAB set and left alone, so by the time an
; RX interrupt can arrive this is a receive read.
MUZIX_UART_RBR   .equ  0x68

; The CTC vector byte is `VVVVV000` on channel 0, with bits 2 and 1 of the
; supplied byte set to the channel number that fired.  So one byte programmed
; on channel 0 places all four channel vectors, two bytes apart:
;
;   0x80 + 0  channel 0   unused here
;   0x82      channel 1   the tick
;   0x84      channel 2   the UART
;
; and the CPU reads the handler address at I:vector.  With I = 0x00 the table
; therefore lives at 0x0080-0x008F, one 16-bit entry per channel.
MUZIX_CTC_VECTOR_BASE .equ  0x80
MUZIX_IM2_VECTOR  .equ  0x0080
; The entry the CPU fetches for a given vector byte.
MUZIX_IM2_TICK    .equ  MUZIX_IM2_VECTOR + 2
MUZIX_IM2_UART    .equ  MUZIX_IM2_VECTOR + 4

; THE BOARD'S CTC WIRING, FROM ITS OWN DOCUMENTATION
; ----------------------------------------------------
; The Zeta SBC V2 README, "Interrupts":
;
;   "Channels 0 and 1 are chained together.  So that channel 1 can be used to
;    generate low-frequency periodic interrupts:
;      Channel's 0 CLK/TG input is connected to UART_CLK/2 (921.6 kHz)
;      Channel's 1 CLK/TG input is connected to channel's 0 ZC/TO output"
;
; So the tick is a CHAIN, and both halves of it are counters:
;
;   UART_CLK/2 = 921 600 Hz  ->  channel 0, TC 256  ->  3 600 Hz
;                                       ZC/TO
;   3 600 Hz  ->  channel 1, TC N  ->  3600/N Hz
;
; and the README gives the bytes verbatim:
;
;   "Output 01000111 to the channel 0.  Configure the channel in the counter
;    mode."
;   "Output 00000000 to the channel 0.  Set the time constant to 256, so that it
;    will divide the input clock by 256, resulting in 3.6 kHz pulses."
;   "Output 11000111 to the channel 1.  Configure the channel in the counter
;    mode and enable interrupt when counter reaches 0."
;   "Output 11110000 to the channel 1.  Set the time constant to 240, so that the
;    interrupt will be generated every 1/15 of a second."
;
; 0x47 and 0xC7 are those two control words exactly.  The README's time
; constant of 240 gives 15 Hz, which is the vendor's own example and far too
; slow for a system tick - 240 was also the constant this tree used, arrived at
; by dividing the CPU clock directly, which is not what this board does.  120 Hz
; out of 3 600 Hz is a time constant of 30.
;
; WHY D6 IS SET ON BOTH CHANNELS, WHICH IS NOT A MISTAKE
; ------------------------------------------------------
; On the Z80 CTC, D6 selects timer or counter mode. Set, the channel is a COUNTER
; clocked from its CLK/TG pin - which is the whole point here, because the clock
; it needs comes from the board, not from the CPU. Clear selects TIMER mode.
;
; D5 selects the timer prescaler (/16 or /256) and has no effect in counter mode.
; The board's documented control words 0x47 and 0xC7 therefore both select
; counter mode because their D6 bit is set.
;
; What the symptom looked like, for the record, because it read as something
; else entirely.  With channel 1 programmed and channel 0 left at its reset
; state, the tick counter moved ONCE and then never again: "IRQ: ok, 0001 ->
; 0002", and a HALT that never returned.  A counter with no clock interrupts
; once, on the CTC's own reset pulse, and then has nothing left to count - so a
; halted CPU had nothing to wake it.  Nothing about that output says "no
; prescaler", which is why it survived being looked at twice.
MUZIX_CTC_CTRL_SHUTDOWN  .equ  0x43
MUZIX_CTC_CTRL_CH0       .equ  0x47
; 0 is 256 to the CTC: the largest time constant that fits a byte.
MUZIX_CTC_TC_CH0         .equ  0x00
MUZIX_CTC_CTRL_TICK_IRQ  .equ  0xC7

; Channel 3 as a free-running CYCLE counter for the load accounting, and why the
; tick cannot do this job.
;
; The tick runs at 120 Hz. A tick is 61 440 CPU cycles in the 7.3728 MHz
; emulator profile and about 33 333 cycles on the board's 4 MHz U17 clock.
; Tick-granular accounting cannot resolve the idle-wakeup work within that
; interval; bracketing HALT with channel 3 is used instead - see
; muzix_load_note_halted().
;
; So this needs a clock, and channel 3 is the one nothing else in the tree uses.
;
; WHY CHANNEL 3 IS USABLE EVEN THOUGH THE BOARD WIRES IT TO THE PPI
; ---------------------------------------------------------------
; The README says: "Channel's 3 CLK/TG input is connected to 8255 PPI port PC3.
; This port is used as the interrupt output in PPI modes 1 and 2."  So a COUNTER on
; channel 3 would see only PPI edges, and nothing here wants that.
;
; A TIMER does not care.  D6 = 0 selects timer mode, and in timer mode the CTC
; clocks the channel from the SYSTEM CLOCK and ignores CLK/TG entirely - the pin
; is only an input in counter mode.  So this channel counts CPU cycles off the
; board's own clock while sitting on a pin wired to the PPI, and the PPI cannot
; perturb it.  That is what makes the pin's assignment irrelevant, and it is the
; reason the byte below has D6 CLEAR while the two tick channels have it set.
;
; With the installed 4 MHz oscillator it counts at 4 MHz / 256 = 15.625 kHz
; and wraps every 16.384 ms. A tick is 8.333 ms (about 130 CTC counts), so a
; halt ending at the next tick cannot wrap the counter. Startup measures the
; actual counts-per-tick ratio against the independent 120 Hz tick. The measured
; halt plus interrupt return is less than one tick, and the awake gap is only a
; few thousand CPU cycles.
;
;   bit 7  0 = no interrupt - this is a counter, not a source
;   bit 6  0 = timer (1 selects counter mode)
;   bit 5  1 = divide by 256 (0 selects divide by 16)
;   bit 4  0 = rising edge
;   bit 3  0 = timer mode
;   bit 2  1 = the next byte IS the time constant - REQUIRED, see below
;   bit 1  1 = reset on write
;   bit 0  1 = control word
; which is 0010_0111 = 0x27.
;
; BIT 2 IS NOT OPTIONAL, and leaving it clear is what made the first attempt
; useless.  A CTC channel with no time constant loaded holds 0, reloads 0, and
; reads back 0 - it does not free-run at all, it sits there.  The first version of
; this was 0x23, without bit 2, and muzix_tick_cycles() returned zero forever:
; every awake gap measured as zero, the busy fraction came out as 0.00 on a
; machine the emulator independently measured at 9.8% busy, and the only symptom
; was a load figure that was confidently, smoothly and completely wrong.
;
; The time constant written after it is 0, which the CTC reads as 256. So the
; channel counts 256, 255, ... 1, 0 and reloads: a free-running 8-bit
; down-counter that advances one count per 256 CPU cycles and wraps every
; 16.384 ms at the board's 4 MHz clock. 256 is also the
; largest value that fits an 8-bit register, which is why reading it costs a
; single `in` and needs no 16-bit assembly.
MUZIX_CTC_CTRL_CYCLE      .equ  0x27
; 0 is 256 to the CTC, and is the largest time constant that fits the register.
MUZIX_CTC_CYCLE_TC        .equ  0
; Cycles per count.  The prescaler is what divides by 256; this is that number,
; and the load arithmetic divides by it in units of counts, never converting to
; cycles - see kernel/load.c.
MUZIX_CTC_CYCLE_DIV       .equ  256

; Channel 2 as the UART's interrupt controller: counter mode, interrupt enabled,
; load the time constant next.
;   bit 7  1 = interrupt enabled
;   bit 6  1 = counter mode
;   bit 5  0 = /16 prescaler selection (unused in counter mode)
;   bit 4  1 = rising edge on CLK/TG
;   bit 3  0 = counter
;   bit 2  1 = the next byte is the time constant
;   bit 1  1 = stay enabled
;   bit 0  1 = control word
; which is 1101_0111 = 0xD7.  FUZIX uses this same setting for the UART interrupt
; input on the Zeta V2 board.
;
; The time constant is 1, so the rising edge on CLK/TG - the UART's asserted
; interrupt output - fires an interrupt.  That
; is the whole point of the channel: it turns "the UART wants attention" into an
; edge the CTC can vector, which is what makes a per-keystroke wake possible.
;
; Bit 6 must remain set so this is COUNTER mode and follows the UART edge.
; Clearing it selects TIMER mode and disconnects the interrupt from the UART.
MUZIX_CTC_CTRL_UART_IRQ  .equ  0xD7
; One edge, i.e. one received byte.
MUZIX_CTC_UART_TC        .equ  1

; Channel 1 counts the 3.6 kHz output of channel 0. With TC 30 the chain is
; 921 600 / 256 / 30 = 120 Hz. The UART clock driving channel 0 is independent
; of the CPU oscillator, so the interrupt rate is the same with the board's
; 4 MHz U17 and with the emulator's 7.3728 MHz CPU profile. In that emulator
; profile only, the equivalent interval is 61 440 CPU cycles. At 120 Hz the
; 16-bit software tick counter wraps every 546.13 seconds; deadlines therefore
; use wrap-safe unsigned comparisons (tick.h).
MUZIX_CTC_TC_TICK .equ  30

; IM1 lives at 0038H.  This is the same trick kernel_entry.s uses for the
; syscall trap at 0030H: the CPU pushes the return address and jumps to 0038H,
; so a three-byte JP planted there is the whole vector.
;
; THE SYSTEM IS IN IM2, AND 0038 IS KEPT ANYWAY.
; ------------------------------------------------
; IM2 is now the mode, because the board uses the CTC as a vectored interrupt
; controller and needs it (see the port comment above): the UART has its own
; vector, so the tick and the console need to be told apart by the vector byte
; rather than by sharing one address and re-testing every source inside a single
; handler.
;
; 0038H is still planted with a JP to the tick handler, and it costs three bytes
; that are written once.  It is not dead: IM1 is what the ROM boot stub leaves
; behind, so if anything ever re-arms interrupts before muzix_tick_init has run
; `im 2`, a tick lands on the tick handler rather than on whatever byte happened
; to be at 0x38.  It is a floor under the one mode in which the old code worked,
; not a second code path.
;
; WHERE THE IM2 TABLE LIVES, AND WHY WINDOW 0 IS SAFE
; --------------------------------------------------
; The IM2 acknowledge reads a 16-bit handler address out of memory at
; I:vector, so the table has to be readable at that address under EVERY map -
; including the maps a user process installs.  That is a different requirement
; from IM1, whose 0x38 is only ever a jump target and never a data read.
;
; The answer is that window 0 is always kernel.  mm/mproc_model.c:131 builds a
; process map as {KERNEL_BANK_0, KERNEL_BANK_1, KERNEL_BANK_2, KERNEL_BANK_3} and
; only pages[1] and pages[2] are ever replaced (mproc_model.c:157-162, and
; exec_loader.c:244 for the exec path): the stack goes in window 1, the text in
; window 2, and windows 0 and 3 stay kernel pages 0x20 and 0x23.  So anything
; below 0x4000 is reachable whatever process is running - which is the same fact
; tools/check_window0.rb enforces from the link map, and the reason the existing
; tick handler at 0x011D has always been safe.
;
; 0x0080 is inside window 0, above the syscall trap at 0x0030 and the IM1 vector
; at 0x0038, and below _CODE, which starts at 0x0098 (Makefile, `-b _CODE=0x0098`).
; So the table cannot collide with linked code, and it survives a map change for
; the same reason the handler does.
;
; WHAT THE EMULATOR DOES WITH THIS
; --------------------------------
; The Z80 core's IM2 path reads the handler address at I:vector and CALLs it -
; emulator/z80/z80.c, case 2 in process_interrupts:
;
;     case 2:
;       z->cyc += 19;
;       call(z, rw(z, (z->i << 8) | z->int_data));
;
; and z80pack.c's CTC hook already supplies a channel-numbered vector byte
; (z80pack_ctc_tick: ctc_vector_base + (ch << 1)), so with channel 0 programmed
; to 0x80 the tick arrives as vector 0x82 and the UART as 0x84, and this table is
; what the core reads.  No emulator change is needed for the tick or for the IM2
; acknowledge itself - only for the UART to raise an interrupt at all, since the
; live CTC in z80pack.c fires channel 1 alone.
MUZIX_IM1_VECTOR .equ  0x0038

; ------------------------------------------------------------------ data ---
;
; 16 bits, and deliberately 16 rather than 8.  _DATA is in window 3, which is
; the kernel's page 0x23 in every map including every process map, so the
; handler can reach it under any map - but a one-byte counter read is not
; atomic against an interrupt landing between the read and the write, and the
; reader here is a userspace process asking for the time.  A torn one-byte read
; returns a value that is up to 255 ticks away from either the old or the new
; one, which for a 120 Hz counter is two seconds of nonsense.  16 bits plus the
; ordering below makes every read exact or one tick stale, and never wrong.
;
; Low byte first (that is the byte at this label), high byte at label+1.
.area _DATA
_muzix_tick_count:
	.dw #0

; Boot-time question: does the tick interrupt reach a CPU that is stopped in
; HALT?  Non-zero means "emit this byte on every tick", and it is only set by the
; halt probe.  See MUZIX_HALT_PROBE in kernel/kernel_main.c.
;
; This exists because "interrupts arrive" and "HALT resumes" are two different
; questions and the board has answered them differently.  The probe at boot shows a
; tick arriving while the CPU is spinning - IRQ: ok, 0001 -> 0002 - and then the
; very next instruction, z80_halt(), never returns.  Two readings fit that, and
; they are not the same fault:
;
;   the ISR runs while the CPU is stopped, and RETI does not resume it
;       -> the halt is the problem, and the console character stream below proves
;          the ISR is alive;
;   nothing appears while stopped
;       -> the interrupt does not reach a stopped CPU at all, and no amount of
;          work on the halt path will ever fix it.
;
; A character written from inside the handler is visible on the wire either way,
; so one run tells them apart.  That is worth a byte of handler time; guessing
; which one it is has already cost this project two wrong answers.
_muzix_tick_probe:
	.db #0

; ------------------------------------------------- received-bytes and flag ---
;
; A 16-bit count of UART interrupts taken, and a flag saying at least one byte is
; waiting.  Nothing else is buffered here, and the reason why is the longest
; comment in this file - see WHY IT DOES NOT READ THE BYTE above.  The 16550's own
; 16-byte FIFO is the buffer; fs/tty_device.c drains it; this only announces.
;
; The count is 16-bit and low byte first, as the tick counter is, because it is
; read from C and a torn 16-bit value would be a plausible byte count.
_uart_rx_count:
	.dw #0
; Read-and-clear: see _muzix_uart_rx_pending below.
_uart_rx_pending:
	.db #0
_ctc3_cal_target:
	.dw #0
_ctc3_cal_wraps:
	.db #0
_ctc3_cal_start:
	.db #0
_ctc3_cal_previous:
	.db #0
_ctc3_cal_current:
	.db #0
_ctc3_cal_start_tick:
	.dw #0
_muzix_tick_calibration_status:
	.db #0
_muzix_tick_calibration_elapsed_ticks:
	.dw #0

; ------------------------------------------------------------------ code ---

.area _CODE

; Program channel 1, plant the IM1 vector, and let interrupts in.
;
; `di` first even though the caller is required to arrive with interrupts
; already off: between the vector being planted and `im 1` taking effect there
; is a window in which a tick would be taken by whatever interrupt mode was in
; force, and on a machine that has been running with DI since kernel_entry.s
; that mode is whatever the ROM boot stub left behind.  The store to 0038H is
; the third byte of a JP, and the CPU cannot reach the middle of an
; instruction - so a tick that arrived between the three stores would jump into
; the operand of a half-written JP.
_muzix_tick_init:
	di

; Order is the board's wiring, not preference: channel 1's CLK/TG input IS
; channel 0's ZC/TO output, so channel 0 is programmed first and the tick cannot
; be armed before it has a clock to count.  A previous version of this file
; programmed only channel 1, which is a counter with nothing driving its trigger
; input - it never reached terminal count, never interrupted, and a HALT with no
; interrupt to wake it is why the board sat with the HALT LED lit.  The emulator
; could not see it, because z80pack_ctc_tick() fired channel 1 from a CPU cycle
; timer and never modelled the chain.

; Shut every channel down before configuring any of them.
;
; FUZIX does this first on the same author's boards
; and documents the reason:
; "We must initialize all channels of the CTC.  The documentation states that the
; initial CTC state is undefined and we don't want random interrupt surprises."
; That is a real unknown rather than a formality - this file had been leaving
; three of the four channels in whatever state power-on left them, while two of
; them carry interrupt enables in the final configuration.
;
; 0x43 is 0100_0011: interrupts off, counter, reset, control word, and bit 2 clear
; so no time constant byte follows it.  Writing it to all four takes the CTC from
; an undefined state to a defined, silent one, and every later write is then made
; from a known baseline.
	ld	a,#MUZIX_CTC_CTRL_SHUTDOWN
	out	(MUZIX_CTC_CH0),a
	out	(MUZIX_CTC_CH1),a
	out	(MUZIX_CTC_CH2),a
	out	(MUZIX_CTC_CH3),a

	ld	a,#MUZIX_CTC_CTRL_CH0
	out	(MUZIX_CTC_CH0),a
	ld	a,#MUZIX_CTC_TC_CH0
	out	(MUZIX_CTC_CH0),a

; Channel 0's vector base, and only that.  A write with bit 0 clear to channel 0
; is a vector, not a control word - that is how the CTC is told where its
; channel vectors live.  Set it before enabling any interrupting channel.
	ld	a,#MUZIX_CTC_VECTOR_BASE
	out	(MUZIX_CTC_CH0),a

; Channel 3: the free-running cycle counter the load accounting reads.  Armed
; before `im 2` so it is already counting by the time anything asks. It does
; not generate interrupts.
	ld	a,#MUZIX_CTC_CTRL_CYCLE
	out	(MUZIX_CTC_CH3),a
	ld	a,#MUZIX_CTC_CYCLE_TC
	out	(MUZIX_CTC_CH3),a

	ld	hl,#_muzix_tick_isr
	ld	a,#0xc3
	ld	(MUZIX_IM1_VECTOR),a
	ld	a,l
	ld	(MUZIX_IM1_VECTOR+1),a
	ld	a,h
	ld	(MUZIX_IM1_VECTOR+2),a

; The IM2 table: one 16-bit handler address per channel vector, at 0x80 + n*2.
; Written before `im 2` so that no interrupt can be taken through a table entry
; that is still zero - which would CALL 0x0000.
	ld	hl,#_muzix_tick_isr
	ld	(MUZIX_IM2_TICK),hl
	ld	hl,#_muzix_uart_rx_isr
	ld	(MUZIX_IM2_UART),hl

; I = 0x00 puts the table at 0x0000-0x00FF, and with the vector base at 0x80 the
; entries actually used are 0x82 and 0x84.  0x00 is the high byte, so `xor a`.
	xor	a
	ld	i,a
	im	2

; Do not arm interrupt sources until the mode, vector base, and table are all
; installed.  Channel 1 starts the 120 Hz tick chain and is deliberately last.
; This also prevents a pending UART edge during setup from being acknowledged
; through an uninitialized IM2 table.
	ld	a,#MUZIX_CTC_CTRL_UART_IRQ
	out	(MUZIX_CTC_CH2),a
	ld	a,#MUZIX_CTC_UART_TC
	out	(MUZIX_CTC_CH2),a

	ld	a,#MUZIX_CTC_CTRL_TICK_IRQ
	out	(MUZIX_CTC_CH1),a
	ld	a,#MUZIX_CTC_TC_TICK
	out	(MUZIX_CTC_CH1),a
	ei
	ret

; The IM1 handler: count a tick and get out.
;
; WHAT THIS MAY TOUCH, AND WHY THAT IS ENOUGH
; ------------------------------------------
; It reads and writes exactly two bytes of memory, and both are kernel pages in
; every map this system can install:
;
;   * the counter, in _DATA.  _DATA is linked into window 3, and a process map
;     is {0x20, stack page, text page, 0x23}, so window 3 is page 0x23 - the
;     kernel's own - whether the CPU is running the kernel or a process.  The
;     install path enforces it rather than assuming it: context_switch.c:245
;     refuses any map whose pages[3] is not the one currently installed, so a
;     process cannot be given a window 3 that is not the kernel's.
;
;   * its own instructions, in window 0.  _CODE starts at 0x0098, and this
;     module is linked immediately after muzix_kernel_entry, so the whole of
;     this handler is below 0x4000.  tools/check_window0.rb pins the module and
;     the symbol, so a link that moved it out of window 0 fails the build
;     rather than the machine.
;
;   The two windows that are NOT kernel are 1 and 2 - a process's stack and
;   text - and this handler never addresses them.  That is the entire safety
;   argument, and it holds at EVERY instant, not just at a convenient one:
;
;     write_all_banks() (platform/zeta-v2/context_switch.c:49) writes window 0
;     first and unconditionally, as 0x20, before it programs windows 1, 2 and 3
;     from the map.  So window 0 is the kernel's page from the moment the map
;     change starts, not from the moment it finishes.  A tick that lands in the
;     middle of that sequence - between the `out (0x79)` and the `out (0x7a)`,
;     with the map half-installed - finds its own code and its counter exactly
;     where they were, because the handler changes no bank register and so
;     cannot be the thing that makes the map inconsistent.  The interrupted
;     write_all_banks() resumes and finishes the map.
;
;   The stack is the other half of it.  When the kernel is running, SP is the
;   kernel stack at the top of window 3, page 0x23, so six pushes land on
;   kernel memory.  When a process is running, SP is that process's own stack
;   in window 1, its own page, and the pushes land there - which is what an
;   interrupt is supposed to do, and the RETI below takes them off again.  So
;   the pushes are on kernel memory or on the interrupted process's own stack
;   and never on a page that belongs to neither.
;
;   This is why the handler must never grow a tail call.  A call out of here
;   reaches code whose location depends on the map, and the first thing almost
;   any such routine does is touch a _DATA static - fine - or a bank register -
;   fatal, because the handler is running at an arbitrary point inside a map
;   change.  Keeping the whole handler here, in six pushes, one increment and a
;   RETI, is what makes "safe under any map" a property of the code rather than
;   of the moment it happens to run in.
;
; THE COUNTER IS 16-BIT, AND THE ORDER IS THE POINT
; -------------------------------------------------
; A single `inc (hl)` is not atomic against an interrupt, but this handler is
; the only writer, and it writes the LOW byte first and the HIGH byte second.
; The reader (muzix_tick_now, below) reads the HIGH byte first and the LOW
; byte second.  That pairing is what makes a torn read impossible:
;
;   reader reads high (old), then the tick lands: writer writes low, then high
;   - reader reads low (new).
;
; The reader has now seen old_high:new_low.  If the low byte did not carry, that
; is simply the value one tick on from the old one, and the tick has been
; counted.  If the low byte carried, old_high:0x00 is exactly one less than the
; value the tick produced, and the reader is one tick stale.  Either way it is a
; value the counter really held, never a mix of two of them.
;
; Write the pair the other way round and the same interleaving gives
; new_low:old_high - 0x00:0x02 - a value 65 534 lower than either, which reads
; as time running backwards by nine minutes.  That is the tear the ordering
; exists to prevent, and it is why the reader is not allowed to be "improved"
; into reading the low byte first for speed.
_muzix_tick_isr:
	push	af
	push	bc
	push	de
	push	hl
	push	ix
	push	iy

	ld	hl,#_muzix_tick_count
	inc	(hl)			; low byte first
	jr	nz,001$			; no carry out of it
	inc	hl
	inc	(hl)			; ... so the high byte goes last
001$:
	; One byte per tick, but only while the boot probe has asked for it.
	ld	a,(_muzix_tick_probe)
	or	a
	jr	Z,005$
	out	(0x68),a          ; UART THR, port and not memory
005$:
	pop	iy
	pop	ix
	pop	hl
	pop	de
	pop	bc
	pop	af
	ei
	reti

; The UART receive handler: count that it fired and say so.
;
; WHY IT CANNOT WAKE ANYBODY ITSELF
; ---------------------------------
; It would be tidier to call the parking machinery here and requeue the waiting
; reader directly, and that is not possible.  The argument above - that the
; handler may touch window 0 and window 3 only - is about the CPU executing at an
; arbitrary point in a map change, including the half-installed window between
; two port writes in write_all_banks().  A call from here reaches code whose
; location depends on that map, which is exactly what the tick handler is built
; not to do, so a wake done here would be a wake that can land on a page being
; banked out from under it.
;
; So the handler stops at _DATA.  It sets a flag; the halt loop in kernel_loop.c
; looks at that flag on its next pass, which it is about to take anyway because
; the halt returned, and requeues the reader from ordinary kernel context with
; the map known.  The deferral costs nothing measurable: the interrupt that
; leaves the HALT is THIS one, not the tick, so the loop reaches the flag check
; within a handful of instructions of the byte arriving.
;
; WHY IT DOES NOT READ THE BYTE, WHICH WAS TRIED AND WAS WRONG
; ---------------------------------------------------------
; Reading the receive register here and queueing the bytes in _DATA is the
; obvious design and it is wrong, because the consumer already exists and already
; reads the hardware: fs/tty_device.c goes straight to the 16550 through
; muzix_zeta_uart_recv() and muzix_zeta_uart_available(), and it is the only
; thing in the system that should.  A byte taken here has to be handed back to
; there, and the clean way to hand it back is not to take it - read the register
; in one place or the other, never both.
;
; It was done the other way first and the symptom was precise.  This handler took
; the byte, so muzix_zeta_uart_available() then reported "nothing ready" for a
; byte that had already arrived, and the canonical read returned a zero-length
; read, which the line discipline and the shell both take for end of file.  The
; shell printed its prompt and exited.
;
; There is no buffering need for the handler to serve, either.  The 16550 has a
; 16-byte receive FIFO of its own; it raises this interrupt when there is
; something in it and lowers it when the FIFO drains.  The hardware is the buffer,
; and this handler's entire remaining job is to say that it fired.
;
; WHAT IT TOUCHES
; ---------------
; _DATA, which is window 3 and is the kernel's page under every map, and nothing
; else.  Not even a port.  No tail call, for the reason above.

_muzix_uart_rx_isr:
	push	af
	push	bc
	push	de
	push	hl

	ld	hl,#_uart_rx_count
	inc	(hl)
	jr	nz,002$
	inc	hl
	inc	(hl)
002$:
	ld	a,#1
	ld	(_uart_rx_pending),a

	pop	hl
	pop	de
	pop	bc
	pop	af
	ei
	reti

; The reader, and the other half of the ordering argument above: HIGH first.
;
; Eight bytes, no branch, and no interrupt disable - a `di` here would be a
; lie about what the rest of the system does, since DI clears IFF2 as well as
; IFF1 and there is no copy of IFF2 left to restore (context_switch.c:41 says
; the same thing about the map primitives).  The ordering is what makes this
; safe, so the ordering is what this does.
;
; Declared without __sdcccall(1), and the absence is not an oversight: with no
; parameters the attribute has nothing to select - a zero-argument call passes
; nothing in HL, DE or BC either way - and the 16-bit return is in HL under
; both conventions.  The attribute is a front-end convention marker, not a
; link-time contract, so there is no way for the two to disagree here.
; uint16_t muzix_tick_now(void)  - result in DE
;
; DE, NOT HL.  This is the single most expensive mistake in the file and it is
; written down because it cost an afternoon to find: with the result left in HL,
; SDCC's own code for the caller read DE instead - `call _muzix_tick_now /
; ld hl,#600 / add hl,de` - so the deadline was computed from whatever the caller
; happened to be carrying, not from the tick.  Every bounded wait in the system
; then had a random deadline, fired almost at once, and the kernel's console
; turned into a reboot loop: the shell's read timed out, retried, and never got
; to the prompt.  AGENTS.md says a 16-bit return is in HL; that is the
; sdcccall(1) rule, and these modules are compiled plain -mz80, where the
; 16-bit return is DE.  Verified on a real call site in this tree rather than
; reasoned about: kernel_loop.c's `muzix_system_entry_current_pid()` compiles to
;
;     call    _muzix_system_entry_current_pid
;     ld      (hl), e
;     inc     hl
;     ld      (hl), d
;     bit     7, d
;
; DE for the value and D for its sign.  So DE it is, and
; tools/check_z80_call_convention.rb now compiles a probe and fails the build if
; this ever stops being true - the mistake is invisible from the assembly side
; and invisible from the C side, so nothing else in the tree can see it.
;
; The value is also left in HL, so that a caller written to the sdcccall(1) rule
; gets a correct answer rather than a plausible one.  Two registers are free
; here; one wrong convention is not.
;
; The ordering is high byte first, which is what makes a read safe against a
; tick landing in the middle of it, and it is the other half of the argument in
; the handler above.
_muzix_tick_now:
	ld	de,#_muzix_tick_count+1	; the label holds the LOW byte (see the
	ld	a,(de)			; data note), so label+1 is the high
	ld	h,a			; byte and that is the one read first.
					; DE is the pointer and HL is not:
					; `ld h,a` below would move an HL
					; pointer out from under the read
	dec	de			; ... and the low byte second
	ld	a,(de)
	ld	l,a			; HL = now
	ex	de,hl			; DE = now, which is the return register
	ret

; uint8_t muzix_tick_expired(uint16_t deadline)  - deadline in HL, result in A
;
; This is the whole deadline test in seventeen bytes, and it is a function
; rather than a macro because the alternative is ruinous here.  A macro expands
; to a call to muzix_tick_now(), a subtract, a compare and a branch at every
; call site; this kernel has five of them and measured 20 bytes of _CODE each,
; which is 100 bytes of the 64 KiB budget spent re-deriving one comparison.
; The kernel is 160 bytes from the floor its own stack headroom sets, so that
; is not a cost that can be paid.  One routine, called, is 17 bytes once and
; five bytes a site.
;
; The comparison is the wrap-safe one: `now - deadline` as a 16-bit unsigned
; subtraction carries exactly while now is behind the deadline, so a deadline
; anywhere from 0 to 32767 ticks ahead works and the counter wrapping past
; 65 536 (every 546 seconds) is a non-event.  `or a / sbc hl,de` rather than
; `cp` twice, because the low byte alone is ambiguous exactly where it matters -
; it is equal at both ends of a 256-tick window.
_muzix_tick_expired:
	push	de			; the caller's DE, which this routine
					; leaves alone - see below
	ex	de,hl			; DE = the deadline, which frees HL to
					; be the pointer it has to be: the
					; Z80 has `ld r,(hl)` but no
					; `ld l,(de)`, and the deadline
					; arrives in HL
	ld	bc,#_muzix_tick_count+1	; label+1 is the high byte.  BC is the
	ld	a,(bc)			; pointer, not HL: HL holds the high
	ld	h,a			; byte being built, and `ld h,a` would
					; move an HL pointer by 0x100
	dec	bc			; ... and the low byte second
	ld	a,(bc)
	ld	l,a			; HL = now
	or	a
	sbc	hl,de			; HL = now - deadline
	jp	m,001$			; not yet: bit 15 of the difference is
					; set.  This is `jp` and not `jr`
					; because this assembler has no sign
					; condition for `jr`, and it is the
					; SIGN and not the CARRY that says
					; which half of the wrap the deadline
					; is in - see below
	ld	a,#1
	pop	de			; the frame this routine pushed - see below
	ret
001$:
	xor	a
	pop	de			; ... and here
	ret

; THE pop de ABOVE IS NOT COSMETIC, AND NOTHING ELSE IN THE TREE CATCHES IT
;
; This routine pushes DE on entry and, until 2026-10-01, never popped it, so
; SP was left two bytes short and `ret` jumped to whatever DE happened to hold
; rather than to the caller.  It was never caught because the byte at the jump
; target happened to be harmless: muzix_tick_now() sat at 0x0137, so address
; 0x013E held 6F, and the `ret` three bytes later swallowed the call's return
; address.  The caller's own `pop hl` then popped the address that `ret` had
; just eaten, so the stack came back balanced by exactly the two bytes that had
; leaked and the caller carried on.  A cancelled accident, not correctness.
;
; Any growth in this module moves muzix_tick_now().  With five bytes added to
; the interrupt handler - five bytes of code that compute nothing - muzix_tick_now
; moved from 0x0137 to 0x013C, address 0x013E became the third byte of an
; `ld de,#...` and so the opcode E9, and the `ret` executed `jp (hl)`.  The
; machine jumped into the zero fill at 0xE800 and rebooted, repeatedly.
;
; Two bytes.  What it removes is a dependency nobody asked for: this routine
; worked or did not depending on where an unrelated function in the same module
; happened to be aligned.  There is no build-time check for a .s routine that
; pushes without popping; tools/check_window0.rb pins this module to window 0
; and cannot see a stack imbalance.

; void muzix_tick_add_process_time(uint32_t *counter, uint16_t *stamp)
;   HL = counter (32-bit, little-endian), DE = stamp
;
; *counter += (now - *stamp);  *stamp = now.
;
; This is the whole of UTIME and STIME, and it is assembly because the C version
; measured 247 bytes of _CODE against 53 here.  That is not a rounding error:
; the counters are uint32_t because struct proc_info says so, and SDCC 4.5
; turns `counter += elapsed` on a struct field into a 32-bit add through a
; stack frame - the -9(ix) and -10(ix) reloads of a 16-bit value it had just
; stored, a zero-extension of a comparison it had already done in 16 bits, and
; a `pop bc / pop hl / push hl / push bc` shuffle in the middle of it.  The
; kernel is 152 bytes above the stack headroom floor its own deepest measured
; chain sets, so those 194 bytes were the difference between fitting and
; failing the build.
;
; The tick is read here, high byte first, for the same reason as everywhere
; else in this file, and for the same reason it matters most here: this runs
; with interrupts enabled, so a tick can land between the two loads.
;
; The subtract is deliberately NOT guarded on the difference being non-zero.  If
; two calls land inside one tick the difference is zero and the 32-bit add adds
; zero, which is a wasted read-modify-write and nothing else.  The C version
; guarded it, and the guard was not free: it cost the branch, and it made the
; "no charge" case - two syscalls inside one tick, which is most of them -
; take a different path through the function than the "charge" case did.
;
; DE and BC are clobbered, which the plain -mz80 calling convention already
; permits, so nothing else is pushed.  HL is: see below.
;
; WHY THE PUSH
; -----------
; There are two pointers and the Z80 has three useful ones, so the register
; discipline here is not obvious - and getting it wrong is silent.  The first
; version of this routine read the stamp through DE into HL, then did the 32-bit
; add against whatever HL happened to point at, which was the STAMP.  So it
; added the elapsed time to the stamp and left the counter at zero: UTIME and
; STIME read 0 forever, on a kernel whose entire point was that they no longer
; did, and the C restatement of this routine that the host test exercises passed
; every case - because the bug was in which register held the address, which is
; not a thing a C model of the function can have.
;
; That is why the counter pointer is pushed and reloaded rather than kept in a
; third register, and why it is the LAST thing before the add: HL has to point
; at the counter for four consecutive stores, and nothing in between is allowed
; to move it.  BC holds the tick once it is read, DE holds the stamp, and HL is
; the pointer the tick is read through before it is popped back to the counter.
; The tick is read through HL rather than BC because BC is the register the two
; halves of the tick land in, and a pointer has to be a pair the routine does not
; need the contents of: `inc bc` would then advance the pointer by the low byte's
; value, not by one.  If you add a register to this routine, this paragraph is
; the thing that has to be true afterwards.
_muzix_tick_add_process_time:
	push	hl			; the counter pointer - see the note
	ld	hl,#_muzix_tick_count+1	; label+1 is the high byte
	ld	a,(hl)			; now high
	ld	b,a
	dec	hl			; ... and the low byte second
	ld	a,(hl)			; now low
	ld	c,a			; BC = now, C low and B high

	ld	a,(de)			; the old stamp, low byte first
	ld	l,a			; L = stamp low
	ld	a,c
	ld	(de),a			; restamp, before anything else can
	inc	de			; change the difference
	ld	a,(de)			; the old stamp's other byte
	ld	h,a			; H = stamp high
	ld	a,b
	ld	(de),a			; ... and its restamp

	ld	a,c			; elapsed = now - stamp, low half
	sub	a,l			; first and high half with the carry
	ld	e,a
	ld	a,b
	sbc	a,h
	ld	d,a			; DE = elapsed, 16-bit, little-endian

	; counter += (uint32_t)elapsed.  A 16-bit addend into a 32-bit field is
	; four byte steps: the low byte, the high byte with the carry, then the
	; top two words carrying straight through.  Little-endian throughout, which
	; is what the Z80 and every x86 behind this are and what the C compiler's
	; own store of the same field would have done.
	pop	hl			; HL = the counter, and it stays here
	ld	a,(hl)
	add	a,e
	ld	(hl),a
	inc	hl
	ld	a,(hl)
	adc	a,d
	ld	(hl),a
	inc	hl
	ld	a,(hl)
	adc	a,#0x00
	ld	(hl),a
	inc	hl
	ld	a,(hl)
	adc	a,#0x00
	ld	(hl),a
	ret

; void muzix_tick_stamp_store(uint16_t *stamp)  - HL = stamp
;
; Start a process's accounting clock at now.  Called when a slot is created, so
; its first charge covers only the time since it existed - a slot left at zero
; would be charged for every tick since boot, which is time it did not run for.
;
; It exists as a routine because the same three instructions were being spelled
; out twice in kernel/proc_table.c at 19 bytes a time, and this is 10 plus two
; calls.
_muzix_tick_stamp_store:
	ld	bc,#_muzix_tick_count
	ld	a,(bc)
	ld	(hl),a
	inc	hl
	inc	bc
	ld	a,(bc)
	ld	(hl),a
	ret

; uint16_t muzix_uart_rx_count(void)  - DE = count, HL also
;
; How many bytes have arrived since boot.  16-bit low byte first, read high-first
; for the same anti-tear ordering the tick counter uses.  DE is the return
; register for a 16-bit value in this tree - see AGENTS.md, and the measured
; result that a 16-bit return in HL is silently wrong.
;
; Only DE and HL are touched, and DE is where the answer goes.  A is free because a
; uint16_t return leaves nothing of it in the result.
_muzix_uart_rx_count:
	ld	de,#_uart_rx_count+1
	ld	a,(de)
	ld	h,a
	dec	de
	ld	a,(de)
	ld	l,a
	ex	de,hl
	ret

; uint8_t muzix_uart_rx_pending(void)  - A = flag
;
; Whether the handler has a byte to hand over, and clears the flag on the way
; out.  Read-and-clear rather than a separate clear, so a flag raised while the
; caller was busy cannot be lost and cannot be cleared twice.
;
; The caller is claiming the interrupt has been taken care of, so it must call
; this only when it is really going to do something about the wake.
;
; REGISTERS: A is the return and HL is a pointer, and nothing else is touched.
; That is not tidiness.  The first version of this used C to hold the value
; across the clear, on the assumption that C is scratch - and the C this is
; called from had something of its own in C, so clearing the flag quietly
; corrupted the caller's frame.  Nothing crashed in the build and the host tests
; passed, because a C model of this routine does not clobber C either.  The Z80
; has no registers the compiler will promise are dead across a call, so the rule
; for every bridge in this file is: touch only what the result needs, or save it.
_muzix_uart_rx_pending:
	ld	hl,#_uart_rx_pending
	ld	a,(hl)
	ld	(hl),#0			; clear, without destroying A
	ret

; uint8_t muzix_tick_cycles(void)  - A = channel 3's count
;
; One count is MUZIX_CTC_CYCLE_DIV cycles.  8 bits, wraps every 8.9 ms.
;
; IT COUNTS DOWN.  The CTC is a down-counter and this is a raw read of one, so a
; later reading is a SMALLER number and an elapsed interval is mark - now, not
; now - mark.  The difference mod 256 is the gap, and it needs no wrap handling
; because the thing measured is an idle wakeup of a few thousand cycles - about 21
; counts - while a gap long enough to wrap would be 8.9 ms of the kernel running
; flat out, which is a different fault and shows up as a figure of 1.00.
;
; The port is a literal because sdasz80 will not take a symbol in `in a,(...)`.
; It is the CTC's third channel, and the same register uart_io.s reads as the
; UART's receive buffer at 0x68 is a different port entirely.
_muzix_tick_cycles:
	in	a,(0x23)
	ret

; uint16_t muzix_tick_calibrate_ctc3(void) - result in DE
;
; Measure CTC3 counts over exactly 32 independent 120 Hz ticks. Polling the
; down-counter more frequently than its 16 ms wrap lets this routine count each
; wrap, so the result works even when the total interval spans many wraps.
; Dividing by 32 is a rounded five-bit shift. The 16-bit poll counter bounds the
; wait if the tick stops; rates above 255 are rejected because kernel_halt()
; currently records each HALT with an 8-bit modulo delta.
_muzix_tick_calibrate_ctc3:
	xor	a
	ld	(_muzix_tick_calibration_status),a
	call	_muzix_tick_now
	ld	hl,#32
	add	hl,de
	ld	(_ctc3_cal_target),hl
	ld	(_ctc3_cal_start_tick),de
	call	_muzix_tick_cycles
	ld	(_ctc3_cal_start),a
	ld	(_ctc3_cal_previous),a
	xor	a
	ld	(_ctc3_cal_wraps),a
	ld	bc,#0
1$:
	call	_muzix_tick_now
	in	a,(0x23)
	ld	l,a
	ld	a,(_ctc3_cal_previous)
	cp	l
	jr	nc,2$
	ld	a,(_ctc3_cal_wraps)
	inc	a
	jr	nz,3$
	ld	a,#3
	ld	(_muzix_tick_calibration_status),a
	jr	8$
3$:
	ld	(_ctc3_cal_wraps),a
2$:
	ld	a,l
	ld	(_ctc3_cal_current),a
	ld	(_ctc3_cal_previous),a
	inc	bc
	ld	a,b
	or	c
	jr	nz,5$
	ld	a,#1
	ld	(_muzix_tick_calibration_status),a
	jr	8$
5$:
	ld	hl,(_ctc3_cal_target)
	or	a
	sbc	hl,de
	jr	nz,1$

	ld	a,(_ctc3_cal_wraps)
	ld	h,a
	ld	a,(_ctc3_cal_start)
	ld	l,a
	ld	a,(_ctc3_cal_current)
	ld	c,a
	ld	a,l
	sub	c
	ld	l,a
	ld	a,h
	sbc	a,#0
	ld	h,a
	ld	bc,#16
	add	hl,bc
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	srl	h
	rr	l
	ld	a,h
	or	a
	jr	z,6$
	ld	a,#3
	ld	(_muzix_tick_calibration_status),a
	jr	8$
6$:
	ld	a,l
	or	a
	jr	nz,7$
	ld	a,#2
	ld	(_muzix_tick_calibration_status),a
	jr	8$
7$:
	ex	de,hl
	ret
8$:
	call	_muzix_tick_now
	ld	hl,(_ctc3_cal_start_tick)
	ex	de,hl
	or	a
	sbc	hl,de
	ld	(_muzix_tick_calibration_elapsed_ticks),hl
4$:
	ld	de,#0
	ret

; uint8_t muzix_ctc_ch0(void)  /  uint8_t muzix_ctc_ch1(void)  - A = counter
;
; A raw read of a CTC channel's down-counter.  For DIAGNOSIS, not for the kernel:
; reading channel 0 has a side effect on a real CTC - an outstanding interrupt is
; acknowledged - so a program that reads these while running will perturb what it
; is measuring.  The boot probe reads them once, at a point where the tick is
; already known to be dead, and that is what they are for.
;
; Two zero-argument routines rather than one taking a port, because this tree's
; measured calling convention puts a parameter in HL or A depending on width and
; a diagnostic is the last place to want that ambiguity.  The ports are literals
; for the same reason as elsewhere in this file: sdasz80 will not take a symbol in
; `in a,(...)`.
_muzix_ctc_ch0:
	in	a,(0x20)
	ret

_muzix_ctc_ch1:
	in	a,(0x21)
	ret
