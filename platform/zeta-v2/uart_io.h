#ifndef MUZIX_ZETA_UART_H
#define MUZIX_ZETA_UART_H

#include <stdint.h>

void muzix_zeta_uart_init(void);

/* These three are implemented in assembly (uart_io.s) using the sdcccall(1)
 * register convention: param1 in A, param2 in DE, and the callee's ret pops
 * only the return address. Without sdcccall(1) here, SDCC passes both
 * arguments on the stack and every single call leaves the stack four bytes
 * too high, which silently walks SP out of the stack and into _DATA.
 *
 * __sdcccall(1) is SDCC's calling-convention attribute, and SDCC's front end
 * is the only thing that understands the keyword - a host compiler reads
 * `__sdcccall(1)` as the start of a function body and rejects the header
 * outright, which made every module including this one unbuildable off-target.
 * The attribute is a register convention recorded as an option in the object,
 * not a link-time dependency: it introduces no undefined symbol, so dropping it
 * when the compiler is not SDCC changes no Z80 behaviour whatsoever.  The
 * declaration below is byte-identical under __SDCC; there is only one copy of
 * it, and the attribute is the whole difference. */
#ifdef __SDCC
#define MUZIX_ZETA_SDCCCALL1 __sdcccall(1)
#else
#define MUZIX_ZETA_SDCCCALL1
#endif

void muzix_zeta_uart_send(uint8_t byte, void *userdata) MUZIX_ZETA_SDCCCALL1;
uint8_t muzix_zeta_uart_recv(void *userdata) MUZIX_ZETA_SDCCCALL1;
uint8_t muzix_zeta_uart_available(void *userdata) MUZIX_ZETA_SDCCCALL1;

void muzix_zeta_enter_userspace(void);

#endif