/*
 * Muzix kernel entry point.
 *
 * Initializes services, mounts the ROM filesystem, opens a TTY, and
 * launches the userspace shell via exec_loader.
 */

#include "../platform/zeta-v2/syscall_runtime.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/syscall_entry.h"
#include "../platform/zeta-v2/syscall_vector.h"
#include "../platform/zeta-v2/z80_io.h"
#include "../platform/zeta-v2/uart_io.h"
#include "../platform/zeta-v2/tick.h"
#include "../platform/zeta-v2/trace.h"
#include "../platform/zeta-v2/context_switch.h"
#include "kernel_loop.h"

/* No leading underscore: SDCC prefixes C identifiers with one, so the C name here
 * would resolve to __muzix_tick_probe and the linker would report it undefined
 * against the assembly's _muzix_tick_probe.  The underscore here is SDCC's, not
 * mine. */
extern uint8_t muzix_tick_probe;
#include "load.h"
#include "../pm/pm_service.h"
#include "../mm/mm_service.h"
#include "../mm/mproc_model.h"
#include "../fs/fs_service.h"
#include "../fs/volume.h"
#include "../fs/block_device.h"
#include "exec_loader.h"
#include "rom_read.h"

extern void shell_task(muzix_fs_service_t *fs);
extern int rom_read_block(struct muzix_block_device *device, uint16_t block, uint8_t *buffer);

/* The image fills the ROM from MUZIX_ROMFS_BASE_OFFSET to the end of the 512 KiB
 * part.  66 was a stale hand count left over from an earlier, smaller image; the
 * superblock declares 67 zones, so the mount's "nzones must not exceed the
 * device" sanity check rejected the volume outright and every exec then failed
 * at open with EXEC FAIL A.  Derived from the ROM geometry now, so the count
 * cannot fall behind the image again. */
#define ROMFS_BLOCK_COUNT   MUZIX_ROMFS_BLOCK_COUNT

static muzix_kernel_loop_t g_loop;
static muzix_zeta_syscall_runtime_t g_runtime;
static muzix_system_task_t g_task;
static muzix_pm_service_t g_pm;
static muzix_mm_service_t g_mm;
static muzix_fs_service_t g_fs;
static muzix_fs_volume_t g_volume;
static muzix_block_device_t g_block_dev;
uint16_t muzix_userspace_entry;
uint8_t muzix_userspace_banks[4];

/* Resume point for a context switch, read by window-0 assembly immediately
 * before the map changes.  Same shape as the pair above; see
 * platform/zeta-v2/kernel_entry.s for why these are copied out of _DATA first. */
uint16_t muzix_resume_pc;
uint16_t muzix_resume_sp;
uint16_t muzix_resume_ret;
uint8_t muzix_resume_banks[4];

/* One staging buffer for the fork stack copy, and nothing more.
 *
 * It used to be 1024 bytes and it used to be the whole of the copy: the syscall
 * entry ran a single `ldir` of the entire active stack into it, because that was
 * the only point where the parent's page was still mapped, and a buffer that
 * holds a whole stack has to be as big as the deepest stack anyone might have.
 * It measured 1024 bytes of _DATA as the single largest object in the area, on a
 * kernel whose _DATA has nine bytes of growth left before the layout gate fails.
 *
 * The copy is two-sided now and the buffer is one piece of it. The entry
 * measures the active stack and publishes only the length; the fork handler then
 * reads it back a MUZIX_ZETA_COPY_STAGING-sized piece at a time, out of the
 * parent's page through the memory manager and into the child's, using this
 * buffer as the piece in transit. So the buffer's size is now the transfer's
 * size and nothing else: the same 256 bytes the copy primitive already stages
 * through, and the 768 bytes it used to occupy are what the process table's
 * command names are paid for out of.
 *
 * The entry writes zero to the length whenever it declines to measure, and the
 * fork handler then leaves the child sharing the parent's stack, as it did
 * before any of this existed. */
uint8_t muzix_fork_stack_scratch[MUZIX_ZETA_COPY_STAGING];
uint16_t muzix_fork_stack_len;

/* Non-zero when a handler has asked for a context switch.  The trap's exit
 * path tests it and hands over to window 0, which clears it first so a refused
 * switch cannot repeat forever. */
uint16_t muzix_switch_pending;
muzix_exec_load_args_t g_muzix_exec_load_args;

static void uart_putc(char c);
static void uart_puts(const char *s);

/* Set to 1 to run the one-shot HALT probe at the end of boot.  See the comment
 * there: this build exists because the console halt did not return on real
 * hardware and every answer since then has been inference. */
#ifndef MUZIX_HALT_PROBE
#define MUZIX_HALT_PROBE 0
#endif

static uint8_t g_current_bank2 = MUZIX_ZETA_KERNEL_BANK_2;

static void uart_putc(char c)
{
    if (c == '\n') {
        muzix_trace_putc((uint8_t)'\r');
    }
    muzix_trace_putc((uint8_t)c);
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s);
        s++;
    }
}

static void uart_puthex(uint8_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_putc(hex[v >> 4]);
    uart_putc(hex[v & 0xF]);
}

static void report_ctc_chain(void)
{
    volatile uint16_t delay;
    uint8_t sample;

    uart_puts("CTC0:");
    for (sample = 0; sample < 10u; sample++) {
        uart_puthex(muzix_ctc_ch0());
        for (delay = 0; delay < 4000u; delay++) {
        }
    }
    uart_puts("\nCTC1:");
    for (sample = 0; sample < 10u; sample++) {
        uart_puthex(muzix_ctc_ch1());
        for (delay = 0; delay < 4000u; delay++) {
        }
    }
    uart_puts("\n");
}

void kernel_main(void)
{
    uart_puts("KM\n");
    process_map_t kernel_map = {{MUZIX_ZETA_KERNEL_BANK_0, MUZIX_ZETA_KERNEL_BANK_1,
                                 MUZIX_ZETA_KERNEL_BANK_2, MUZIX_ZETA_KERNEL_BANK_3}};
    process_map_t init_map;
    muzix_mproc_t init_mp;
    uint16_t ctc3_counts_per_tick;
    int shell_slot = -1;
    int i;

    muzix_zeta_uart_init();

    /* Start the 120 Hz tick, which is what makes every bounded wait in the
     * system bounded rather than hopeful, and what makes UTIME and STIME mean
     * something other than a count of entries into userspace.
     *
     * This is the first instruction in the kernel that enables interrupts at
     * all: platform/zeta-v2/kernel_entry.s has run with `di` since 0x0098, so
     * until here no interrupt had ever been taken on this machine.  It is
     * placed here, and not later, because the three failure paths below fall
     * into shell_task(), whose input loop is bounded on the tick - a kernel that
     * reached the fallback shell with the counter frozen at zero would have
     * replaced a spin with a hang rather than with a timeout.  It is placed
     * here and not earlier because the boot stub's own ldir and the vector at
     * 0x0030 are the two things that have to be in place before anything can
     * take an interrupt, and both are.
     *
     * What the handler may touch is window 0 and window 3 and nothing else, in
     * every map, including part-way through a map change - the argument is in
     * platform/zeta-v2/tick.s and it is why enabling this here is safe rather
     * than hopeful.  Nothing about the tick preempts: the handler counts and
     * returns, and there is no scheduler switch in it. */
    muzix_tick_init();
    ctc3_counts_per_tick = muzix_tick_calibrate_ctc3();
    if (ctc3_counts_per_tick == 0) {
        ctc3_counts_per_tick = MUZIX_LOAD_CTC3_COUNTS_PER_TICK;
        switch (muzix_tick_calibration_status) {
        case 1:
            uart_puts("LOAD CAL: tick timeout, ticks seen: ");
            uart_puthex((uint8_t)(muzix_tick_calibration_elapsed_ticks >> 8));
            uart_puthex((uint8_t)muzix_tick_calibration_elapsed_ticks);
            uart_puts("; using 4MHz default\n");
            report_ctc_chain();
            break;
        case 2:
            uart_puts("LOAD CAL: CTC3 did not advance, ticks seen: ");
            uart_puthex((uint8_t)(muzix_tick_calibration_elapsed_ticks >> 8));
            uart_puthex((uint8_t)muzix_tick_calibration_elapsed_ticks);
            uart_puts("; using 4MHz default\n");
            break;
        case 3:
            uart_puts("LOAD CAL: CTC3 rate exceeds 8-bit range, ticks seen: ");
            uart_puthex((uint8_t)(muzix_tick_calibration_elapsed_ticks >> 8));
            uart_puthex((uint8_t)muzix_tick_calibration_elapsed_ticks);
            uart_puts("; using 4MHz default\n");
            break;
        default:
            uart_puts("LOAD CAL: failed (unknown reason); using 4MHz default\n");
            break;
        }
    } else {
        uart_puts("CTC3/tick: ");
        uart_puthex((uint8_t)(ctc3_counts_per_tick >> 8));
        uart_puthex((uint8_t)ctc3_counts_per_tick);
        uart_puts("\n");
    }
    /* The windows are initialized after calibration so startup time is not
     * included in the first CPU-busy interval. */
    muzix_load_init(muzix_tick_now(), ctc3_counts_per_tick);


    init_map = kernel_map;

    g_current_bank2 = MUZIX_ZETA_KERNEL_BANK_2;

    /* Initialize context stack for temp map operations */
    extern void zeta_context_stack_init(void);
    zeta_context_stack_init();

    muzix_mproc_init(&init_mp, 0x0000u, 0x1000u, 0x2000u,
                     0x1000u, 0x1000u, 0x1000u, 1);

    muzix_kernel_loop_init(&g_loop, &kernel_map, 1, &init_map);
    muzix_kernel_loop_register(&g_loop, 0, 1, &init_mp, &init_map,
                               MUZIX_PROC_TASK_Q,
                               (uint8_t)(MUZIX_PROC_FREE | MUZIX_PROC_NO_MAP));

    muzix_system_task_init(&g_task, &g_loop, &kernel_map);
    muzix_pm_service_init(&g_pm, &g_task, 1);
    muzix_mm_service_init(&g_mm, &g_task, 2);
    g_loop.mm = &g_mm;

    /* Initialize MM memory allocator with kernel map */
    muzix_mm_memory_init(&g_mm, &kernel_map);

    g_block_dev.context = &g_mm;  /* Pass MM service for banked ROM reads */
    g_block_dev.block_count = ROMFS_BLOCK_COUNT;

    uart_puts("MNT\n");

    muzix_fs_volume_init(&g_volume, &g_block_dev);
    if (muzix_fs_volume_mount(&g_volume) != 0) {
        uart_puts("MNT FAIL\n");
    }

    muzix_fs_service_init(&g_fs, &g_task, 3);
    muzix_fs_service_attach_volume(&g_fs, &g_volume);

    uart_puts("SVC\n");

    muzix_kernel_loop_bind_syscalls(&g_loop, &g_runtime, &g_fs);

    /* Point the syscall layer at the exec loader.
     *
     * muzix_zeta_syscall_runtime_init() fills in userspace.kernel and
     * userspace.fs but has no way to know about kernel/exec_loader.c, so
     * userspace.exec_loader was left NULL. Every exec then took the
     * `if (ctx->exec_loader && ...)` false branch: no binary was loaded, exec
     * reported success, and the handoff re-entered the previous program. Set it
     * here, where both sides are visible. The struct is the one the runtime
     * already bound, so filling the field in after the bind is enough. */
    g_runtime.userspace.exec_loader = muzix_exec_load_for_slot;

    /* Initialize global zeta state pointer for syscall entry assembly */
    muzix_z80_set_zeta_state(&g_mm.zeta);

    /* Open TTY for stdin (0), stdout (1), stderr (2) */
    muzix_fs_service_open_tty(&g_fs);
    muzix_fs_service_open_tty(&g_fs);
    muzix_fs_service_open_tty(&g_fs);

    uart_puts("RDY\n");

    /* Kernel process (slot 0) - set pid=0 manually */
    g_loop.system.startup.proc_table.slots[0].active = 1;
    g_loop.system.startup.proc_table.slots[0].pid = 0;

    const char *shell_argv[] = { "shell", NULL };

    /* Set up global args for exec_load */
    g_muzix_exec_load_args.fs = &g_fs;
    g_muzix_exec_load_args.mm = &g_mm;
    g_muzix_exec_load_args.processes = &g_loop.system.startup.proc_table;
    g_muzix_exec_load_args.slot = 1;
    g_muzix_exec_load_args.path = "shell";
    g_muzix_exec_load_args.argv = shell_argv;

    /* Step 1: Fork init from kernel (slot 0 -> slot 1, PID 1) */
    int rc = muzix_pm_fork(&g_pm, 0, 1, 1);
    if (rc != 0) {
        uart_puts("FORK FAIL\n");
        muzix_trace_dump();
        shell_task(&g_fs);
        return;
    }
    /* Step 2: Load the shell binary into init's process memory */
    uart_puts("LOAD\n");

    int load_rc = muzix_exec_load();
    if (load_rc != 0) {
        uart_puts("LOAD FAIL\n");
        muzix_trace_dump();
        shell_task(&g_fs);
        return;
    }

    /* Save the process map for later userspace entry */
    g_loop.system.startup.proc_table.slots[1].saved_context =
        g_loop.system.startup.proc_table.slots[1].map;

    /* Step 3: Mark init process as ready to run via PM exec */
    int exec_rc = muzix_pm_exec(&g_pm, 1,
        g_loop.system.startup.proc_table.slots[1].stack_ptr);
    if (exec_rc != 0) {
        uart_puts("PM FAIL\n");
        muzix_trace_dump();
        shell_task(&g_fs);
        return;
    }

    uart_puts("SCHED\n");
    muzix_trace_stack_report();

    /* MUZIX_HALT_PROBE - one HALT at the end of boot, where everything above has
     * already run and the tick is known to be alive (RDY and LOAD both used it).
     *
     * This exists because the console wait's halt did not come back on real
     * hardware on 2026-10-04: the machine booted, printed this banner and its
     * prompt, and then sat with the HALT LED lit, accepting nothing.  Everything
     * since has been inference, and inference is what produced the wrong answer
     * twice.  This measures it.
     *
     * Read the output like this:
     *
     *   HALT: no return      the line after "tick before" is missing.  HALT does not
     *                        come back on this board, and the console halt can never
     *                        be used.  The tick was alive immediately before, so the
     *                        interrupt was being delivered; what it will not do is
     *                        resume the CPU.  That is a hardware or CTC wiring
     *                        question and nothing in userspace can fix it.
     *
     *   HALT: returned       HALT works, the difference between the two tick values
     *                        is how many ticks elapsed while stopped, and the fault
     *                        is therefore in the console wait's own path, not in the
     *                        instruction.
     *
     * If the difference is 0 the instruction returned without the tick having run,
     * which means the resume happened but the interrupt did not - a third case, and
     * the one that would explain a console that neither wakes nor dies.
     *
     * The probe cannot hang the machine any worse than the console already does: it
     * runs once, at a point where every earlier stage has already reported, and the
     * last line before the halt is on the wire when it does not come back. */
#if MUZIX_HALT_PROBE
    /* Two questions, asked in the order that makes them answerable.
     *
     * First: does an interrupt reach this CPU?  Enable, then spin until the tick
     * counter moves, with a bound so that a machine with no interrupt prints an
     * answer instead of spinning forever.  This cannot hang.  The hardware answered
     * "IRQ: ok, 0001 -> 0002", so interrupts do arrive.
     *
     * Second: does HALT come back?  On 2026-10-04 the probe did this alone, printed
     * "HALT tick 0001 ->" and stopped.  I read that as a dead interrupt channel and
     * went looking for one - and the counter reading 1 is not a dead channel, it is
     * a ROM-only boot taking 8.3 ms at 120 Hz, which is one interrupt, correctly.
     * So the first result was misread and the halt result was never checked against
     * a known-good interrupt.
     *
     * Both are now measured, in that order, in one run.  Nothing before the halt can
     * hang the machine, so if the halt does not come back the only thing that is in
     * question is the instruction.
     */
    {
        uint16_t before = muzix_tick_now();
        uint16_t after = before;
        uint32_t spins = 0u;
        uint16_t halt_before;
        uint16_t halt_after;
        uint16_t tick_mark;
        uint8_t cycle_before;
        uint8_t cycle_after;
        uint8_t idx;
        uint16_t cycle_total;

        z80_enable_interrupts();
        while (after == before && spins < 400000u) {
            spins++;
            after = muzix_tick_now();
        }
        uart_puts("IRQ: ");
        uart_puts(after != before ? "ok, " : "NONE, ");
        uart_puthex((uint8_t)(before >> 8));
        uart_puthex((uint8_t)before);
        uart_puts(" -> ");
        uart_puthex((uint8_t)(after >> 8));
        uart_puthex((uint8_t)after);
        uart_puts("\n");

        /* And now the instruction, with the interrupt known to be arriving.
         *
         * muzix_tick_probe makes the handler emit one character per tick while
         * the CPU is stopped, so that a halt which does not return still produces
         * output.  Without it there is one reading - the machine stops - and two
         * faults fit it, and this project has already guessed between them twice
         * and been wrong both times.
         *
         *   characters stream out after the line below
         *       the tick interrupt reaches a stopped CPU and its RETI does not
         *       resume the instruction, so the halt itself is what to fix;
         *   the line below is the last thing on the wire
         *       nothing is delivered to a stopped CPU, no change to the halt path
         *       can help, and the board's interrupt wiring is the question.
         */
        muzix_tick_probe = (uint8_t)'#';
        z80_enable_interrupts();

        /* QUESTION 0, AND IT COMES FIRST BECAUSE IT DEPENDS ON NOTHING.
         *
         * Read the CTC's own counters.  Everything after this line reasons about
         * interrupts, and an interrupt is the LAST thing in the chain to be able
         * to go wrong - it depends on the clock, the prescaler, the counter, the
         * vector base, the I register and the table all being right at once.  The
         * counters are upstream of all of that, so they localise the fault.
         *
         * The board's chain is
         *
         *   UART_CLK/2 (921.6 kHz) -> ch0 / 256 -> 3.6 kHz -> ZC/TO -> ch1 / 30
         *     -> 120 Hz tick
         *
         * and the three outcomes are:
         *
         *   ch0 frozen and ch1 frozen   ch0 is not counting: no clock reaching it,
         *                                or it was never programmed;
         *   ch0 moving, ch1 frozen      ch1 is not counting: its trigger is not
         *                                being driven, or it was never programmed;
         *   both moving                 the chain is alive and the fault is
         *                                downstream - vector base, I register, the
         *                                table, or the acknowledge.
         *
         * The spin between the two readings is long enough that a counting
         * channel 0 - whose period is 8 * 256 = 2 048 cycles, about a quarter of
         * a millisecond - must wrap several times.  Anything less would read as
         * "frozen" for a channel that is in fact running.
         */
        uart_puts("CTC0:");
        for (idx = 0u; idx < 10u; idx++) {
            uart_puthex(muzix_ctc_ch0());
            for (spins = 0u; spins < 4000u; spins++) {
                /* pause between reads: channel 0's period is 2048 cycles */
            }
        }
        uart_puts("\nCTC1:");
        for (idx = 0u; idx < 10u; idx++) {
            uart_puthex(muzix_ctc_ch1());
            for (spins = 0u; spins < 4000u; spins++) {
            }
        }
        uart_puts("\n");

        /* QUESTION 1: DOES THE TICK ADVANCE?
         *
         * A FIXED SPIN AND A REPORTED DELTA, NOT A WAIT FOR A TARGET.
         *
         * The first version waited for the counter to reach 240 and had a
         * 40-million-iteration bound.  That bound is 3 200 million cycles, which
         * is 434 seconds of CPU at 7 372 800 Hz, so on a machine whose tick is
         * dead the loop had to run for SEVEN MINUTES before printing the word
         * STALLED.  On the board that read as "the machine hung after TICK:" -
         * which is what I reported, and what sent this looking for a fault in
         * the interrupt path that was not there.  A measurement that takes seven
         * minutes to report its own failure is not a measurement.
         *
         * So: spin a fixed half-million iterations, then print how far the
         * counter actually got.  Half a million is about 40 million cycles,
         * which at 120 Hz is roughly 650 ticks - so a working tick shows a large
         * number and a dead one shows zero, and both answers arrive in about
         * five seconds.
         */
        /* ISOLATION: MUTE THE UART CHANNEL, THEN ASK AGAIN.
         *
         * Channel 2 is armed in tick_init with interrupts enabled and a time
         * constant of 1, clocked from the 16550's interrupt output - and the
         * 16550 is not initialised until later in kernel_main, so for a while
         * after reset that output is whatever the chip's undefined reset state
         * left it.  A counter with a time constant of 1 fires on the first edge
         * it sees, so a floating or oscillating interrupt line is enough to make
         * it fire a few times - and the `#` characters in the boot trace, one in
         * "MNT" and one in "CT#C", are the right size for exactly that.
         *
         * This is not a guess about the fault, it is the experiment that separates
         * two candidates that look identical from the outside:
         *
         *   the tick starts moving  channel 2 was taking the interrupts, or
         *                         holding the CTC's shared line, and the tick's
         *                         own vector never got serviced;
         *   still nothing         the tick's interrupt is not being delivered at
         *                         all, and the fault is in the vector path - the
         *                         base, the I register, the table, or the
         *                         acknowledge.
         *
         * It is written from here rather than built in, so the same ROM answers
         * both questions: the channel is re-armed afterwards, so if the tick starts
         * we have isolated a conflict rather than removed a feature.
         */
        uart_puts("MUTE2: ");
        z80_outb(0x22u, 0x43u);       /* counter, interrupts OFF, no TC byte */
        uart_puts("muted\n");

        uart_puts("TICK: ");
        before = muzix_tick_now();
        spins = 0u;
        while (spins < 500000u) {
            spins++;
        }
        after = muzix_tick_now();
        uart_puts("moved ");
        uart_puthex((uint8_t)(((uint16_t)(after - before)) >> 8));
        uart_puthex((uint8_t)((uint16_t)(after - before)));
        uart_puts(" ticks\n");

        /* Put channel 2 back the way tick_init left it, so the UART can still
         * wake a parked reader and so whatever follows is the real machine. */
        z80_outb(0x22u, MUZIX_CTC_UART_IRQ_CONTROL);
        z80_outb(0x22u, 0x01u);

        /* QUESTION 2: DOES THE INSTRUCTION COME BACK?
         *
         * Only worth asking if the first question answered yes, and that is why
         * it is here rather than beside the IRQ test: a HALT that does not return
         * is uninformative on its own, and this line is the same either way.
         */
        muzix_tick_probe = (uint8_t)'#';
        uart_puts("HALT: entering\n");
        halt_before = muzix_tick_now();
        tick_mark = halt_before;
        cycle_total = 0u;
        for (idx = 0u; idx < 8u; idx++) {
            do {
                cycle_before = muzix_tick_cycles();
                z80_idle_halt();
                cycle_after = muzix_tick_cycles();
                cycle_total += (uint8_t)(cycle_before - cycle_after);
                halt_after = muzix_tick_now();
            } while (halt_after == tick_mark);
            tick_mark = halt_after;
        }
        muzix_tick_probe = 0u;
        uart_puts("CTC3 8-tick idle counts: ");
        uart_puthex((uint8_t)(cycle_total >> 8));
        uart_puthex((uint8_t)cycle_total);
        uart_puts("\n");
        uart_puts("HALT: ");
        uart_puthex((uint8_t)(halt_before >> 8));
        uart_puthex((uint8_t)halt_before);
        uart_puts(" -> ");
        uart_puthex((uint8_t)(halt_after >> 8));
        uart_puthex((uint8_t)halt_after);
        uart_puts(", ");
        uart_puts(halt_after != halt_before ? "returned\n" : "SAME TICK\n");
    }
#endif

    while (1) {
        muzix_kernel_loop_run(&g_loop);
    }
}
