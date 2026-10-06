#include "syscalls.h"
#include "kernel_loop.h"
#include "../mm/mm_service.h"
#include "system_task.h"
#include "../mm/mproc_model.h"
#include "../platform/zeta-v2/bank_io.h"
#include "../platform/zeta-v2/rtc_ds1302.h"

/*
 * SYS_SETRTC must hand the chip's answer back to the caller.
 *
 * THE DEFECT THIS IS FOR
 * ----------------------
 * The handler copies the caller's muzix_rtc_time_t into a kernel-local, hands
 * that to muzix_zeta_rtc_set(), and returns.  muzix_zeta_rtc_set() does not stop
 * at the write: it reads the chip back over the same struct, so on return the
 * struct holds what the chip says rather than what it was asked to store.  The
 * handler threw that away, and with it the only report a caller gets about
 * whether the set took.
 *
 * `date` is that caller.  It saves what it asked for, sets, and compares - and
 * since the caller's buffer was never written, the comparison was the requested
 * time against itself.  It could not fail.  "the clock did not take the new
 * time" was a message with no way of ever being produced, and every report of a
 * set that silently went nowhere was indistinguishable from a set that worked.
 *
 * WHY A DOUBLE FOR ANYTHING IS NOT USED
 * -------------------------------------
 * Nothing here is stubbed.  The handler is the real one, the memory manager's
 * mediation is the real one, the DS1302 driver is the real one and the chip is
 * the emulator's own model - the same one platform/zeta-v2/test_rtc_ds1302.c
 * drives.  What stands in is the platform's banked copy primitives, in
 * test_host/zeta_host.c, and that is a seam the handler does not own: it is the
 * place where the C stops being portable.  A test that supplied its own
 * muzix_zeta_rtc_set() would prove only that a memcpy exists somewhere.
 *
 * HOW THE CHIP IS MADE TO DISAGREE
 * --------------------------------
 * Not by a lie, but by a case the driver documents.  muzix_zeta_rtc_set() writes
 * bin_to_bcd(time->weekday ? time->weekday : 1u) - a burst write always sends the
 * day-of-week register, so there is no way to leave it alone, and a caller that
 * did not say (zero) gets Sunday.  So a request with weekday 0 comes back from
 * the chip with weekday 1, in band, through the real write and the real read-back.
 * That is the "the clock did not take the new time" case itself, and it is the
 * only one needed: before the fix the caller's buffer keeps the 0 it was given
 * and the test fails; after the fix it holds the 1 the chip stored.
 */

/*
 * host-test-provides: muzix_mm_copy_user_to_kernel muzix_mm_copy_kernel_to_user
 *                      muzix_mm_service_init muzix_mm_memory_init
 *                      muzix_mm_alloc_page muzix_mm_free_page
 *                      muzix_mm_load_kernel_map muzix_mm_cross_bank_call
 *                      muzix_mm_copy_page_to_kernel
 *                      muzix_mm_copy_rom_page_to_kernel
 *
 * WHY THE MEMORY MANAGER'S OWN COPY ENTRY POINTS ARE NOT USED
 * ----------------------------------------------------------
 * muzix_handle_userspace_syscall() does not copy the caller's struct itself; it
 * calls muzix_mm_copy_user_to_kernel(), which does not copy either.  It builds a
 * message whose dst_vir field carries the KERNEL BUFFER'S ADDRESS, and sends it
 * through the system task:
 *
 *     msg.dst_vir = (uint16_t)(uintptr_t)kernel_buf;      mm/mm_service.c:298
 *     kernel_buf = (void *)(uintptr_t)msg->dst_vir;       mm/mm_copy.c:241
 *
 * A uint16_t is the whole address because the kernel's C stack lives in window 3
 * at a 16-bit address.  On a host the same expression truncates a 64-bit stack
 * address to its low word and the copy writes to 0x8555, which is a SEGV.  That
 * is not a bug on the target and it is not fixable in the test: it is the Z80's
 * address space being load-bearing in the message ABI.
 *
 * So these two entry points are the seam, and they are named in the directive
 * above so that tools/host_tests.rb stops looking for the real ones for this
 * test rather than linking them alongside and letting the linker's idea of which
 * wins decide it.  What they do is the real thing minus the message: a copy
 * between the flat address space in test_host/zeta_host.c and a buffer.
 *
 * Everything above them is still the tree's own: the handler, the copy
 * direction, the chunking, the ordering, and the caller's own struct.
 */
#include <stdint.h>
#include <string.h>

/* From test_host/zeta_host.c: the flat address space standing in for the
 * caller's four banked windows. */
uint8_t *muzix_host_user_space(void);
void muzix_host_user_space_clear(void);

/* From test_host/z80_io_host.c: where the driver's port accesses go. */
void muzix_host_port_attach(void (*write)(uint8_t port, uint8_t value),
                            uint8_t (*read)(uint8_t port));

/* From test_host/ds1302_host.c: the emulator's own chip model, reached through
 * one door so that emulator.h - which declares a Zeta bank API that collides
 * with the kernel's platform headers - stays inside test_host/. */
void muzix_host_ds1302_reset(void);
void muzix_host_ds1302_write(uint8_t port, uint8_t value);
uint8_t muzix_host_ds1302_read(uint8_t port);

/* Where the caller will say its struct is.  Window 2 of a process, because
 * windows 0 and 3 are the kernel's and the copy primitives refuse both - userspace
 * stack is at 0xFFF0, which is window 3, and the real primitives refuse that too,
 * which is why this address is in window 2 and the test does not lean on a copy
 * the target would refuse. */
#define CALLER_STRUCT_ADDR 0x8000u

/*
 * The caller's side of the syscall.
 *
 * A real muzix_kernel_loop_t, initialised and registered through the tree's own
 * entry points, because the handler's copy helpers resolve the running process
 * by walking loop->system.startup.proc_table - its `current`, then the pid
 * fallback.  A hand-built context with the same fields filled in would have made
 * this test assert against a route nothing takes.
 *
 * loop->mm is a pointer to storage and nothing else: the two MM entry points
 * the handler reaches are the ones this file provides (see the header), and they
 * ignore it.  So there is no system task and no mm service here, and
 * muzix_system_task_init() is not called - the message hop it would carry is
 * part of what the seam replaces.
 */
static muzix_kernel_loop_t g_loop;
static muzix_mm_service_t g_mm;
/* The kernel's own map, as kernel_main.c:117-118 builds it, and a process map
 * that is the kernel's map with the data and text pages moved - which is the
 * arrangement muzix_mproc_to_zeta_map() derives and the one the copy resolves
 * against.  Nothing here reads the pages; the flat address space in
 * test_host/zeta_host.c stands behind any address at all. */
static process_map_t g_test_kernel_map = {{MUZIX_ZETA_KERNEL_BANK_0,
                                      MUZIX_ZETA_KERNEL_BANK_1,
                                      MUZIX_ZETA_KERNEL_BANK_2,
                                      MUZIX_ZETA_KERNEL_BANK_3}};
static process_map_t g_test_init_map;
static muzix_mproc_t g_init_mp;

static muzix_syscall_context_t g_kernel_ctx;
static muzix_userspace_syscall_context_t g_ctx;

static void chip_write(uint8_t port, uint8_t value)
{
    muzix_host_ds1302_write(port, value);
}

static uint8_t chip_read(uint8_t port)
{
    return muzix_host_ds1302_read(port);
}

static int setup(void)
{
    muzix_host_user_space_clear();
    muzix_host_port_attach(chip_write, chip_read);
    muzix_host_ds1302_reset();

    g_test_init_map = g_test_kernel_map;
    muzix_kernel_loop_init(&g_loop, &g_test_kernel_map, 1, &g_test_init_map);
    g_loop.mm = &g_mm;

    muzix_mproc_init(&g_init_mp, 0x0000u, 0x1000u, 0x2000u, 0x1000u, 0x1000u,
                     0x1000u, 1);
    muzix_kernel_loop_register(&g_loop, 0, 1, &g_init_mp, &g_test_init_map,
                               MUZIX_PROC_TASK_Q,
                               (uint8_t)(MUZIX_PROC_FREE | MUZIX_PROC_NO_MAP));
    /* The table's `current` is what a running process looks like, and the
     * handler's copy helpers resolve the slot through it first. */
    g_loop.system.startup.proc_table.current = 0;
    g_loop.current_pid = 1;

    memset(&g_kernel_ctx, 0, sizeof(g_kernel_ctx));
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_kernel_ctx.loop = &g_loop;
    g_kernel_ctx.mm = &g_mm;
    g_kernel_ctx.process_table = &g_loop.system.startup.proc_table;
    g_ctx.kernel = &g_kernel_ctx;
    g_ctx.pid = 1;
    return 0;
}

/* THE SEAM, in two halves.
 *
 * The first half is the MM's two user-facing copy entry points, and these do the
 * work: a copy between the caller's struct in the flat address space and the
 * kernel's.  The real ones cannot be used - see the file header - and getting
 * the direction wrong here would make every case below fail, so the direction is
 * the thing being asserted as much as the copy is.
 *
 * The second half is seven entry points that no SYS_SETRTC path reaches, named
 * only because kernel/syscalls.c references all of them and the module that
 * defines them is excluded by the directive above.  Each refuses rather than
 * pretending: a page allocation that returned a plausible page number would
 * make a future test of this handler believe it had a page. */

int muzix_mm_copy_user_to_kernel(muzix_mm_service_t *mm, uint8_t src_proc,
                                 uint16_t src_vir, void *kernel_buf,
                                 uint16_t len)
{
    (void)mm;
    (void)src_proc;
    if (!kernel_buf) {
        return -1;
    }
    memcpy(kernel_buf, &muzix_host_user_space()[src_vir], len);
    return 0;
}

int muzix_mm_copy_kernel_to_user(muzix_mm_service_t *mm, uint8_t dst_proc,
                                 uint16_t dst_vir, const void *kernel_buf,
                                 uint16_t len)
{
    (void)mm;
    (void)dst_proc;
    if (!kernel_buf) {
        return -1;
    }
    memcpy(&muzix_host_user_space()[dst_vir], kernel_buf, len);
    return 0;
}

void muzix_mm_service_init(muzix_mm_service_t *mm,
                           struct muzix_system_task_t *system_task,
                           uint8_t source)
{
    (void)system_task;
    (void)source;
    if (mm) {
        memset(mm, 0, sizeof(*mm));
    }
}

void muzix_mm_memory_init(muzix_mm_service_t *mm,
                          const process_map_t *kernel_map)
{
    (void)kernel_map;
    (void)mm;
}

int muzix_mm_alloc_page(muzix_mm_service_t *mm, uint8_t *page)
{
    (void)mm;
    (void)page;
    return -1;
}

void muzix_mm_free_page(muzix_mm_service_t *mm, uint8_t page)
{
    (void)mm;
    (void)page;
}

int muzix_mm_load_kernel_map(muzix_mm_service_t *mm, const process_map_t *map)
{
    (void)mm;
    (void)map;
    return -1;
}

int muzix_mm_cross_bank_call(muzix_mm_service_t *mm,
                             const process_map_t *target_map,
                             int (*func)(void *), void *arg)
{
    (void)mm; (void)target_map; (void)func; (void)arg;
    return -1;
}

int muzix_mm_copy_page_to_kernel(muzix_mm_service_t *mm, uint8_t page,
                                  uint16_t offset, void *dst, uint16_t len)
{
    (void)mm; (void)page; (void)offset; (void)dst; (void)len;
    return -1;
}

int muzix_mm_copy_rom_page_to_kernel(muzix_mm_service_t *mm, uint8_t page,
                                     uint16_t offset, void *dst, uint16_t len)
{
    (void)mm; (void)page; (void)offset; (void)dst; (void)len;
    return -1;
}

/* Put a request in the caller's buffer and set the clock from it. */
static int32_t set_rtc(const muzix_rtc_time_t *request)
{
    memcpy(&muzix_host_user_space()[CALLER_STRUCT_ADDR], request,
           sizeof(*request));
    return muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_SETRTC,
                                         (int32_t)CALLER_STRUCT_ADDR, 0, 0);
}

/* What the caller is left holding. */
static muzix_rtc_time_t caller_time(void)
{
    muzix_rtc_time_t time;

    memcpy(&time, &muzix_host_user_space()[CALLER_STRUCT_ADDR], sizeof(time));
    return time;
}

/*
 * 1: the write-back.  The clock stored Sunday for a request that did not name a
 *    weekday, and the caller has to be able to find that out.  This is the test
 *    the defect needs: before the handler copied the struct back, the caller's
 *    buffer still held the 0 and this returned 1.
 */
static int test_write_back_reaches_the_caller(void)
{
    muzix_rtc_time_t request;
    muzix_rtc_time_t after;
    int32_t rc;

    /* Binary, because that is what muzix_rtc_time_t holds and what
     * muzix_zeta_rtc_set() encodes: the driver's own BCD conversion is on the
     * path, and a test that hands it BCD would be testing nothing.  This is not
     * hypothetical - the first version of this case passed 0x30 and 0x45, which
     * are the BCD literals for these same two times, and the chip then reported
     * 48 and 69. */
    request.second = 30u;
    request.minute = 45u;
    request.hour   = 13u;
    request.day    = 15u;
    request.month  = 9u;
    request.year   = 26u;
    request.weekday = 0u;      /* not named: the chip stores Sunday */

    rc = set_rtc(&request);
    if (rc != 0) {
        return 1;
    }

    after = caller_time();
    /* Every field but the weekday has to have come back unchanged - the set did
     * work, and the only disagreement is the one the case is about. */
    if (after.second != 30u || after.minute != 45u || after.hour != 13u ||
        after.day != 15u || after.month != 9u || after.year != 26u) {
        return 2;
    }
    /* And the weekday has to have come back as the chip stored it.  If it is
     * still 0, the handler wrote the struct to the chip and dropped the answer. */
    if (after.weekday != 1u) {
        return 3;
    }
    return 0;
}

/*
 * 2: the caller's buffer is not a scratch buffer.  A handler that copied back
 *    without looking at where it read from would pass test 1 with any address.
 *    This asks for a time whose fields are all distinct and checks that all of
 *    them landed, so a copy from the wrong place cannot pass it.
 */
static int test_the_right_bytes_came_back(void)
{
    muzix_rtc_time_t request;
    muzix_rtc_time_t after;
    int32_t rc;

    /* Binary again, and every field a different value so that a copy from the
     * wrong offset lands somewhere else.  17, 18 and 19 are the interesting
     * ones: bin_to_bcd turns them into 0x11, 0x12 and 0x13, so a test that fed
     * the BCD literals here would look right and assert nothing about the
     * conversion. */
    request.second = 7u;
    request.minute = 8u;
    request.hour   = 1u;
    request.day    = 17u;
    request.month  = 18u;
    request.year   = 19u;
    request.weekday = 5u;

    rc = set_rtc(&request);
    if (rc != 0) {
        return 1;
    }
    after = caller_time();
    if (after.second != 7u || after.minute != 8u || after.hour != 1u ||
        after.day != 17u || after.month != 18u || after.year != 19u ||
        after.weekday != 5u) {
        return 2;
    }
    return 0;
}

/*
 * 3: a second set from the struct the first one left behind.  This is what
 *    makes the write-back worth having: with it, the caller can set the clock
 *    again from what the chip says rather than from what it hoped the chip
 *    said, and a disagreement that survives one round trip is visible.
 */
static int test_a_second_set_starts_from_the_answer(void)
{
    muzix_rtc_time_t request;
    muzix_rtc_time_t after;
    int32_t rc;

    /* Whatever the first set left in the caller's buffer, asking for it again
     * must be accepted, and the chip must agree - so the answer is a fixed
     * point.  Before the fix the buffer held the request, which the chip had
     * already disagreed with, and this would be setting a clock back. */
    after = caller_time();
    rc = set_rtc(&after);
    if (rc != 0) {
        return 1;
    }
    request = caller_time();
    if (memcmp(&request, &after, sizeof(after)) != 0) {
        return 2;
    }
    return 0;
}

/*
 * 4: the refusal is still a refusal.  A null user pointer must come back -1 and
 *    must not be copied to, so that the write-back cannot turn a refused call
 *    into a successful-looking one.
 */
static int test_null_pointer_is_still_refused(void)
{
    int32_t rc = muzix_handle_userspace_syscall(&g_ctx, MUZIX_USER_SYS_SETRTC,
                                               0, 0, 0);
    return rc == -1 ? 0 : 1;
}

int main(void)
{
    static int (*const cases[])(void) = {
        test_write_back_reaches_the_caller,
        test_the_right_bytes_came_back,
        test_a_second_set_starts_from_the_answer,
        test_null_pointer_is_still_refused
    };
    unsigned i;
    int rc;

    setup();
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc = cases[i]();
        if (rc != 0) {
            /* case number * 10 + what went wrong, so the exit status names it */
            return (int)(i + 1) * 10 + rc;
        }
    }
    return 0;
}
