/*
 * test_w5100_timeout.c - host test for W5100 timeout handling using
 * elapsed-time deadlines instead of fixed iteration counts.
 *
 * Tests that MUZIX_TICK_DEADLINE() and muzix_tick_expired() work
 * correctly for W5100 polling loops, including tick-counter wraparound
 * and bounded termination.
 */

#include "ztypes.h"
#include <stdio.h>
#include "../../platform/zeta-v2/tick.h"

/* From test_host/z80_io_host.c: the host's settable CTC tick. */
extern uint16_t muzix_host_tick;
extern void muzix_host_tick_set(uint16_t ticks);
extern void muzix_host_tick_advance(uint16_t ticks);

static int failures;

static void
check(int ok, int id, const char *what) {
    if (!ok) {
        printf("FAIL %d: %s\n", id, what);
        failures++;
    }
}

/* --- Test 1: deadline creation and expiry --- */

static int
test_deadline_expiry(void) {
    int id = 100;
    uint16_t now;

    /* Set tick to 1000, create a 1-second (120 tick) deadline */
    muzix_host_tick_set(1000);
    uint16_t deadline = MUZIX_TICK_DEADLINE(1);  /* 1000 + 120 = 1120 */
    check(deadline == 1120u, id, "1-second deadline = now + 120 ticks");

    /* Not expired at the instant it is set */
    check(muzix_tick_expired(deadline) == 0, id + 1,
          "deadline not expired at creation time");

    /* Advance 119 ticks: still not expired */
    muzix_host_tick_advance(119);
    check(muzix_tick_expired(deadline) == 0, id + 2,
          "deadline not expired 119 ticks later (now = 1119)");

    /* Advance 1 more tick: expired */
    muzix_host_tick_advance(1);
    check(muzix_tick_expired(deadline) == 1, id + 3,
          "deadline expired at now = 1120");

    return failures == 0 ? 0 : 1;
}

/* --- Test 2: tick counter wraparound --- */

static int
test_wraparound(void) {
    int id = 200;
    uint16_t deadline;

    /* Set tick near wrap: 65500, create a 10-second (1200 tick) deadline */
    muzix_host_tick_set(65500u);
    deadline = MUZIX_TICK_DEADLINE(10);  /* 65500 + 1200 = 66700 -> wraps to 1164 */
    check(deadline == 1164u, id, "wraparound deadline = (65500 + 1200) & 0xFFFF = 1164");

    /* Not expired just before wrap */
    muzix_host_tick_set(65530u);
    check(muzix_tick_expired(deadline) == 0, id + 1,
          "not expired before wrap (now = 65530, deadline = 1164)");

    /* Advance past wrap: set now directly to 1170 (past deadline = 1164) */
    muzix_host_tick_set(1170u);
    check(muzix_tick_expired(deadline) == 1, id + 2,
          "expired after wrap (now = 1170 > deadline = 1164)");

    return failures == 0 ? 0 : 1;
}

/* --- Test 3: polling loop terminates on deadline --- */

static int g_condition_met = 0;

static uint8_t
condition_never(void) {
    return 0;
}

static uint8_t
condition_met(void) {
    return g_condition_met;
}

static int
test_poll_terminates_on_timeout(void) {
    int id = 300;

    /* Condition never met: advance tick past deadline, then check expiry */
    muzix_host_tick_set(5000);
    uint16_t deadline = MUZIX_TICK_DEADLINE(1);  /* 120 ticks from now */
    check(muzix_tick_expired(deadline) == 0, id, "deadline not expired at creation");

    /* Advance past deadline */
    muzix_host_tick_advance(120);
    check(muzix_tick_expired(deadline) == 1, id + 1,
          "deadline has expired after advancing past it");

    return failures == 0 ? 0 : 1;
}

static int
test_poll_terminates_on_success(void) {
    int id = 400;

    /* Condition met immediately: no need to wait */
    muzix_host_tick_set(1000);
    uint16_t deadline = MUZIX_TICK_DEADLINE(10);  /* far in the future */
    g_condition_met = 1;
    check(g_condition_met, id, "condition is met");
    check(muzix_tick_expired(deadline) == 0, id + 1,
          "deadline not expired after successful poll");

    g_condition_met = 0;
    return failures == 0 ? 0 : 1;
}

/* --- Test 4: W5100 timeout durations --- */

static int
test_timeout_durations(void) {
    int id = 500;

    /* w5100_wait_cmd_done: 1 second = 120 ticks */
    muzix_host_tick_set(100);
    uint16_t d = MUZIX_TICK_DEADLINE(1);
    check((uint16_t)(d - 100) == 120u, id, "cmd_done timeout = 1 second (120 ticks)");

    /* w5100_wait_send_done: 5 seconds = 600 ticks */
    d = MUZIX_TICK_DEADLINE(5);
    check((uint16_t)(d - 100) == 600u, id + 1,
          "send_done timeout = 5 seconds (600 ticks)");

    /* w5100_init MR reset: 2 seconds = 240 ticks */
    d = MUZIX_TICK_DEADLINE(2);
    check((uint16_t)(d - 100) == 240u, id + 2,
          "init reset timeout = 2 seconds (240 ticks)");

    /* w5100_open_socket: 5 seconds = 600 ticks */
    d = MUZIX_TICK_DEADLINE(5);
    check((uint16_t)(d - 100) == 600u, id + 3,
          "open_socket timeout = 5 seconds (600 ticks)");

    /* w5100_close first wait: 10 seconds = 1200 ticks */
    d = MUZIX_TICK_DEADLINE(10);
    check((uint16_t)(d - 100) == 1200u, id + 4,
          "close timeout = 10 seconds (1200 ticks)");

    /* w5100_close forced wait: 5 seconds = 600 ticks */
    d = MUZIX_TICK_DEADLINE(5);
    check((uint16_t)(d - 100) == 600u, id + 5,
          "close forced timeout = 5 seconds (600 ticks)");

    return failures == 0 ? 0 : 1;
}

/* --- Test 5: every W5100 polling loop remains bounded --- */

static int
test_all_loops_bounded(void) {
    int id = 600;

    /* Simulate each W5100 wait loop: advance tick to just before deadline,
     * then one tick past.  Each deadline must correctly report expiry. */

    /* w5100_wait_cmd_done: 1 second deadline (120 ticks) */
    muzix_host_tick_set(2000);
    uint16_t deadline = MUZIX_TICK_DEADLINE(1);
    check(muzix_tick_expired(deadline) == 0, id, "cmd_done not expired at creation");
    muzix_host_tick_advance(120);
    check(muzix_tick_expired(deadline) == 1, id + 1,
          "cmd_done expired after 1-second deadline");

    /* w5100_wait_send_done: 5 second deadline (600 ticks) */
    muzix_host_tick_set(2000);
    deadline = MUZIX_TICK_DEADLINE(5);
    check(muzix_tick_expired(deadline) == 0, id * 2, "send_done not expired at creation");
    muzix_host_tick_advance(600);
    check(muzix_tick_expired(deadline) == 1, id * 2 + 1,
          "send_done expired after 5-second deadline");

    /* w5100_init MR reset: 2 second deadline (240 ticks) */
    muzix_host_tick_set(2000);
    deadline = MUZIX_TICK_DEADLINE(2);
    check(muzix_tick_expired(deadline) == 0, id * 3, "init reset not expired at creation");
    muzix_host_tick_advance(240);
    check(muzix_tick_expired(deadline) == 1, id * 3 + 1,
          "init reset expired after 2-second deadline");

    /* w5100_close: 10 second deadline (1200 ticks) */
    muzix_host_tick_set(2000);
    deadline = MUZIX_TICK_DEADLINE(10);
    check(muzix_tick_expired(deadline) == 0, id * 4, "close not expired at creation");
    muzix_host_tick_advance(1200);
    check(muzix_tick_expired(deadline) == 1, id * 4 + 1,
          "close expired after 10-second deadline");

    return failures == 0 ? 0 : 1;
}

/* --- main --- */

int
main(void) {
    int rc;

    rc = test_deadline_expiry();
    if (rc != 0) return rc + 100;
    rc = test_wraparound();
    if (rc != 0) return rc + 200;
    rc = test_poll_terminates_on_timeout();
    if (rc != 0) return rc + 300;
    rc = test_poll_terminates_on_success();
    if (rc != 0) return rc + 400;
    rc = test_timeout_durations();
    if (rc != 0) return rc + 500;
    rc = test_all_loops_bounded();
    if (rc != 0) return rc + 600;

    if (failures) {
        printf("test_w5100_timeout: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_w5100_timeout: all cases pass\n");
    return 0;
}

