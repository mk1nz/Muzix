/**
 * Z80pack - ROMWBW HBIOS Emulation
 * 
 * Implements ROMWBW HBIOS function calls for:
 * - Console I/O (CONIN, CONOUT, CONST)
 * - String output (STROUT)
 * - Bank management (SETBNK, GETBNK)
 * 
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "z80pack.h"

/* Get Z80 CPU type for register access */
#include "../z80/z80.h"

/* Console input buffer */
#define CONSOLE_BUF_SIZE    256
static volatile uint8_t console_buffer[CONSOLE_BUF_SIZE];
static volatile size_t console_head = 0;
static volatile size_t console_tail = 0;

/* HBIOS function numbers */
#define HBIOS_CONIN             0x00    /* Console input (blocking) - returns char in A */
#define HBIOS_CONOUT            0x01    /* Console output - character in E */
#define HBIOS_CONST             0x02    /* Console input status - returns char count in A */
#define HBIOS_DIRIOF            0x04    /* Direct console I/O (raw) */
#define HBIOS_DIRIST            0x05    /* Direct console input status */
#define HBIOS_OUTSUB            0x07    /* Output sub (special character) */
#define HBIOS_INSUB             0x08    /* Input sub (special character) */
#define HBIOS_CONST2            0x0B    /* Console input status (alternate) */
#define HBIOS_CONIN2            0x0C    /* Console input (alternate) */
#define HBIOS_CONOUT2           0x0D    /* Console output (alternate) */
#define HBIOS_STROUT            0x09    /* Output string from memory (DE=ptr) */
#define HBIOS_SETBNK            0x18    /* Set bank for DMA operations */
#define HBIOS_GETBNK            0x19    /* Get current bank */
#define HBIOS_VERSION           0x21    /* Get HBIOS version string */

/* HBIOS callback */
static z80pack_hbios_call_t hbios_callback = NULL;
static void* hbios_userdata = NULL;

/* Verbose mode */
static bool hbios_verbose = false;

/**
 * Configure HBIOS callback
 */
void z80pack_configure_hbios(z80pack_t* pack,
    z80pack_hbios_call_t callback,
    void* userdata)
{
    (void)pack;
    hbios_callback = callback;
    hbios_userdata = userdata;
}

/**
 * Set HBIOS verbose mode
 */
void z80pack_set_hbios_verbose(bool enable)
{
    hbios_verbose = enable;
}

/**
 * Add string to console input buffer
 */
void z80pack_console_input_string(const char* str)
{
    while (*str) {
        size_t next = (console_head + 1) % CONSOLE_BUF_SIZE;
        if (next != console_tail) {
            console_buffer[console_head] = (uint8_t)(*str++);
            console_head = next;
        } else {
            /* Buffer full, discard oldest */
            console_tail = (console_tail + 1) % CONSOLE_BUF_SIZE;
            console_buffer[console_head] = (uint8_t)(*str++);
            console_head = next;
        }
    }
}

/**
 * Clear console input buffer
 */
void z80pack_console_clear(void)
{
    console_head = 0;
    console_tail = 0;
}

/**
 * Check if console data is available
 */
static bool console_available(void)
{
    return console_head != console_tail;
}

/**
 * Get character from console (non-blocking)
 */
static bool console_get_char(uint8_t* ch)
{
    if (console_head == console_tail) {
        return false;
    }
    *ch = console_buffer[console_tail];
    console_tail = (console_tail + 1) % CONSOLE_BUF_SIZE;
    return true;
}

/**
 * Execute HBIOS function
 * 
 * HBIOS is called via RST 0x08 instruction
 * Function number is passed in register A
 * Returns result in A or HL
 */
uint16_t z80pack_hbios_exec(z80pack_t* pack, uint8_t func)
{
    /* If custom callback is set, use it */
    if (hbios_callback) {
        return hbios_callback(func, pack, hbios_userdata);
    }
    
    /* Default HBIOS implementation */
    switch (func) {
        case HBIOS_CONOUT:
        case HBIOS_CONOUT2: {
            /* Console output - character in E */
            uint8_t ch = pack->cpu->e;
            if (pack->serial_send) {
                pack->serial_send(ch, pack->serial_userdata);
            } else {
                putchar_unlocked(ch);
            }
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS CONOUT] 0x%02X ('%c')\n", ch, isprint(ch) ? ch : '.');
            }
            return 0;  /* Success */
        }
        
        case HBIOS_CONST:
        case HBIOS_CONST2: {
            /* Console input status - return 1 if character available */
            uint8_t available = console_available() ? 1 : 0;
            pack->cpu->a = available;
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS CONST] -> 0x%02X (%s)\n", available,
                        available ? "ready" : "not ready");
            }
            return available;  /* Return in A */
        }
        
        case HBIOS_CONIN:
        case HBIOS_CONIN2: {
            /* Console input (blocking) - wait for character */
            uint8_t ch = 0;
            
            /* First check buffer */
            if (!console_get_char(&ch)) {
                /* Check serial callback */
                if (pack->serial_recv) {
                    ch = pack->serial_recv(pack->serial_userdata);
                } else {
                    /* No input available - return 0 to prevent hanging */
                    ch = 0;
                }
            }
            
            pack->cpu->a = ch;
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS CONIN] -> 0x%02X ('%c')\n", ch, isprint(ch) ? ch : '.');
            }
            return ch;  /* Return character in A */
        }
        
        case HBIOS_STROUT: {
            /* Output string from memory (DE=ptr) */
            uint16_t ptr = ((uint16_t)pack->cpu->d << 8) | pack->cpu->e;
            uint8_t ch;
            
            while ((ch = z80pack_read_byte(pack, ptr)) != 0) {
                if (pack->serial_send) {
                    pack->serial_send(ch, pack->serial_userdata);
                } else {
                    putchar_unlocked(ch);
                }
                ptr++;
            }
            
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS STROUT] from 0x%04X\n", ((uint16_t)pack->cpu->d << 8) | pack->cpu->e);
            }
            return 0;
        }
        
        case HBIOS_SETBNK: {
            /* Set bank for DMA operations - E contains bank number */
            /* For now, just return success */
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS SETBNK] bank=%d\n", pack->cpu->e);
            }
            return 0;
        }
        
        case HBIOS_GETBNK: {
            /* Get current bank - return 0 (common bank) */
            pack->cpu->a = 0;
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS GETBNK] -> 0x00\n");
            }
            return 0;
        }
        
        case HBIOS_VERSION: {
            /* Return pointer to version string */
            static const char* version = "Z80pack HBIOS 1.0";
            (void)version;
            pack->cpu->d = 0x00;  /* Point to internal buffer */
            pack->cpu->e = 0x00;
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS VERSION]\n");
            }
            return 0;
        }
        
        default:
            if (hbios_verbose) {
                fprintf(stderr, "[HBIOS 0x%02X] unimplemented\n", func);
            }
            return 0;  /* Return success for unimplemented functions */
    }
}

/**
 * Handle HBIOS RST 0x08 instruction
 * 
 * The Z80 executes RST 0x08 which vectors to HBIOS
 * Function number is in register A
 */
void z80pack_handle_hbios_rst(z80pack_t* pack)
{
    uint8_t func = pack->cpu->a;
    uint16_t result = z80pack_hbios_exec(pack, func);
    
    /* Return value in HL */
    pack->cpu->l = result & 0xFF;
    pack->cpu->h = (result >> 8) & 0xFF;
}

/**
 * Check if address is HBIOS RST 0x08 vector
 * HBIOS uses RST 0x08 instruction at address 0x0008
 */
bool z80pack_is_hbios_rst(z80pack_t* pack, uint16_t addr)
{
    /* RST 0x08 instruction is CF (11001111) at 0x0008 */
    if (addr == 0x0008) {
        uint8_t instr = z80pack_read_byte(pack, 0x0008);
        return instr == 0xCF;
    }
    return false;
}

/**
 * Handle memory-mapped HBIOS call
 * Some ROMs use LD (address),A to call HBIOS
 */
bool z80pack_handle_hbios_mem(z80pack_t* pack, uint16_t addr)
{
    (void)pack;
    /* Check for known HBIOS memory-mapped calls */
    switch (addr) {
        case 0xFFE0:
            /* Memory-mapped console output (RomWBW style) */
            /* This is handled in z80_write_byte */
            return true;
            
        default:
            return false;
    }
}

/**
 * Get console buffer status
 */
size_t z80pack_console_buffer_size(void)
{
    if (console_head >= console_tail) {
        return console_head - console_tail;
    }
    return CONSOLE_BUF_SIZE - console_tail + console_head;
}

/**
 * Inject key into console buffer
 */
void z80pack_console_inject_key(uint8_t key)
{
    size_t next = (console_head + 1) % CONSOLE_BUF_SIZE;
    if (next != console_tail) {
        console_buffer[console_head] = key;
        console_head = next;
    }
}

/**
 * Inject string as key presses
 */
void z80pack_console_inject_string(const char* str)
{
    z80pack_console_input_string(str);
}
