#ifndef MUZIX_LIBC_H
#define MUZIX_LIBC_H

#include <stdint.h>
#include <stddef.h>

#include "proc_info.h"
#include "tty_ioctl.h"

/* Standard types */
typedef int pid_t;      /* Process ID */
typedef int fd_t;       /* File descriptor */
typedef int mode_t;     /* File mode */
typedef int ssize_t;    /* Signed size */

struct dirent {
	uint16_t inode;
	char name[15];
};

/*
 * Calendar time, as read from and written to the on-board DS1302.
 *
 * Binary, not BCD: the chip keeps BCD in its registers and the driver converts
 * once, on the way in and out, so nothing above the driver has to know.
 *
 * year is 0-99 with no century of its own, because the DS1302 has nowhere
 * to put one.  Which century it is read in is MUZIX_RTC_YEAR_BASE below.
 *
 * weekday is 1-7 with Sunday = 1, which is how the chip counts it.  It is
 * carried here so that `date` can set the chip's day-of-week register; a caller
 * that does not know it passes 0 and the register is left alone.
 */
/*
 * The century the chip's two-digit year is read in.
 *
 * A DS1302 has a two-digit year register and nowhere to put a century, so a
 * bare 26 could be 1926 or 2026 and the chip cannot say.  Nothing in the
 * system can say either: there is no other clock, and the kernel's `tick` is a
 * scheduler-pass counter rather than a time of day.  The century is therefore a
 * stated assumption rather than a derived fact, and 2000 is the one to state:
 * it is the century the hardware this runs on is in, it makes `date` round-trip
 * what it just set, and it is one #define away from being changed on a board
 * that should read the other one.
 *
 * The consequence is that the window is 2000-2099 and that time()'s seconds
 * are offset by this many years from a true 1970 epoch.  The offset is imposed
 * by the hardware, not chosen.
 */
#define MUZIX_RTC_YEAR_BASE 2000

struct rtc_time {
	uint8_t second;   /* 0-59  */
	uint8_t minute;   /* 0-59  */
	uint8_t hour;     /* 0-23  */
	uint8_t day;      /* 1-31  */
	uint8_t month;    /* 1-12  */
	uint8_t year;     /* 0-99, meaning MUZIX_RTC_YEAR_BASE + year */
	uint8_t weekday;  /* 1-7, Sunday = 1; 0 = not specified, stored as Sunday */
};

/* Standard I/O */
int putchar(int c);
int getchar(void);
void puts(const char *str);
void printf(const char *fmt, ...);

/* String functions */
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *s);
char *strcat(char *dest, const char *src);

/* Memory functions */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

/* Character functions */
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int toupper(int c);
int tolower(int c);

/* Conversion functions */
int atoi(const char *nptr);
long atol(const char *nptr);
char *itoa(int value, char *str, int base);

/* Process control */
int fork(void);
int exec(const char *path, const char *const *argv);
void exit(int status);
int wait(int *status);
int getpid(void);
int getppid(void);

/* One slot of the process table, as a struct proc_info.
 *
 * This is the only call in the system that reports a process other than the
 * caller, so it is what a program that has to enumerate processes is given -
 * `top` is the one that exists.  `slot` is a slot number and not a pid: slots
 * are handed out from 0, slot 0 is the kernel, and a slot that has never run
 * comes back with active == 0 rather than as an error.  There are
 * MUZIX_PROC_INFO_SLOTS of them and the kernel refuses anything outside that
 * range, so walking the whole range is safe.
 *
 * sys_times() is the same call for the calling process's own slot and takes
 * just the buffer.  It used to answer with a four-counter struct of its own;
 * struct proc_info is a superset of that, and the kernel builds it by handing
 * the front of the slot to the copy-out rather than field by field - see
 * lib/proc_info.h. */
int32_t sys_proctab(struct proc_info *buf, int32_t slot);
int32_t sys_times(struct proc_info *buf);

/* The three CPU busy fractions, 1, 5 and 15 minute constants, in Q14 where
 * 16384 is 1.0.  All three come out in one call because the kernel folds the
 * interval that has just ended into its history as it answers, so there is
 * nothing for a caller to do but read three words.  apps/top.c is the display
 * that reads them; kernel/load.c is where they come from and is the place to
 * look for why they are a busy fraction and not a Unix load average. */
int32_t sys_load(uint32_t *windows);

/* File operations */
int open(const char *pathname, int flags);
int close(int fd);
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int seek(int fd, int offset);

/* Directory operations */
int mkdir(const char *pathname, int mode);
int rmdir(const char *pathname);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int readdir(int fd, int index, struct dirent *entry);

/* File operations - additional */
int unlink(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int stat(const char *pathname, void *statbuf);

/* System control */
void sync(void);
int system(const char *command);

/*
 * Time.
 *
 * These go to the DS1302 on port $70, not to a counter.  time() used to return
 * the kernel's scheduler-pass counter, which does not advance while a process
 * is running: a program that read it twice in a row got the same number both
 * times no matter how long it had been running, and a program that slept saw
 * it move.  A clock that stops when you ask it a question is not a clock.
 */
int time(int32_t *seconds);
int get_rtc(struct rtc_time *time);

/* set_rtc writes the chip and then hands back what the chip read, in the same
 * struct: compare it against what you passed in to find out whether the set
 * took.  That is deliberate - a write that silently did not land would
 * otherwise be indistinguishable from one that did.
 *
 * Returns -1 and leaves both the chip and your struct alone if any field is out
 * of range, so an impossible time is an error rather than a clock set to one.
 * The chip itself would accept 60 seconds and count on through it; the ranges
 * are second 0-59, minute 0-59, hour 0-23, day 1-31, month 1-12, year 0-99 and
 * weekday 0-7.  Weekday 0 means "not specified" and is stored as Sunday, since
 * a burst write always sends that register and cannot skip it.
 *
 * The check is here and not in the kernel's driver because the kernel has no
 * room for it; see platform/zeta-v2/rtc_ds1302.h.  sys_setrtc() below is the
 * unvalidated path underneath, and a program that calls it directly has to make
 * its own. */
int set_rtc(struct rtc_time *time);

/*
 * Seconds since the first exec after power-on: the clock now minus the clock as
 * it read at boot, the second of which the kernel took once and kept.
 *
 * The DS1302 supplies calendar time; the project converts its reading to
 * seconds since 2000-01-01. The kernel saves a boot-time RTC reading and this
 * function subtracts it from the current reading. Scheduler ticks are used for
 * deadlines and process accounting, not as the uptime clock. The baseline is
 * re-taken at every power-on, so time spent switched off is not counted; see
 * lib/libc.c for the calendar assumptions and their limits.
 *
 * get_rtc()'s convention, not time()'s: 0 on success with *seconds holding the
 * interval, -1 when there is no interval to report.  Zero and a negative are
 * both legitimate values here - a machine that has just booted, and a clock set
 * back since boot - so the return value cannot carry the answer.  Callers should
 * treat a negative *seconds as something to report rather than print.
 *
 * `boot` receives the baseline the answer was computed from, which the call has
 * read anyway; pass NULL for the interval alone.  Handing it back is what lets a
 * program print the figure and the reading behind it as one observation.
 */
int uptime(int32_t *seconds, struct rtc_time *boot);

/*
 * Is there unconsumed input on the console?  1 if there is, 0 if there is not,
 * -1 if the request could not be made.
 *
 * This is the one call in the system that answers "has anybody typed
 * anything" without wanting a whole line first. `top` uses it to stop after a
 * keypress without consuming that byte. The system has timer and UART
 * interrupts, but the scheduler is cooperative: neither interrupt preempts a
 * running userspace program, and read() waits for a complete canonical line.
 *
 * It does not block and it does not consume.  Asking is free, and the byte that
 * made the answer non-zero is still there afterwards, so a program can ask
 * every refresh and the shell that gets the console back will still read the
 * keypress that stopped it.  The user does see that keypress echoed, at the
 * next prompt, which is the one visible consequence - see the note in
 * apps/top.c.
 *
 * A negative answer is not a reason to stop. A program polling this should
 * treat -1 as "nothing known" and carry on, rather than as a keypress.
 */
int tty_input_pending(void);

#endif /* MUZIX_LIBC_H */
