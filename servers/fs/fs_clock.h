/*
 * fs_clock.h - the FS server's own counter; see fs_clock.c.
 */
#ifndef MUZIX_FS_CLOCK_H
#define MUZIX_FS_CLOCK_H

#include <stdint.h>

/* Advances the FS server's progress counter.  Called once per dispatched
 * request from fs_server.c's fs_server_handle() - the loop itself is
 * lib/server.c's, shared with the other servers, so the tick is taken at the
 * one point every delivered message passes through. */
void muzix_clock_tick(void);

/* The same counter, for the TTY's and the UART driver's self-timeouts.  It is
 * not a wall clock and must not be used where one is required. */
uint16_t muzix_tick_now(void);

#endif