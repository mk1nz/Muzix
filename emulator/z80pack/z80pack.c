/**
 * Z80pack-Based Emulator - Core Implementation
 * 
 * A clean z80pack-style Z80 emulator that supports:
 * - RomWBW
 * - FUZIX
 * - Muzix
 * 
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "z80pack.h"

#define ZETA_ROM_PAGE_MAX       0x1F
#define ZETA_RAM_PAGE_MIN       0x20
#define ZETA_RAM_PAGE_MAX       0x3F

/* Z80 CPU core from superzazu/z80 */
#include "../z80/z80.h"

/* HBIOS support */
#include "hbios.c"

/* Forward declarations for static functions */
static uint8_t z80pack_z80_read_byte(void* userdata, uint16_t addr);
static void z80pack_z80_write_byte(void* userdata, uint16_t addr, uint8_t val);
static uint8_t z80pack_z80_port_in(z80* z, uint8_t port);
static void z80pack_z80_port_out(z80* z, uint8_t port, uint8_t val);
static void z80pack_fdc_execute(z80pack_t* pack, uint8_t cmd);
static void z80pack_fdc_start_read(z80pack_t* pack);
static void z80pack_fdc_start_write(z80pack_t* pack);
static void z80pack_fdc_transfer(z80pack_t* pack, bool is_read);
static void z80pack_update_visible_ram(z80pack_t* pack);
static void z80pack_update_bank(z80pack_t* pack, uint8_t port);
static void z80pack_ctc_tick(z80pack_t* pack);
static void z80pack_rtc_begin_transfer(z80pack_t* pack, uint8_t cmd);
static void z80pack_rtc_latch_write(z80pack_t* pack, uint8_t val);
static uint8_t z80pack_mapped_bank(z80pack_t* pack, uint16_t addr);

static uint8_t to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

static void z80pack_rtc_build_clock(z80pack_t* pack)
{
    time_t now = time(NULL);
    struct tm tm_now;
#if defined(_POSIX_VERSION)
    localtime_r(&now, &tm_now);
#else
    struct tm *tmp = localtime(&now);
    if (tmp != NULL) {
        tm_now = *tmp;
    } else {
        memset(&tm_now, 0, sizeof(tm_now));
    }
#endif
    pack->rtc_read_buf[0] = to_bcd((uint8_t)tm_now.tm_sec);
    pack->rtc_read_buf[1] = to_bcd((uint8_t)tm_now.tm_min);
    pack->rtc_read_buf[2] = to_bcd((uint8_t)tm_now.tm_hour);
    pack->rtc_read_buf[3] = to_bcd((uint8_t)tm_now.tm_mday);
    pack->rtc_read_buf[4] = to_bcd((uint8_t)(tm_now.tm_mon + 1));
    pack->rtc_read_buf[5] = to_bcd((uint8_t)(tm_now.tm_wday == 0 ? 7 : tm_now.tm_wday));
    pack->rtc_read_buf[6] = to_bcd((uint8_t)(tm_now.tm_year % 100));
    pack->rtc_read_buf[7] = 0x00; /* Write-protect register */
}

static void z80pack_rtc_begin_transfer(z80pack_t* pack, uint8_t cmd)
{
    pack->rtc_read_mode = (cmd & 0x01U) ? true : false;
    pack->rtc_write_mode = !pack->rtc_read_mode;
    pack->rtc_read_pos = 0;
    pack->rtc_shift_bits = 0;

    if (pack->rtc_read_mode) {
        if ((cmd & 0xFEU) == 0xBEU) {
            z80pack_rtc_build_clock(pack);
            pack->rtc_read_len = 8;
            pack->rtc_shift_out = pack->rtc_read_buf[0];
        } else {
            z80pack_rtc_build_clock(pack);
            pack->rtc_read_len = 1;
            switch (cmd & 0xFEU) {
                case 0x80: pack->rtc_shift_out = pack->rtc_read_buf[0]; break;
                case 0x82: pack->rtc_shift_out = pack->rtc_read_buf[1]; break;
                case 0x84: pack->rtc_shift_out = pack->rtc_read_buf[2]; break;
                case 0x86: pack->rtc_shift_out = pack->rtc_read_buf[3]; break;
                case 0x88: pack->rtc_shift_out = pack->rtc_read_buf[4]; break;
                case 0x8A: pack->rtc_shift_out = pack->rtc_read_buf[5]; break;
                case 0x8C: pack->rtc_shift_out = pack->rtc_read_buf[6]; break;
                default:   pack->rtc_shift_out = 0x00; break;
            }
        }
    } else {
        pack->rtc_read_len = 0;
        pack->rtc_shift_out = 0x00;
    }
}

static void z80pack_rtc_latch_write(z80pack_t* pack, uint8_t val)
{
    bool new_ce = (val & Z80PACK_DSRTC_CE) != 0;
    bool new_clk = (val & Z80PACK_DSRTC_CLK) != 0;
    bool data_out = (val & Z80PACK_DSRTC_DATA) != 0;

    pack->rtc_latch = (uint8_t)(val & 0xF0);

    if (!pack->rtc_ce_high && new_ce) {
        pack->rtc_cmd_bits = 0;
        pack->rtc_cmd = 0;
        pack->rtc_shift_in = 0;
        pack->rtc_shift_bits = 0;
        pack->rtc_read_mode = false;
        pack->rtc_write_mode = false;
        pack->rtc_read_len = 0;
        pack->rtc_read_pos = 0;
    }

    if (pack->rtc_ce_high && !new_ce) {
        pack->rtc_read_mode = false;
        pack->rtc_write_mode = false;
    }

    if (new_ce && !pack->rtc_clk_high && new_clk) {
        if (pack->rtc_cmd_bits < 8) {
            if (data_out) {
                pack->rtc_cmd |= (uint8_t)(1U << pack->rtc_cmd_bits);
            }
            pack->rtc_cmd_bits++;
            if (pack->rtc_cmd_bits == 8) {
                z80pack_rtc_begin_transfer(pack, pack->rtc_cmd);
            }
        } else if (pack->rtc_write_mode) {
            if (data_out) {
                pack->rtc_shift_in |= (uint8_t)(1U << pack->rtc_shift_bits);
            }
            pack->rtc_shift_bits++;
            if (pack->rtc_shift_bits >= 8) {
                /* Writes currently accepted but ignored (clock follows host time). */
                pack->rtc_shift_in = 0;
                pack->rtc_shift_bits = 0;
            }
        } else if (pack->rtc_read_mode) {
            pack->rtc_shift_bits++;
            if (pack->rtc_shift_bits >= 8) {
                pack->rtc_shift_bits = 0;
                if (pack->rtc_read_pos + 1U < pack->rtc_read_len) {
                    pack->rtc_read_pos++;
                    pack->rtc_shift_out = pack->rtc_read_buf[pack->rtc_read_pos];
                }
            }
        }
    }

    pack->rtc_ce_high = new_ce;
    pack->rtc_clk_high = new_clk;
}

/* Raise an interrupt vector, queueing it if one is already pending.
 *
 * WHY THIS EXISTS RATHER THAN CALLING z80_gen_int() DIRECTLY
 * -------------------------------------------------------
 * z80_gen_int() holds exactly ONE pending vector: z->int_pending is a flag and
 * z->int_data is a byte.  Calling it twice in the same instruction - once for
 * the tick, once for the UART - makes the second overwrite the first, so one of
 * the two sources is silently lost.
 *
 * That is not a corner case here.  The tick fires every 61 440 cycles and a
 * keystroke arrives whenever the user types, so "both in the same step" happens
 * constantly - and when it happens the UART's byte is never read, its interrupt
 * line stays asserted, and because the edge detector has already seen the edge
 * it never fires again.  The observable result was a machine that sat at its
 * prompt ignoring the keyboard completely.
 *
 * The hardware does not lose interrupts this way.  Every CTC channel shares one
 * INT line, but each channel's terminal count latches independently and the Z80
 * services them one at a time from the daisy chain: a channel that is still
 * waiting keeps its claim until the CPU takes its vector.  So a source that
 * cannot be delivered immediately stays pending and is delivered as soon as the
 * CPU has finished with the current one - which is what this queue does.
 *
 * Depth 8: two sources at 120 Hz and a UART cannot outrun the CPU, and if they
 * ever did, dropping the oldest is the right answer anyway - a full queue means
 * the machine is not taking interrupts at all.
 */
static void z80pack_raise_irq(z80pack_t* pack, uint8_t vector)
{
    if (!pack->cpu->int_pending) {
        z80_gen_int(pack->cpu, vector);
        return;
    }
    if (pack->irq_queue_len < Z80PACK_IRQ_QUEUE_DEPTH) {
        pack->irq_queue[pack->irq_queue_len++] = vector;
    }
}

/* Hand the CPU the next queued vector, if it has finished with the current one.
 *
 * Called once per instruction, after the CPU has stepped, so a vector that was
 * taken during that instruction is gone from int_pending by the time this looks
 * and the next one can be delivered.
 */
static void z80pack_irq_pump(z80pack_t* pack)
{
    if (pack->irq_queue_len == 0 || pack->cpu->int_pending) {
        return;
    }
    z80_gen_int(pack->cpu, pack->irq_queue[0]);
    pack->irq_queue_len--;
    if (pack->irq_queue_len > 0) {
        memmove(pack->irq_queue, pack->irq_queue + 1, pack->irq_queue_len);
    }
}

/* Recompute every CTC channel's period from the Zeta SBC V2 wiring.
 *
 * The board's README, "Interrupts", is the authority here and it is specific:
 *
 *   "Channels 0 and 1 are chained together.  So that channel 1 can be used to
 *    generate low-frequency periodic interrupts:
 *      Channel's 0 CLK/TG input is connected to UART_CLK/2 (921.6 kHz)
 *      Channel's 1 CLK/TG input is connected to channel's 0 ZC/TO output"
 *
 * So:
 *
 *   UART_CLK/2 = 921 600 Hz = CPU / 8 at this build's 7 372 800 Hz
 *        |
 *        v
 *   ch0: COUNTER, TC 256          ->  3 600 Hz
 *        |
 *        | ZC/TO
 *        v
 *   ch1: COUNTER, TC 30           ->  120 Hz
 *
 * which is 8 * 256 * 30 = 61 440 CPU cycles per tick, and 7 372 800 / 61 440 is
 * exactly 120.
 *
 * WHAT THIS REPLACES, AND WHY IT MATTERED
 * --------------------------------------
 * This hook used to fire channel 1 straight off total_cycles with period
 * prescaler * time constant.  That is a free-running CPU cycle timer, which is
 * not a thing this board has: channel 1 is a counter whose trigger input comes
 * from channel 0.  The two agree only by coincidence - 256 * 240 = 61 440 - and
 * the coincidence was the bug's hiding place:
 *
 *   - with the prescaler unprogrammed, the real channel 1 has no clock at all and
 *     never interrupts, while this model kept ticking at 120 Hz.  So the board
 *     hung with the HALT LED lit and the emulator was fine, and every load
 *     figure and every loadavg printed here was computed against a tick that
 *     cannot occur on the hardware;
 *   - with the time constant set to the correct 30 for 120 Hz through the real
 *     chain, the old model would have computed 256 * 30 = 7 680 cycles, i.e.
 *     960 Hz, and reported the tick as sixteen times too fast.
 *
 * A channel also only counts once it has been given a control word - bit 0 of a
 * CTC control word is always 1, so a channel still holding its reset value of
 * zero has never been programmed.  That is modelled here, and it is what makes
 * "the prescaler was left alone" reproduce the hang rather than quietly working.
 */
static void z80pack_ctc_recompute(z80pack_t* pack)
{
    int ch;

    for (ch = 0; ch < 4; ch++) {
        uint16_t tc = (uint16_t)(pack->ctc_time_constant[ch]
                                  ? pack->ctc_time_constant[ch] : 256u);

        if ((pack->ctc_control[ch] & 0x01) == 0) {
            /* Never programmed: not counting, so it has no period. */
            pack->ctc_cycles_per_tick[ch] = 0;
        } else if ((pack->ctc_control[ch] & 0x40) == 0) {
            /* TIMER mode: the system clock, and CLK/TG is ignored entirely. D6
             * selects counter mode; D5 selects the timer prescale (0 = 16,
             * 1 = 256). This is the only mode in which a channel can free-run. */
            pack->ctc_cycles_per_tick[ch] =
                (uint64_t)((pack->ctc_control[ch] & 0x20) ? 256u : 16u) * (uint64_t)tc;
        } else if (ch == 0) {
            /* COUNTER mode on channel 0, clocked from UART_CLK/2 - which is a
             * real 921.6 kHz clock on the board, not a missing signal.  An
             * earlier version of this model lumped channel 0 in with the
             * interrupt-driven channels and gave it no period at all; channel 1
             * then inherited nothing, the tick stopped dead, and the boot probe
             * said so in the first line anyone read: "IRQ: NONE, 0000 -> 0000".
             * That is the model failing loudly rather than quietly, which is the
             * only reason it was found before the hardware saw it. */
            pack->ctc_cycles_per_tick[ch] =
                (uint64_t)(Z80PACK_CPU_CLOCK / Z80PACK_UART_CLK_2) * (uint64_t)tc;
        } else if (ch == 1) {
            /* COUNTER mode, clocked from channel 0's ZC/TO output, so its period
             * is channel 0's period times its own time constant.  This is the
             * whole tick, and it is why leaving channel 0 alone stops it. */
            pack->ctc_cycles_per_tick[ch] =
                pack->ctc_cycles_per_tick[0] * (uint64_t)tc;
        } else {
            /* COUNTER mode on channels 2 and 3: clocked from the UART interrupt
             * and the PPI's PC3 respectively.  Neither free-runs, and channel 2 is
             * driven by the edge-detect path in z80pack_ctc_tick() rather than by
             * a period. */
            pack->ctc_cycles_per_tick[ch] = 0;
        }
    }

    /* A new period starts a new count, so do not let a shortened or lengthened
     * one fire immediately against a stale origin. */
    for (ch = 0; ch < 4; ch++) {
        pack->ctc_last_fire_cycles[ch] = pack->total_cycles;
    }
}

static void z80pack_ctc_tick(z80pack_t* pack)
{
    uint8_t ch;
    for (ch = 0; ch < 4; ch++) {
        /* ZETA V2 emulation model:
         * - CH1 drives the periodic kernel tick.
         * - CH2 is the UART's interrupt controller: on the board its CLK/TG input
         *   is the 16550's INT output, so it counts EDGES ON THE WIRE, not CPU
         *   cycles.  It is handled below, not here.
         * - CH0 holds the vector base and CH3 the PPI, neither of which counts.
         */
        if (ch != 1) {
            continue;
        }
        /* Channel 1's period comes from z80pack_ctc_recompute(), which models
         * the board's chain rather than a CPU cycle timer.  See there for why
         * this is not the same thing. */
        if (!pack->ctc_irq_enabled[ch] || pack->ctc_cycles_per_tick[ch] == 0) {
            continue;
        }
        if ((pack->total_cycles - pack->ctc_last_fire_cycles[ch]) >= pack->ctc_cycles_per_tick[ch]) {
            pack->ctc_last_fire_cycles[ch] = pack->total_cycles;
            /* IM2 vector byte on CTC daisy-chain, base programmed on CH0.
             * Z80 CTC channel vectors occupy 2-byte slots. */
            z80pack_raise_irq(pack, (uint8_t)(pack->ctc_vector_base + (uint8_t)(ch << 1)));
        }
    }

    /* CTC channel 2 as the UART interrupt controller.
     *
     * The hardware model, and why it cannot be folded into the loop above:
     *
     * 16550 INT is asserted while the receive FIFO is non-empty (and IER bit 0
     * enabled), and deasserted when the FIFO drains.  The board wires that to CH2's
     * CLK/TG, and CH2 is programmed in COUNTER mode with a time constant of 1, so
     * ONE RISING EDGE - the asserted interrupt output - produces one terminal count and one
     * interrupt.  Nothing is time-based: the interrupt is caused by the byte.
     *
     * So this fires on the TRANSITION, not on a cycle count, which is why a
     * cycle-driven model of it would be wrong in both directions at once - it
     * would keep firing while the FIFO stayed non-empty and never fire when a
     * single byte arrived.  The condition is sampled once per instruction, which
     * is what the real edge detector does too: it latches a level and fires on
     * the edge, so a level that does not change produces no further interrupt and
     * the line cannot storm.
     *
     * Without this the whole IM2 UART path is untestable here: the emulator would
     * never raise an interrupt for a received byte, whatever the kernel did. */
    {
        const int have_byte = (pack->serial_available && pack->serial_available(pack->serial_userdata)) ? 1 : 0;
        const int irda = (pack->uart_ier & 0x01) ? 1 : 0;

        if (have_byte && irda && !pack->ctc_uart_int_prev) {
            if (pack->ctc_irq_enabled[2]) {
                /* One byte, one vector: ch2's slot in the IM2 table, which is
                 * ctc_vector_base + (2 << 1). */
                z80pack_raise_irq(pack, (uint8_t)(pack->ctc_vector_base + 4));
            }
        }
        /* Remember the active-high level, so only its 0->1 transition fires. */
        pack->ctc_uart_int_prev = (uint8_t)(have_byte && irda);
    }

    /* Hand the DS1302 one second for each second of CPU time that has passed.
     * 7.3728 MHz, so 7,372,800 cycles.  The model honours the chip's own
     * clock-halt bit and simply does nothing while it is set, exactly as the
     * hardware would. */
    {
        const uint64_t cycles_per_second = 7372800;
        if (!pack->rtc_started) {
            ds1302_init();
            pack->rtc_started = 1;
            pack->rtc_last_second_cycles = pack->total_cycles;
        }
        while ((pack->total_cycles - pack->rtc_last_second_cycles)
               >= cycles_per_second) {
            pack->rtc_last_second_cycles += cycles_per_second;
            ds1302_advance_one_second();
        }
    }
}

/* ============================================================================
 * Version
 * ============================================================================ */

static const char z80pack_version[] = "1.0.0-z80pack";

const char* z80pack_get_version(void)
{
    return z80pack_version;
}

/* ============================================================================
 * Z80 Callbacks
 * ============================================================================ */

static uint8_t z80pack_z80_read_byte(void* userdata, uint16_t addr)
{
    z80pack_t* pack = (z80pack_t*)userdata;
    if (!pack) return 0xFF;
    
    pack->memory_reads++;
    
    /* Debug: trace reads from 0x4260 (_tty_init) */
    if (addr == 0x4260 && pack->verbose) {
        fprintf(stderr, "[DEBUG] Read from 0x4260: banks=[%02X,%02X,%02X,%02X], visible_ram=0x%02X, ram[0x4260]=0x%02X (PC=0x%04X, SP=0x%04X, A=0x%02X, BC=0x%04X, DE=0x%04X, HL=0x%04X, IX=0x%04X, IY=0x%04X)\n",
                pack->bank_reg[0], pack->bank_reg[1], pack->bank_reg[2], pack->bank_reg[3],
                pack->visible_ram[0x4260], pack->ram[0x4260],
                pack->cpu->pc, pack->cpu->sp, pack->cpu->a,
                (pack->cpu->b << 8) | pack->cpu->c,
                (pack->cpu->d << 8) | pack->cpu->e,
                (pack->cpu->h << 8) | pack->cpu->l,
                pack->cpu->ix, pack->cpu->iy);
    }
    
    /* Do not alias 0x0068-0x006F as MMIO UART on memory reads.
     * MUZIX boot ROM executes code in this region; UART is port-mapped. */
    
    /* Check common memory region */
    if (addr >= Z80PACK_COMMON_BASE) {
        return pack->common_mem[addr - Z80PACK_COMMON_BASE];
    }
    
    return pack->visible_ram[addr];
}

static void z80pack_z80_write_byte(void* userdata, uint16_t addr, uint8_t val)
{
    z80pack_t* pack = (z80pack_t*)userdata;
    if (!pack) return;
    
    pack->memory_writes++;
    
    /* Do not alias 0x0068-0x006F as MMIO UART on memory writes.
     * UART for MUZIX is handled through IN/OUT port callbacks. */
    
    /* Check common memory region */
    if (addr >= Z80PACK_COMMON_BASE) {
        pack->common_mem[addr - Z80PACK_COMMON_BASE] = val;
        pack->visible_ram[addr] = val;
        return;
    }
    
    /* Memory-mapped console output for RomWBW (0xFFE0) */
    if (addr == 0xFFE0) {
        if (pack->verbose) {
            fprintf(stderr, "[MEM OUT $FFE0] 0x%02X ('%c')\n", val, (val >= 32 && val < 127) ? val : '.');
        }
        if (pack->serial_send) {
            pack->serial_send(val, pack->serial_userdata);
        } else {
            putchar_unlocked(val);
            fflush(stdout);
        }
        pack->visible_ram[addr] = val;
        return;
    }
    
    {
        uint8_t bank = z80pack_mapped_bank(pack, addr);
        if (bank <= ZETA_ROM_PAGE_MAX) {
            return;
        }
    }

    pack->visible_ram[addr] = val;

    /* --watchw ADDR: report any store to ADDR with the PC that made it. Used
     * to find what overwrites a kernel _DATA cell; the kernel has no way to
     * defend a cell it does not know is being written. */
    if (addr == pack->watch_waddr && pack->watch_whits < 12) {
        pack->watch_whits++;
        printf("[store %04X] <- %02X  from PC=%04X SP=%04X banks=%02X %02X %02X %02X\n",
               addr, val, pack->cpu->pc, pack->cpu->sp,
               pack->bank_reg[0], pack->bank_reg[1],
               pack->bank_reg[2], pack->bank_reg[3]);
        fflush(stdout);
    }

    /* Debug: trace writes to 0x4260 */
    if (addr == 0x4260 && pack->verbose) {
        fprintf(stderr, "[WRITE] 0x4260 = 0x%02X (PC=0x%04X, banks=[%02X,%02X,%02X,%02X])\n",
                val, pack->cpu->pc, pack->bank_reg[0], pack->bank_reg[1], pack->bank_reg[2], pack->bank_reg[3]);
    }

    /* Write to underlying RAM bank for RAM pages (0x20-0x3F). */
    {
        uint8_t bank = z80pack_mapped_bank(pack, addr);
        if (bank >= ZETA_RAM_PAGE_MIN && bank <= ZETA_RAM_PAGE_MAX) {
            uint32_t bank_num = (uint32_t)(bank - ZETA_RAM_PAGE_MIN);
            uint32_t offset = addr % 0x4000;
            uint32_t ram_addr = bank_num * Z80PACK_BANK_SIZE + offset;
            if (ram_addr < Z80PACK_RAM_SIZE) {
                pack->ram[ram_addr] = val;
            }
        }
    }
}

static uint8_t z80pack_z80_port_in(z80* z, uint8_t port)
{
    z80pack_t* pack = (z80pack_t*)z->userdata;
    if (!pack) return 0xFF;
    
    pack->port_reads++;
    return z80pack_port_in(pack, port);
}

static void z80pack_z80_port_out(z80* z, uint8_t port, uint8_t val)
{
    z80pack_t* pack = (z80pack_t*)z->userdata;
    if (!pack) return;
    
    pack->port_writes++;
    z80pack_port_out(pack, port, val);
}

/* ============================================================================
 * Port I/O
 * ============================================================================ */

uint8_t z80pack_port_in(z80pack_t* pack, uint8_t port)
{
    pack->port_reads++;

    /* DS1302 RTC on $70. Standard three-wire, one port: bit 7 DATA,
     * bit 6 CLK, bit 5 RD, bit 4 CE, bit 0 the bit the chip drives back. */
    if (port == Z80PACK_PORT_RTC) {
        return ds1302_read(NULL, port);
    }

    /* RomWBW boot port $00 */
    if (port == 0x00 && !pack->strict_zeta) {
        return 0x40;  /* Boot compatibility */
    }

    /* Bank controller readback (ZETA SBC V2).
     * In strict mode these selectors are treated as write-only to match board
     * behavior and expose firmware that incorrectly depends on readback.
     */
    if (port >= Z80PACK_PORT_BANK0 && port <= Z80PACK_PORT_BANK3) {
        if (pack->strict_zeta) {
            return 0xFF;
        }
        return pack->bank_reg[port - Z80PACK_PORT_BANK0];
    }
    if (port == Z80PACK_PORT_MPGENA) {
        if (pack->strict_zeta) {
            return 0xFF;
        }
        return pack->mpgena;
    }
    
    /* Zeta CTC ports */
    if (port >= Z80PACK_PORT_CTC0 && port <= Z80PACK_PORT_CTC3) {
        uint8_t ch = (uint8_t)(port - Z80PACK_PORT_CTC0);
        /* Return current down-counter approximation from elapsed cycles.
         *
         * The armed test is ctc_cycles_per_tick ALONE, and the time constant is
         * not part of it.  A time constant of 256 - written as the byte 0, which
         * is the only way to express 256 in eight bits, and is what channel 3
         * uses for the load accounting's free-running counter - is stored as 0,
         * because ctc_time_constant[] is a uint8_t.  Testing that field too
         * therefore reports a perfectly armed channel as unarmed and reads 0
         * forever, which is exactly what it did: the kernel measured every awake
         * gap as zero and reported 0.00 while the emulator independently measured
         * the machine at 9.8% busy.
         *
         * ctc_cycles_per_tick already carries the time constant - it is
         * prescaler * tc, with tc computed as 256 for a written 0 - so it is the
         * whole of what "armed" means here. */
        if (pack->ctc_cycles_per_tick[ch] == 0) {
            return 0x00;
        }
        {
            uint64_t elapsed = pack->total_cycles - pack->ctc_last_fire_cycles[ch];
            uint64_t rem = pack->ctc_cycles_per_tick[ch] - (elapsed % pack->ctc_cycles_per_tick[ch]);
            uint64_t div = rem / (uint64_t)(pack->ctc_control[ch] & 0x20 ? 256 : 16);
            if (div == 0 || div > 255) {
                div = pack->ctc_time_constant[ch];
            }
            return (uint8_t)div;
        }
    }

    /* Zeta 8255 PPI ports */
    if (port == Z80PACK_PORT_PPI_A) {
        return pack->ppi_a_input ? 0xFF : pack->ppi_port_a;
    }
    if (port == Z80PACK_PORT_PPI_B) {
        return pack->ppi_b_input ? 0xFF : pack->ppi_port_b;
    }
    if (port == Z80PACK_PORT_PPI_C) {
        uint8_t v = pack->ppi_port_c;
        if (pack->ppi_c_upper_input) v |= 0xF0;
        if (pack->ppi_c_lower_input) v |= 0x0F;
        return v;
    }
    if (port == Z80PACK_PORT_PPI_CTL) {
        return pack->ppi_control;
    }

    /* ZETA2 UART ports $68-$6F */
    if (port >= Z80PACK_PORT_UART_ZETA_BASE && port < Z80PACK_PORT_UART_ZETA_BASE + Z80PACK_PORT_UART_ZETA_OFFSET) {
        uint8_t offset = port - Z80PACK_PORT_UART_ZETA_BASE;
        
        switch (offset) {
            case 0:  /* RBR */
                if ((pack->uart_lcr & 0x80) != 0) {
                    return pack->uart_dll;
                }
                if (pack->serial_available && pack->serial_available(pack->serial_userdata)) {
                    return pack->serial_recv ? pack->serial_recv(pack->serial_userdata) : 0x00;
                }
                return 0x00;
            case 1:  /* IER */
                if ((pack->uart_lcr & 0x80) != 0) {
                    return pack->uart_dlh;
                }
                return pack->uart_ier;
            case 2:  /* IIR */
                return 0x01;
            case 3:  /* LCR */
                return pack->uart_lcr;
            case 4:  /* MCR */
                return pack->uart_mcr;
            case 5:  /* LSR */
                return (pack->serial_available && pack->serial_available(pack->serial_userdata)) ? 0x61 : 0x60;
            case 6:  /* MSR */
                return 0x30;
            case 7:  /* SCR */
                return pack->uart_scr;
            default:
                return 0x00;
        }
    }
    
    /* RomWBW UART port $70 */
    if (port == Z80PACK_PORT_UART_RWB) {
        if (pack->system == SYSTEM_MUZIX) {
            uint8_t data_in = 0;
            if (pack->rtc_ce_high && pack->rtc_read_mode && (pack->rtc_latch & Z80PACK_DSRTC_RD)) {
                data_in = (uint8_t)(pack->rtc_shift_out >> pack->rtc_shift_bits) & 0x01U;
            }
            return (uint8_t)(pack->rtc_latch | (data_in ? Z80PACK_DSRTC_IN : 0x00));
        }
        return 0x02;  /* TX ready */
    }
    
    /* TTY ports */
    if (port == Z80PACK_PORT_TTY1_STAT || port == Z80PACK_PORT_TTY2_STAT ||
        port == Z80PACK_PORT_TTY3_STAT || port == Z80PACK_PORT_TTY4_STAT) {
        int tty = 0;
        if (port == Z80PACK_PORT_TTY2_STAT) tty = 1;
        if (port == Z80PACK_PORT_TTY3_STAT) tty = 2;
        if (port == Z80PACK_PORT_TTY4_STAT) tty = 3;
        
        if (pack->serial_available && pack->serial_available(pack->serial_userdata)) {
            pack->tty_status[tty] |= TTY_STAT_RX_READY;
        }
        
        return pack->tty_status[tty];
    }
    
    /* FDC status port */
    if (port == Z80PACK_PORT_FDC_STAT) {
        uint8_t status = pack->fdc_status;
        if (pack->fdc_drq) status |= FDC_STAT_DRQ;
        if (pack->fdc_busy) status |= FDC_STAT_BUSY;
        return status;
    }
    
    return 0xFF;
}

void z80pack_port_out(z80pack_t* pack, uint8_t port, uint8_t val)
{
    pack->port_writes++;

    /* DS1302 RTC on $70. */
    if (port == Z80PACK_PORT_RTC) {
        ds1302_write(NULL, port, val);
        return;
    }

    /* Zeta CTC ports */
    if (port >= Z80PACK_PORT_CTC0 && port <= Z80PACK_PORT_CTC3) {
        uint8_t ch = (uint8_t)(port - Z80PACK_PORT_CTC0);

        /* A byte is a time constant if the previous control word ASKED for one (its
         * bit 2), and otherwise bit 0 decides: 1 is a control word, 0 is a time
         * constant on channels 1-3 and the vector on channel 0.  Both rules are
         * needed and either alone is wrong.
         *
         * The bit-2 rule alone breaks the tick: a control word with bit 2 clear
         * still takes a following time constant on channels 1-3 when bit 0 is
         * clear. With no bit-0 test that byte is neither a control word nor
         * anything else and is dropped, so the channel never gets its constant.
         *
         * The bit-0 rule alone breaks the UART: channel 2 is programmed 0xD7, whose
         * bit 2 IS set, and its time constant is 1 - whose bit 0 is set. Read by
         * bit 0 that is a second control word, and 0x01 has bit 7 clear, so the
         * channel's interrupt enable is switched off by the very byte that was
         * supposed to arm it.  The UART then never raises an interrupt no matter
         * what the kernel does, and the symptom is a shell that sits at its prompt
         * ignoring the keyboard while the tick keeps running.
         *
         * So the byte in hand is a time constant if either the preceding control
         * word asked for one or bit 0 is clear, and a control word otherwise.
         */
        if (pack->ctc_waiting_constant[ch]) {
            pack->ctc_time_constant[ch] = (val == 0) ? 0 : val;
            pack->ctc_waiting_constant[ch] = false;
        } else if ((val & 0x01) != 0) {
            /* Control word */
            pack->ctc_control[ch] = val;
            pack->ctc_irq_enabled[ch] = (val & 0x80) ? true : false;
            pack->ctc_waiting_constant[ch] = (val & 0x04) ? true : false;
        } else if (ch == 0) {
            /* Channel 0: the daisy-chain vector base, VVvvv000. */
            pack->ctc_vector_base = val;
        } else {
            /* Time constant on channels 1-3, arriving because bit 0 is clear. */
            pack->ctc_time_constant[ch] = (val == 0) ? 0 : val;
        }

        /* Every channel's period depends on the time constants of other
         * channels - channel 1 counts what channel 0 produced - so none of them
         * can be worked out in isolation here.  One place, after every write. */
        z80pack_ctc_recompute(pack);
        return;
    }

    /* Zeta 8255 PPI ports */
    if (port == Z80PACK_PORT_PPI_A) {
        if (!pack->ppi_a_input) pack->ppi_port_a = val;
        return;
    }
    if (port == Z80PACK_PORT_PPI_B) {
        if (!pack->ppi_b_input) pack->ppi_port_b = val;
        return;
    }
    if (port == Z80PACK_PORT_PPI_C) {
        uint8_t keep = pack->ppi_port_c;
        if (!pack->ppi_c_upper_input) keep = (uint8_t)((keep & 0x0F) | (val & 0xF0));
        if (!pack->ppi_c_lower_input) keep = (uint8_t)((keep & 0xF0) | (val & 0x0F));
        pack->ppi_port_c = keep;
        return;
    }
    if (port == Z80PACK_PORT_PPI_CTL) {
        pack->ppi_control = val;
        if (val & 0x80) {
            pack->ppi_a_input = (val & 0x10) ? true : false;
            pack->ppi_b_input = (val & 0x02) ? true : false;
            pack->ppi_c_upper_input = (val & 0x08) ? true : false;
            pack->ppi_c_lower_input = (val & 0x01) ? true : false;
        } else {
            uint8_t bit = (uint8_t)((val >> 1) & 0x07);
            uint8_t mask = (uint8_t)(1U << bit);
            if (val & 0x01) {
                pack->ppi_port_c |= mask;
            } else {
                pack->ppi_port_c &= (uint8_t)~mask;
            }
        }
        return;
    }

    /* ZETA2 UART ports $68-$6F */
    if (port >= Z80PACK_PORT_UART_ZETA_BASE && port < Z80PACK_PORT_UART_ZETA_BASE + Z80PACK_PORT_UART_ZETA_OFFSET) {
        uint8_t offset = port - Z80PACK_PORT_UART_ZETA_BASE;
        
        if (offset == 0) {  /* THR or DLL */
            if ((pack->uart_lcr & 0x80) != 0) {
                pack->uart_dll = val;
            } else {
                if (pack->serial_send) {
                    pack->serial_send(val, pack->serial_userdata);
                } else {
                    putchar_unlocked(val);
                }
            }
            return;
        }
        if (offset == 1) { /* IER or DLH */
            if ((pack->uart_lcr & 0x80) != 0) {
                pack->uart_dlh = val;
            } else {
                pack->uart_ier = val;
            }
            return;
        }
        if (offset == 2) {
            pack->uart_fcr = val;
            return;
        }
        if (offset == 3) {
            pack->uart_lcr = val;
            return;
        }
        if (offset == 4) {
            pack->uart_mcr = val;
            return;
        }
        if (offset == 7) {
            pack->uart_scr = val;
            return;
        }
        return;
    }
    
    /* RomWBW UART port $70 */
    if (port == Z80PACK_PORT_UART_RWB) {
        if (pack->system == SYSTEM_MUZIX) {
            z80pack_rtc_latch_write(pack, val);
        } else if (pack->serial_send) {
            pack->serial_send(val, pack->serial_userdata);
        } else {
            putchar_unlocked(val);
        }
        return;
    }
    
    /* TTY data ports */
    if (port == Z80PACK_PORT_TTY1_DATA || port == Z80PACK_PORT_TTY2_DATA ||
        port == Z80PACK_PORT_TTY3_DATA || port == Z80PACK_PORT_TTY4_DATA) {
        if (pack->serial_send) {
            pack->serial_send(val, pack->serial_userdata);
        } else {
            putchar_unlocked(val);
        }
        return;
    }
    
    /* Bank control ports */
    if (port >= Z80PACK_PORT_BANK0 && port <= Z80PACK_PORT_BANK3) {
        int bank_idx = port - Z80PACK_PORT_BANK0;
        if (pack->verbose) {
            fprintf(stderr, "[PORT OUT $%02X] Bank %d = 0x%02X (PC=0x%04X)\n", port, bank_idx, val, pack->cpu->pc);
        }
        pack->bank_reg[bank_idx] = val;
        z80pack_update_bank(pack, port);
        return;
    }
    if (port == Z80PACK_PORT_MPGENA) {
        pack->mpgena = val;
        pack->paging_enabled = (val & 0x01) ? true : false;
        z80pack_update_visible_ram(pack);
        return;
    }
    
    /* FDC ports */
    switch (port) {
        case Z80PACK_PORT_FDC_DRIVE:
            if (val < Z80PACK_MAX_DRIVES) {
                pack->fdc_drive = val;
                pack->fdc_status = FDC_STAT_READY;
                if (pack->disk[val].image_data) {
                    pack->fdc_status |= FDC_STAT_TRK0;
                }
            }
            break;
            
        case Z80PACK_PORT_FDC_TRACK:
            pack->fdc_track = val;
            break;
            
        case Z80PACK_PORT_FDC_SECTOR:
            pack->fdc_sector = val;
            break;
            
        case Z80PACK_PORT_FDC_CMD:
            z80pack_fdc_execute(pack, val);
            break;
            
        case Z80PACK_PORT_FDC_DMAL:
            pack->fdc_dma = (pack->fdc_dma & 0xFF00) | val;
            break;
            
        case Z80PACK_PORT_FDC_DMAH:
            pack->fdc_dma = (pack->fdc_dma & 0x00FF) | ((uint16_t)val << 8);
            break;
            
        case Z80PACK_PORT_FDC_SECTH:
            pack->fdc_sector = (pack->fdc_sector & 0x00FF) | ((uint16_t)(val & 0x0F) << 8);
            break;
    }
}

/* ============================================================================
 * FDC Operations
 * ============================================================================ */

static void z80pack_fdc_execute(z80pack_t* pack, uint8_t cmd)
{
    z80pack_disk_t* disk = &pack->disk[pack->fdc_drive];
    
    pack->fdc_command = cmd;
    pack->fdc_busy = true;
    pack->fdc_drq = false;
    
    switch (cmd & 0x03) {
        case FDC_CMD_READ:
            if (!disk->image_data) {
                pack->fdc_status |= FDC_STAT_BUSERR;
                pack->fdc_busy = false;
                return;
            }
            z80pack_fdc_start_read(pack);
            break;
            
        case FDC_CMD_WRITE:
            if (!disk->image_data) {
                pack->fdc_status |= FDC_STAT_BUSERR;
                pack->fdc_busy = false;
                return;
            }
            z80pack_fdc_start_write(pack);
            break;
            
        case FDC_CMD_RESTORE:
            pack->fdc_track = 0;
            pack->fdc_status = FDC_STAT_READY | FDC_STAT_TRK0;
            pack->fdc_busy = false;
            break;
            
        default:
            pack->fdc_status |= FDC_STAT_BUSERR;
            pack->fdc_busy = false;
    }
}

static void z80pack_fdc_start_read(z80pack_t* pack)
{
    z80pack_disk_t* disk = &pack->disk[pack->fdc_drive];
    if (!disk->image_data) return;
    
    uint32_t offset = (uint32_t)pack->fdc_track * disk->sectors_per_track + pack->fdc_sector;
    offset *= disk->sector_size;
    
    if (offset >= disk->image_size) {
        pack->fdc_status |= FDC_STAT_BUSERR;
        pack->fdc_busy = false;
        return;
    }
    
    pack->fdc_transfer_ptr = (uint8_t*)disk->image_data + offset;
    pack->fdc_transfer_remaining = disk->sector_size;
    pack->fdc_drq = true;
}

static void z80pack_fdc_start_write(z80pack_t* pack)
{
    z80pack_disk_t* disk = &pack->disk[pack->fdc_drive];
    if (!disk->image_data) return;
    
    uint32_t offset = (uint32_t)pack->fdc_track * disk->sectors_per_track + pack->fdc_sector;
    offset *= disk->sector_size;
    
    if (offset >= disk->image_size) {
        pack->fdc_status |= FDC_STAT_BUSERR;
        pack->fdc_busy = false;
        return;
    }
    
    pack->fdc_transfer_ptr = (uint8_t*)disk->image_data + offset;
    pack->fdc_transfer_remaining = disk->sector_size;
    pack->fdc_drq = true;
}

static void z80pack_fdc_transfer(z80pack_t* pack, bool is_read)
{
    if (!pack->fdc_drq || !pack->fdc_transfer_ptr) return;
    
    z80pack_disk_t* disk = &pack->disk[pack->fdc_drive];
    
    while (pack->fdc_transfer_remaining > 0) {
        if (is_read) {
            pack->visible_ram[pack->fdc_dma] = *pack->fdc_transfer_ptr;
        } else {
            *pack->fdc_transfer_ptr = pack->visible_ram[pack->fdc_dma];
        }
        
        pack->fdc_transfer_ptr++;
        pack->fdc_dma++;
        pack->fdc_transfer_remaining--;
    }
    
    if (pack->fdc_transfer_remaining == 0) {
        pack->fdc_sector++;
        if (pack->fdc_sector >= disk->sectors_per_track) {
            pack->fdc_sector = 0;
            pack->fdc_track++;
            if (pack->fdc_track >= disk->tracks) {
                pack->fdc_track = disk->tracks - 1;
            }
        }
        
        pack->fdc_drq = false;
        pack->fdc_busy = false;
    }
}

/* ============================================================================
 * Memory Banking
 * ============================================================================ */

static void z80pack_update_visible_ram_region(z80pack_t* pack, int region)
{
    uint8_t bank;
    uint16_t region_start = (uint16_t)(region * 0x4000);

    if (pack->strict_zeta && !pack->paging_enabled) {
        bank = (uint8_t)region;
    } else {
        bank = pack->bank_reg[region];
    }

    if (bank <= ZETA_ROM_PAGE_MAX) {
        uint32_t rom_offset = (uint32_t)bank * Z80PACK_BANK_SIZE;
        if (rom_offset + 0x4000 <= Z80PACK_ROM_SIZE) {
            memcpy(pack->visible_ram + region_start, pack->rom + rom_offset, 0x4000);
        } else {
            memcpy(pack->visible_ram + region_start, pack->rom + rom_offset,
                   Z80PACK_ROM_SIZE - rom_offset);
            memset(pack->visible_ram + region_start + (Z80PACK_ROM_SIZE - rom_offset),
                   0xFF, 0x4000 - (Z80PACK_ROM_SIZE - rom_offset));
        }
    } else if (bank >= ZETA_RAM_PAGE_MIN && bank <= ZETA_RAM_PAGE_MAX) {
        uint32_t ram_offset = (uint32_t)(bank - ZETA_RAM_PAGE_MIN) * Z80PACK_BANK_SIZE;
        if (ram_offset + 0x4000 <= Z80PACK_RAM_SIZE) {
            memcpy(pack->visible_ram + region_start, pack->ram + ram_offset, 0x4000);
        } else {
            memcpy(pack->visible_ram + region_start, pack->ram + ram_offset,
                   Z80PACK_RAM_SIZE - ram_offset);
            memset(pack->visible_ram + region_start + (Z80PACK_RAM_SIZE - ram_offset),
                   0xFF, 0x4000 - (Z80PACK_RAM_SIZE - ram_offset));
        }
    } else {
        memset(pack->visible_ram + region_start, 0xFF, 0x4000);
    }
}

static void z80pack_update_visible_ram(z80pack_t* pack)
{
    uint8_t old_vis_4260 = pack->visible_ram[0x4260];

    for (int region = 0; region < 4; region++) {
        z80pack_update_visible_ram_region(pack, region);
    }

    memcpy(pack->visible_ram + Z80PACK_COMMON_BASE, pack->common_mem, Z80PACK_COMMON_SIZE);

    if (pack->verbose && old_vis_4260 != pack->visible_ram[0x4260]) {
        fprintf(stderr, "[UPDATE_VIS] 0x4260: %02X -> %02X (banks=[%02X,%02X,%02X,%02X], ram[0x4260]=0x%02X)\n",
                old_vis_4260, pack->visible_ram[0x4260],
                pack->bank_reg[0], pack->bank_reg[1], pack->bank_reg[2], pack->bank_reg[3],
                pack->ram[0x4260]);
    }
}

static void z80pack_update_bank(z80pack_t* pack, uint8_t port)
{
    if (port >= Z80PACK_PORT_BANK0 && port <= Z80PACK_PORT_BANK3) {
        z80pack_update_visible_ram_region(pack, port - Z80PACK_PORT_BANK0);
        memcpy(pack->visible_ram + Z80PACK_COMMON_BASE, pack->common_mem, Z80PACK_COMMON_SIZE);
    }
}

static uint8_t z80pack_mapped_bank(z80pack_t* pack, uint16_t addr)
{
    uint8_t region = (uint8_t)(addr >> 14);
    if (pack->strict_zeta && !pack->paging_enabled) {
        return region;
    }
    return pack->bank_reg[region];
}

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================ */

z80pack_t* z80pack_create(void)
{
    z80pack_t* pack = (z80pack_t*)calloc(1, sizeof(z80pack_t));
    if (!pack) return NULL;
    
    pack->ram = (uint8_t*)calloc(Z80PACK_RAM_SIZE, 1);
    if (!pack->ram) {
        free(pack);
        return NULL;
    }
    
    pack->rom = (uint8_t*)calloc(Z80PACK_ROM_SIZE, 1);
    if (!pack->rom) {
        free(pack->ram);
        free(pack);
        return NULL;
    }
    
    pack->cpu = (z80*)calloc(1, sizeof(z80));
    if (!pack->cpu) {
        free(pack->rom);
        free(pack->ram);
        free(pack);
        return NULL;
    }
    
    z80_init(pack->cpu);
    pack->cpu->read_byte = z80pack_z80_read_byte;
    pack->cpu->write_byte = z80pack_z80_write_byte;
    pack->cpu->port_in = z80pack_z80_port_in;
    pack->cpu->port_out = z80pack_z80_port_out;
    pack->cpu->userdata = pack;
    
    pack->bank_reg[0] = 0x00;
    pack->bank_reg[1] = 0x00;
    pack->bank_reg[2] = 0x00;
    pack->bank_reg[3] = 0x00;
    pack->paging_enabled = false;
    pack->strict_zeta = false;
    pack->watch_pc = 0xFFFFu;
    pack->watch_hits = 0;
    pack->watch_waddr = 0xFFFFu;
    pack->watch_whits = 0;
    pack->trace_ring = NULL;
    pack->trace_len = 0;
    pack->trace_pos = 0;
    pack->trace_total = 0;
    
    pack->fdc_status = FDC_STAT_READY | FDC_STAT_TRK0;
    
    for (int i = 0; i < 4; i++) {
        pack->tty_status[i] = TTY_STAT_TX_READY | TTY_STAT_TX_EMPTY;
        pack->ctc_control[i] = 0;
        pack->ctc_time_constant[i] = 0;
        pack->ctc_waiting_constant[i] = false;
        pack->ctc_irq_enabled[i] = false;
        pack->ctc_last_fire_cycles[i] = 0;
        pack->ctc_cycles_per_tick[i] = 0;
    }

    pack->ctc_vector_base = 0;
    pack->ctc_uart_int_prev = 0;
    pack->irq_queue_len = 0;
    pack->ppi_port_a = 0;
    pack->ppi_port_b = 0;
    pack->ppi_port_c = 0;
    pack->ppi_control = 0x9B;
    pack->ppi_a_input = true;
    pack->ppi_b_input = true;
    pack->ppi_c_upper_input = true;
    pack->ppi_c_lower_input = true;
    pack->rtc_latch = Z80PACK_DSRTC_RD;
    pack->rtc_ce_high = false;
    pack->rtc_clk_high = false;
    pack->rtc_cmd = 0;
    pack->rtc_cmd_bits = 0;
    pack->rtc_shift_in = 0;
    pack->rtc_shift_out = 0;
    pack->rtc_shift_bits = 0;
    pack->rtc_read_len = 0;
    pack->rtc_read_pos = 0;
    pack->rtc_read_mode = false;
    pack->rtc_write_mode = false;
    pack->uart_lcr = 0x03;
    pack->uart_ier = 0x00;
    pack->uart_dll = 0x01;
    pack->uart_dlh = 0x00;
    pack->uart_mcr = 0x03;
    pack->uart_scr = 0x00;
    pack->uart_fcr = 0x00;
    
    pack->running = true;
    pack->halted = false;
    pack->system = SYSTEM_ROMWBW;
    
    return pack;
}

void z80pack_destroy(z80pack_t* pack)
{
    if (!pack) return;

    if (pack->terminal_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &pack->original_termios);
        pack->terminal_raw_mode = false;
        pack->stdin_ready = false;
    }
    
    for (int i = 0; i < Z80PACK_MAX_DRIVES; i++) {
        z80pack_unmount_disk(pack, i);
    }
    
    if (pack->cpu) free(pack->cpu);
    if (pack->ram) free(pack->ram);
    if (pack->rom) free(pack->rom);
    
    free(pack);
}

void z80pack_reset(z80pack_t* pack)
{
    if (!pack) return;
    
    z80_init(pack->cpu);
    pack->cpu->read_byte = z80pack_z80_read_byte;
    pack->cpu->write_byte = z80pack_z80_write_byte;
    pack->cpu->port_in = z80pack_z80_port_in;
    pack->cpu->port_out = z80pack_z80_port_out;
    pack->cpu->userdata = pack;
    pack->cpu->pc = 0x0000;
    pack->cpu->sp = 0xFFFF;
    
    pack->bank_reg[0] = 0x00;
    pack->bank_reg[1] = 0x00;
    pack->bank_reg[2] = 0x00;
    pack->bank_reg[3] = 0x00;
    pack->mpgena = 0x00;
    pack->paging_enabled = false;
    
    pack->fdc_drive = 0;
    pack->fdc_track = 0;
    pack->fdc_sector = 0;
    pack->fdc_dma = 0;
    pack->fdc_status = FDC_STAT_READY | FDC_STAT_TRK0;
    pack->fdc_busy = false;
    pack->fdc_drq = false;
    
    for (int i = 0; i < 4; i++) {
        pack->tty_rx_head[i] = 0;
        pack->tty_rx_tail[i] = 0;
        pack->tty_tx_head[i] = 0;
        pack->tty_tx_tail[i] = 0;
        pack->tty_status[i] = TTY_STAT_TX_READY | TTY_STAT_TX_EMPTY;
        pack->ctc_control[i] = 0;
        pack->ctc_time_constant[i] = 0;
        pack->ctc_waiting_constant[i] = false;
        pack->ctc_irq_enabled[i] = false;
        pack->ctc_last_fire_cycles[i] = 0;
        pack->ctc_cycles_per_tick[i] = 0;
    }

    pack->ctc_vector_base = 0;
    pack->ctc_uart_int_prev = 0;
    pack->irq_queue_len = 0;
    pack->ppi_port_a = 0;
    pack->ppi_port_b = 0;
    pack->ppi_port_c = 0;
    pack->ppi_control = 0x9B;
    pack->ppi_a_input = true;
    pack->ppi_b_input = true;
    pack->ppi_c_upper_input = true;
    pack->ppi_c_lower_input = true;
    pack->rtc_latch = Z80PACK_DSRTC_RD;
    pack->rtc_ce_high = false;
    pack->rtc_clk_high = false;
    pack->rtc_cmd = 0;
    pack->rtc_cmd_bits = 0;
    pack->rtc_shift_in = 0;
    pack->rtc_shift_out = 0;
    pack->rtc_shift_bits = 0;
    pack->rtc_read_len = 0;
    pack->rtc_read_pos = 0;
    pack->rtc_read_mode = false;
    pack->rtc_write_mode = false;
    pack->uart_lcr = 0x03;
    pack->uart_ier = 0x00;
    pack->uart_dll = 0x01;
    pack->uart_dlh = 0x00;
    pack->uart_mcr = 0x03;
    pack->uart_scr = 0x00;
    pack->uart_fcr = 0x00;
    
    pack->total_cycles = 0;
    pack->instructions = 0;
    pack->memory_reads = 0;
    pack->memory_writes = 0;
    pack->port_reads = 0;
    pack->port_writes = 0;
    
    pack->running = true;
    pack->halted = false;
    
    z80pack_update_visible_ram(pack);
}

/* ============================================================================
 * ROM and Disk Loading
 * ============================================================================ */

int z80pack_load_rom(z80pack_t* pack, const char* filename, uint16_t addr)
{
    if (!pack || !filename) return -1;
    
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open ROM file: %s\n", filename);
        return -1;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    
    if (addr + st.st_size > Z80PACK_ROM_SIZE) {
        fprintf(stderr, "ROM too large for address space\n");
        close(fd);
        return -1;
    }
    
    ssize_t bytes_read = read(fd, pack->rom + addr, st.st_size);
    close(fd);
    
    if (bytes_read != st.st_size) {
        fprintf(stderr, "Short read from ROM file\n");
        return -1;
    }
    
    z80pack_update_visible_ram(pack);
    
    fprintf(stderr, "Loaded ROM: %s (%zd bytes at 0x%04X)\n", filename, bytes_read, addr);
    return 0;
}

int z80pack_mount_disk(z80pack_t* pack, int drive, const char* filename)
{
    if (!pack || drive < 0 || drive >= Z80PACK_MAX_DRIVES) return -1;
    
    z80pack_unmount_disk(pack, drive);
    
    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Cannot open disk: %s\n", filename);
            return -1;
        }
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    
    pack->disk[drive].image_data = (uint8_t*)malloc(st.st_size);
    if (!pack->disk[drive].image_data) {
        fprintf(stderr, "Out of memory loading disk\n");
        close(fd);
        return -1;
    }
    
    ssize_t bytes_read = read(fd, pack->disk[drive].image_data, st.st_size);
    close(fd);
    
    if (bytes_read != st.st_size) {
        free(pack->disk[drive].image_data);
        pack->disk[drive].image_data = NULL;
        return -1;
    }
    
    pack->disk[drive].image_size = st.st_size;
    pack->disk[drive].filename = strdup(filename);
    
    uint32_t sectors = st.st_size / 128;
    if (sectors == 26 * 77) {
        pack->disk[drive].sectors_per_track = 26;
        pack->disk[drive].tracks = 77;
        pack->disk[drive].sector_size = 128;
    } else if (sectors == 52 * 77) {
        pack->disk[drive].sectors_per_track = 52;
        pack->disk[drive].tracks = 77;
        pack->disk[drive].sector_size = 128;
    } else {
        pack->disk[drive].sectors_per_track = 128;
        pack->disk[drive].tracks = sectors / 128;
        pack->disk[drive].sector_size = 128;
    }
    
    fprintf(stderr, "Mounted disk %d: %s (%lld bytes, %d t/s, %d tracks)\n",
            drive, filename, (long long)st.st_size,
            pack->disk[drive].sectors_per_track,
            pack->disk[drive].tracks);
    
    return 0;
}

void z80pack_unmount_disk(z80pack_t* pack, int drive)
{
    if (!pack || drive < 0 || drive >= Z80PACK_MAX_DRIVES) return;
    
    if (pack->disk[drive].image_data) {
        free(pack->disk[drive].image_data);
        pack->disk[drive].image_data = NULL;
    }
    
    if (pack->disk[drive].filename) {
        free((void*)pack->disk[drive].filename);
        pack->disk[drive].filename = NULL;
    }
    
    pack->disk[drive].image_size = 0;
}

/* ============================================================================
 * Serial Configuration
 * ============================================================================ */

void z80pack_set_serial_callbacks(z80pack_t* pack,
    z80pack_serial_send_t send,
    z80pack_serial_recv_t recv,
    z80pack_serial_available_t available,
    void* userdata)
{
    if (!pack) return;
    
    pack->serial_send = send;
    pack->serial_recv = recv;
    pack->serial_available = available;
    pack->serial_userdata = userdata;
}

/* ============================================================================
 * Execution
 * ============================================================================ */

void z80pack_step(z80pack_t* pack)
{
    static uint16_t trace_syscall_pc;
    static uint8_t trace_syscall_pc_ready;
    static uint16_t trace_udata_base;
    static uint8_t trace_udata_base_ready;

    if (!pack || pack->halted) return;
    
    /* Check for HBIOS RST 0x08 instruction */
    if (!pack->strict_zeta && pack->cpu->pc == 0x0008) {
        uint8_t opcode = z80pack_read_byte(pack, 0x0008);
        if (opcode == 0xCF) {  /* RST 0x08 */
            uint16_t result = z80pack_hbios_exec(pack, pack->cpu->a);
            pack->cpu->l = result & 0xFF;
            pack->cpu->h = (result >> 8) & 0xFF;
        }
    }
    
    if (!trace_syscall_pc_ready) {
        uint8_t op = z80pack_read_byte(pack, 0x0028);
        if (op == 0xC3) {
            uint8_t lo = z80pack_read_byte(pack, 0x0029);
            uint8_t hi = z80pack_read_byte(pack, 0x002A);
            trace_syscall_pc = (uint16_t)(((uint16_t)hi << 8) | lo);
        } else {
            trace_syscall_pc = 0;
        }
        trace_syscall_pc_ready = 1;
    }
    if (!trace_udata_base_ready && trace_syscall_pc != 0) {
        /* unix_syscall_entry starts with:
         *   ... ld a,c
         *   ld (udata+U_DATA__U_CALLNO),a
         * Opcode stream around entry:
         *   ... 79 32 <lo> <hi> ...
         */
        uint8_t op = z80pack_read_byte(pack, (uint16_t)(trace_syscall_pc + 0x0A));
        if (op == 0x32) {
            uint8_t lo = z80pack_read_byte(pack, (uint16_t)(trace_syscall_pc + 0x0B));
            uint8_t hi = z80pack_read_byte(pack, (uint16_t)(trace_syscall_pc + 0x0C));
            uint16_t callno_addr = (uint16_t)(((uint16_t)hi << 8) | lo);
            trace_udata_base = (uint16_t)(callno_addr - 7U);
        } else {
            trace_udata_base = 0;
        }
        trace_udata_base_ready = 1;
    }

    if (trace_udata_base != 0 && pack->cpu->pc == 0x02E5) {
    }
    if (trace_udata_base != 0 && pack->cpu->pc == 0x02ED) {
    }

    if (pack->verbose && trace_udata_base != 0 && pack->cpu->pc == 0xFE5B) {
    }

    if (pack->verbose && pack->cpu->pc == 0xFC4B) {
    }
    
    if (pack->cpu->pc == 0x80A8) {
    }
    if (pack->cpu->pc == 0x0A7B && pack->cpu->sp < 0xC000) {
    }

    /* Whether the CPU was stopped going into this instruction.  Sampled BEFORE
     * the step: z80_step() clears `halted` itself when it takes an interrupt, so
     * sampling afterwards counts only the halt steps that nothing woke - which
     * would report a machine that idles perfectly as never having idled. */
    {
        int was_halted = pack->cpu->halted;

        /* Execute one instruction */
        z80_step(pack->cpu);

        if (was_halted && pack->cpu->cyc > 0) {
            pack->halted_cycles += pack->cpu->cyc;
        }
    }

    /* Update statistics */
    pack->total_cycles += pack->cpu->cyc;
    pack->cpu->cyc = 0;  /* Reset cycle counter for next instruction */
    pack->instructions++;

    /* Handle FDC transfer if DRQ is set */
    if (pack->fdc_drq) {
        z80pack_fdc_transfer(pack, true);
    }

    /* Advance CTC timers and assert periodic interrupt requests. */
    z80pack_ctc_tick(pack);

    /* Deliver any vector that could not be raised while another was pending. */
    z80pack_irq_pump(pack);
    
    /* Check for HALT - and a halted CPU is NOT a finished emulation.
     *
     * HALT stops instruction execution until the next enabled interrupt, and
     * z80.c already models exactly that: while halted it executes a NOP, and
     * process_interrupts() clears the flag the moment a CTC tick arrives.  So the
     * state is transient and the CPU keeps going on its own.
     *
     * Copying cpu->halted straight into pack->halted ends the run instead, and
     * z80pack_run()'s loop condition is `!pack->halted`.  For a host that uses HALT
     * as its idle state - which this one now does, in the console wait - the
     * machine therefore appeared to die the first time it waited for a key, and
     * the emulator printed "Emulation halted" where a run should have continued.
     * That is also why the busy figure never moved: every measurement with input
     * already available skipped the wait entirely and never executed a HALT, and
     * every measurement with input delayed reached one and stopped.
     *
     * So the run ends only when the halt cannot be woken - interrupts disabled and
     * nothing pending.  That is the condition emulator/zeta_sbc_v2/emu_exec.c
     * tests, with the comment "Interrupts disabled - remain halted.  This correctly
     * models HALT behavior" - and that file is not in this build's source list, so
     * the condition was simply absent from the copy that is. */
    if (pack->cpu->halted && !pack->cpu->iff1 &&
        !pack->cpu->int_pending && !pack->cpu->nmi_pending) {
        pack->halted = true;
    }
}

void z80pack_run(z80pack_t* pack, uint64_t max_cycles)
{
    if (!pack) return;

    uint64_t target = max_cycles > 0 ? (pack->total_cycles + max_cycles) : UINT64_MAX;

    while (pack->running && !pack->halted && pack->total_cycles < target) {
        if (pack->trace_len > 0) {
            pack->trace_ring[pack->trace_pos] = pack->cpu->pc;
            pack->trace_pos = (pack->trace_pos + 1) % pack->trace_len;
            pack->trace_total++;
        }
        /* --watch ADDR: report the machine state every time execution reaches
         * ADDR.  The kernel and the process each reported a different stack
         * pointer for the same userspace trap, and neither side's arithmetic
         * could be trusted on its own, so the observation has to come from
         * outside both. */
        if (pack->watch_pc != 0xFFFFu && pack->cpu->pc == pack->watch_pc &&
            pack->watch_hits < 200000) {
            const struct z80* const z = pack->cpu;
            pack->watch_hits++;
            printf("[watch %04X] PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X "
                   "IX=%04X IY=%04X banks=%02X %02X %02X %02X\n",
                   pack->watch_pc, z->pc, z->sp,
                   (unsigned)((z->a << 8) | (z->sf << 7) | (z->zf << 6) |
                              (z->yf << 5) | (z->hf << 4) | (z->xf << 3) |
                              (z->pf << 2) | (z->nf << 1) | z->cf),
                   (unsigned)((z->b << 8) | z->c),
                   (unsigned)((z->d << 8) | z->e),
                   (unsigned)((z->h << 8) | z->l),
                   z->ix, z->iy,
                   pack->bank_reg[0], pack->bank_reg[1],
                   pack->bank_reg[2], pack->bank_reg[3]);
            fflush(stdout);
        }
        z80pack_step(pack);
    }
}

void z80pack_run_until_halt(z80pack_t* pack)
{
    if (!pack) return;
    
    while (pack->running && !pack->halted) {
        z80pack_step(pack);
    }
}

/* ============================================================================
 * Status and Debug
 * ============================================================================ */

void z80pack_set_watch(z80pack_t* pack, uint16_t addr)
{
    if (!pack) return;
    pack->watch_pc = addr;
    pack->watch_hits = 0;
}

void z80pack_set_trace(z80pack_t* pack, int n)
{
    if (!pack) return;
    free(pack->trace_ring);
    pack->trace_ring = NULL;
    pack->trace_len = 0;
    pack->trace_pos = 0;
    pack->trace_total = 0;
    if (n > 0) {
        pack->trace_ring = (uint16_t*)calloc((size_t)n, sizeof(uint16_t));
        if (pack->trace_ring) {
            pack->trace_len = n;
        }
    }
}

void z80pack_dump_trace(z80pack_t* pack)
{
    int i;
    if (!pack || pack->trace_len <= 0) return;
    printf("--- last %d PCs of %ld ---\n", pack->trace_len, pack->trace_total);
    for (i = 0; i < pack->trace_len; i++) {
        const int idx = (pack->trace_pos + i) % pack->trace_len;
        if (pack->trace_ring[idx]) {
            printf("%04X ", pack->trace_ring[idx]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
    }
    printf("\n");
}

void z80pack_set_watchw(z80pack_t* pack, uint16_t addr)
{
    if (!pack) return;
    pack->watch_waddr = addr;
    pack->watch_whits = 0;
}

void z80pack_set_verbose(z80pack_t* pack, bool enable)
{
    if (pack) pack->verbose = enable;
}

void z80pack_set_system(z80pack_t* pack, z80pack_system_t system)
{
    if (pack) pack->system = system;
}

void z80pack_set_strict_zeta(z80pack_t* pack, bool enable)
{
    if (!pack) return;
    pack->strict_zeta = enable;
    z80pack_update_visible_ram(pack);
}

void z80pack_set_stdin_input(z80pack_t* pack, bool enable)
{
    if (!pack) return;
    
    pack->enable_stdin_input = enable;
    
    if (enable && !pack->terminal_raw_mode) {
        /* Save current terminal settings */
        if (tcgetattr(STDIN_FILENO, &pack->original_termios) == 0) {
            struct termios raw = pack->original_termios;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                pack->terminal_raw_mode = true;
                pack->stdin_ready = true;
            }
        }
    } else if (!enable && pack->terminal_raw_mode) {
        /* Restore original terminal settings */
        tcsetattr(STDIN_FILENO, TCSANOW, &pack->original_termios);
        pack->terminal_raw_mode = false;
        pack->stdin_ready = false;
    }
}

void z80pack_inject_input_string(z80pack_t* pack, const char* str)
{
    if (!pack || !str) return;
    
    while (*str) {
        size_t next_tail = (pack->input_tail + 1) % sizeof(pack->input_buffer);
        if (next_tail != pack->input_head) {
            pack->input_buffer[pack->input_tail] = *str++;
            pack->input_tail = next_tail;
        } else {
            break; /* Buffer full */
        }
    }
}

uint16_t z80pack_get_pc(z80pack_t* pack)
{
    return pack ? pack->cpu->pc : 0;
}

uint16_t z80pack_get_sp(z80pack_t* pack)
{
    return pack ? pack->cpu->sp : 0;
}

uint64_t z80pack_get_cycles(z80pack_t* pack)
{
    return pack ? pack->total_cycles : 0;
}

void z80pack_print_status(z80pack_t* pack)
{
    if (!pack) return;
    
    fprintf(stderr, "\n=== Z80pack Status ===\n");
    fprintf(stderr, "PC: 0x%04X SP: 0x%04X\n", pack->cpu->pc, pack->cpu->sp);
    fprintf(stderr, "Cycles: %llu Instructions: %u\n",
            (unsigned long long)pack->total_cycles, pack->instructions);
    /* Independent ground truth for the kernel's load figure: how much of the
     * elapsed time the processor was actually stopped.  busy = 1 - halted/total. */
    fprintf(stderr, "Halted: %llu (%.2f%%)  true busy: %.2f%%\n",
            (unsigned long long)pack->halted_cycles,
            pack->total_cycles ? 100.0 * (double)pack->halted_cycles / (double)pack->total_cycles : 0.0,
            pack->total_cycles ? 100.0 * (1.0 - (double)pack->halted_cycles / (double)pack->total_cycles) : 0.0);
    fprintf(stderr, "Memory: R=%u W=%u Port: R=%u W=%u\n",
            pack->memory_reads, pack->memory_writes,
            pack->port_reads, pack->port_writes);
    fprintf(stderr, "FDC: drive=%d track=%d sector=%d\n",
            pack->fdc_drive, pack->fdc_track, pack->fdc_sector);
    fprintf(stderr, "Banks: %02X %02X %02X %02X\n",
            pack->bank_reg[0], pack->bank_reg[1],
            pack->bank_reg[2], pack->bank_reg[3]);
}

/* ============================================================================
 * Memory Access
 * ============================================================================ */

uint8_t z80pack_read_byte(z80pack_t* pack, uint16_t addr)
{
    if (!pack) return 0xFF;
    return z80pack_z80_read_byte(pack, addr);
}

void z80pack_write_byte(z80pack_t* pack, uint16_t addr, uint8_t val)
{
    if (!pack) return;
    z80pack_z80_write_byte(pack, addr, val);
}

uint16_t z80pack_read_word(z80pack_t* pack, uint16_t addr)
{
    if (!pack) return 0;
    uint8_t lo = z80pack_read_byte(pack, addr);
    uint8_t hi = z80pack_read_byte(pack, addr + 1);
    return (uint16_t)hi << 8 | lo;
}

void z80pack_write_word(z80pack_t* pack, uint16_t addr, uint16_t val)
{
    if (!pack) return;
    z80pack_write_byte(pack, addr, val & 0xFF);
    z80pack_write_byte(pack, addr + 1, (val >> 8) & 0xFF);
}
