#ifndef LS_TIME_H
#define LS_TIME_H

#include <stdint.h>

/*
 * ls_time.h - epoch-to-text conversion for the `ls` app's long format.
 *
 * The epoch is a Unix-style count of seconds, but relative to
 * 2000-01-01 00:00:00 (the base year muzix_fs timestamps use, not 1970).
 *
 * Zero is treated as the "no timestamp" sentinel and renders as
 * "--.--.---- --:--:--".
 */
void format_epoch(uint32_t epoch, char *buf);

#endif /* LS_TIME_H */
