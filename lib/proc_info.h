#ifndef MUZIX_PROC_INFO_H
#define MUZIX_PROC_INFO_H

#include <stdint.h>

/* The CTC tick rate, in hertz, and the number that makes the four counters
 * below seconds rather than an abstract unit.
 *
 * It is defined HERE and not next to the CTC driver because both sides need it
 * and only one of them can include a platform header: the kernel reads it
 * through platform/zeta-v2/tick.h, and a userspace program printing the
 * columns has no business including platform/zeta-v2/tick.h - that header
 * declares the interrupt handler, which does not exist in a userspace image.
 * Two names for one number is a drift bug waiting to happen, so there is one
 * name and it lives in the header that documents the units.
 *
 * 7372800 / (256 * 240) = 120 exactly: the CTC runs at a 256 prescale in timer
 * mode and tick.s programs 240, and 240 * 256 = 61440 divides 7 372 800 with
 * no remainder.  platform/zeta-v2/test_tick.c re-derives it from the assembler's
 * own constants and fails the build if the two ever stop agreeing - a comment
 * saying 120 next to a time constant that produces 121 is exactly the kind of
 * thing that is true until somebody changes the constant. */
#define MUZIX_PROC_TICK_HZ 120u

/*
 * One process, as userspace is allowed to see it.
 *
 * This is the whole of the process table as it crosses to a program: there is
 * no other syscall that reports a process the caller did not ask about on
 * itself, so this struct is what `top` and anything else that has to enumerate
 * processes is given.  Every field is a plain integer - no pointers, no strings
 * and nothing whose address is meaningful outside the kernel - because the
 * kernel hands it to the MM to be copied out literally.
 *
 * The four leading bytes are the first four of muzix_proc_slot_t and the four
 * counters follow them in the same order, so the kernel fills a slot by handing
 * the front of the slot to the copy-out with no per-field code at all.  That is
 * a size decision, not an aesthetic one: the kernel has under fifty spare bytes
 * and an explicit field-by-field fill does not fit in them.  The
 * correspondence is enforced, not assumed - kernel/proc_table.h carries a
 * compile-time check of every offset, so a field added to the front of the slot
 * without updating this struct fails the build rather than producing plausible
 * wrong numbers.
 *
 * The struct is the first FORTY bytes of the slot.  Twenty-four of them are
 * the fields that were here first, and the four bytes past the original twenty
 * are the next four bytes of the slot for the same reason - they are already
 * there, in the same place, so widening the window that is copied costs no
 * code at all.  The sixteen after that are the command name, which is a field
 * that could not be anywhere else and reach userspace for nothing.  The
 * alternative, a syscall that walks the ready queues and returns a count, was
 * measured at 315 bytes of _CODE for the straightforward implementation and 191
 * bytes stripped of every bounds check, against 46 bytes of room.  It is the
 * reason the tail fields are here rather than the reason the counters are not.
 *
 * flags is muzix_proc_slot_t.p_flags verbatim: MUZIX_PROC_FREE is set on a slot
 * that has never run, MUZIX_PROC_NO_MAP on one with no bank map.  active is 0
 * for a free slot and 1 for a live one; a process that has exited but has not
 * been waited for is still active, which is what distinguishes a zombie from a
 * free slot.
 *
 * The times are ticks of the CTC timer, and the tick is 120 Hz
 * (platform/zeta-v2/tick.h), so a value of 120 is exactly one second.  The
 * kernel accumulates them at the two ends of each syscall
 * (muzix_proc_table_charge), so UTIME is the
 * time between the kernel handing this process the CPU and it trapping back
 * into the kernel, and STIME is the time the kernel then spent serving it.
 *
 * These are CPU-accounting times, not CPU-utilization percentages. Because the
 * scheduler is cooperative, UTIME covers the process's running interval until
 * it traps, parks, or exits; STIME records time spent handling its syscalls.
 * A process that is not running accumulates nothing.
 *
 * They used to be an entry count instead.  The kernel incremented UTIME each
 * time it entered userspace, which is zero for every process that was never
 * preempted - which, before the tick existed, was all of them - and the columns
 * read zero on a system that was working correctly.  Counting entries is not
 * counting time, and the difference is invisible until there is a clock to
 * count with.
 *
 * The units are ticks rather than seconds, and that is not a shortcut.  The
 * kernel has 152 bytes of stack headroom over the floor its own deepest
 * measured syscall chain sets, and a 16-bit divide to convert is not free in
 * those bytes.  A program that wants seconds has the same problem for a
 * different reason: the counters are uint32_t, so a 32-bit divide is
 * `__divulong`, which is not in the link - and a call to an undefined global
 * on this machine is a call to the boot stub, so apps/top.c prints ticks and
 * says 120 a second rather than dividing.  120 divides a tick count exactly,
 * so a caller that does want seconds can do it without rounding.
 *
 * exited separates a zombie - dead, still holding a slot, waiting to be waited
 * for - from a live process, and ready_q is the scheduler queue the process was
 * registered into: TASK_Q, SERVER_Q or USER_Q.  It is the queue the process
 * belongs to, NOT a flag saying it is sitting in that queue right now.  The
 * kernel does not clear ready_q when a process is dequeued, and on this
 * platform the userspace switch path (muzix_context_switch_to) does not dequeue
 * at all, so a field read as "is queued" would be wrong.  What it does say is
 * which queue will run this process next, which is the part worth showing.
 */

/* How many bytes of a command name a slot carries, NUL included.  Fifteen
 * characters plus the terminator, which is twice the longest name in ROMFS
 * ("shell") and leaves room for one that is a path rather than a bare command.
 * Four slots of this is 64 bytes of _DATA, paid for out of the fork stack
 * scratch - see kernel/kernel_main.c. */
#define MUZIX_PROC_NAME_SIZE 16

struct proc_info {
    uint8_t  pid;
    uint8_t  flags;
    uint8_t  active;
    uint8_t  ppid;
    uint32_t user_time;
    uint32_t sys_time;
    uint32_t child_user_time;
    uint32_t child_sys_time;
    uint8_t  exit_status;
    uint8_t  exited;
    uint8_t  ready_q;
    uint8_t  owned_count;
    /* The command name, as the exec that created this slot was given it: the
     * first MUZIX_PROC_NAME_SIZE-1 characters of the path, NUL-terminated.
     *
     * It rides the same transfer as everything above it because it sits in the
     * same place: this struct is the head of muzix_proc_slot_t, so widening the
     * struct by sixteen bytes widens the window the kernel hands to the copy-out
     * and costs no code at all - the size is a compile-time constant in an
     * argument that was already there.  A separate syscall, or a name index
     * into a kernel-side table, would each have cost code this kernel has
     * fifteen bytes of, and the index would have cost a table on top.
     *
     * Empty means nobody has exec'd into this slot yet: the kernel's own slot,
     * a free slot, and a forked child that has not called exec.  A program
     * showing this column prints something in place of an empty name. */
    char     name[MUZIX_PROC_NAME_SIZE];
};

/* The scheduler's ready queues, as reported in ready_q.  These mirror
 * kernel/proc_table.h's MUZIX_PROC_TASK_Q / _SERVER_Q / _USER_Q, which checks
 * them against these at compile time so the two cannot drift. */
#define MUZIX_PROC_Q_TASK 0
#define MUZIX_PROC_Q_SERVER 1
#define MUZIX_PROC_Q_USER 2

/* How many slots there are to ask about.  A program walks this many and skips
 * what comes back inactive; kernel/proc_table.h checks it against the real
 * table size at compile time. */
#define MUZIX_PROC_INFO_SLOTS 4

/* The single transfer this struct needs.  The MM refuses a copy longer than its
 * staging buffer rather than splitting it, so this has to stay well under
 * MUZIX_ZETA_COPY_STAGING - it is about a tenth of it. */
#define MUZIX_PROC_INFO_SIZE ((uint16_t)sizeof(struct proc_info))

#endif /* MUZIX_PROC_INFO_H */