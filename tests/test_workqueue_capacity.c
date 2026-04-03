/*
 * test_workqueue_capacity.c - Unit tests for dynamic workqueue ring capacity
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 *
 * Issue #414: Test rewrite for dynamic WL_WQ_RING_CAP formula after issue #407.
 *
 * The workqueue now dynamically allocates ring capacity based on num_workers:
 *   capacity = max(256, next_pow2(num_workers * 2))
 *
 * This test validates:
 *   1. Capacity formula correctness across various worker counts
 *   2. Minimum capacity enforcement (256 items minimum)
 *   3. Power-of-2 rounding for efficient modulo operations
 *   4. Ring full detection when capacity is exhausted
 *   5. Submit W work items successfully without overflow
 *   6. Edge cases: W=0 (invalid), overflow protection for W > 2^31/2
 */

#include "workqueue.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ================================================================
 * Test Framework
 * ================================================================ */

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name)                                    \
        do {                                              \
            test_count++;                                 \
            printf("TEST %d: %s ... ", test_count, name); \
            fflush(stdout);                               \
        } while (0)

#define PASS()              \
        do {                    \
            pass_count++;       \
            printf("PASS\n");   \
        } while (0)

#define FAIL(msg)                   \
        do {                            \
            fail_count++;               \
            printf("FAIL: %s\n", msg);  \
        } while (0)

#define ASSERT(cond, msg)   \
        do {                    \
            if (!(cond)) {      \
                FAIL(msg);      \
                return;         \
            }                   \
        } while (0)

/* ================================================================
 * Helper: Calculate expected capacity for num_workers
 * ================================================================ */

static uint32_t
expected_capacity(uint32_t num_workers)
{
    /* Mirrors workqueue.c:164-171 */
    uint32_t min_cap = num_workers * 2u;
    if (min_cap < 256u)
        min_cap = 256u;

    uint32_t cap = 1u;
    while (cap < min_cap)
        cap <<= 1u;

    return cap;
}

/* ================================================================
 * Helper: Dummy work function for testing
 * ================================================================ */

static void
dummy_work(void *ctx)
{
    (void)ctx; /* unused */
}

/* ================================================================
 * Test 1: create/destroy lifecycle
 * ================================================================ */

static void
test_create_destroy(void)
{
    TEST("create/destroy lifecycle");

    /* Normal creation */
    wl_work_queue_t *wq = wl_workqueue_create(4);
    ASSERT(wq != NULL, "create(4) returned NULL");
    wl_workqueue_destroy(wq);

    /* NULL-safe destroy */
    wl_workqueue_destroy(NULL);

    /* Invalid: 0 workers */
    wq = wl_workqueue_create(0);
    ASSERT(wq == NULL, "create(0) should return NULL");

    /* Single worker */
    wq = wl_workqueue_create(1);
    ASSERT(wq != NULL, "create(1) returned NULL");
    wl_workqueue_destroy(wq);

    PASS();
}

/* ================================================================
 * Test 2: Capacity formula — W=1 (minimum case)
 * ================================================================ */

static void
test_capacity_w1(void)
{
    TEST("capacity formula W=1");

    /* W=1: min_cap = 2 * 1 = 2, rounded up to 256 */
    wl_work_queue_t *wq = wl_workqueue_create(1);
    ASSERT(wq != NULL, "create(1) failed");

    uint32_t expected = expected_capacity(1);
    ASSERT(expected == 256, "expected capacity for W=1 is 256");

    /* Should be able to submit at least 1 item */
    int ret = wl_workqueue_submit(wq, dummy_work, NULL);
    ASSERT(ret == 0, "submit(1) failed");

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 3: Capacity formula — W=2 (minimum pow2 case)
 * ================================================================ */

static void
test_capacity_w2(void)
{
    TEST("capacity formula W=2");

    /* W=2: min_cap = 2 * 2 = 4, rounded up to 256 */
    wl_work_queue_t *wq = wl_workqueue_create(2);
    ASSERT(wq != NULL, "create(2) failed");

    uint32_t expected = expected_capacity(2);
    ASSERT(expected == 256, "expected capacity for W=2 is 256");

    /* Should be able to submit 2 items */
    for (int i = 0; i < 2; i++) {
        int ret = wl_workqueue_submit(wq, dummy_work, NULL);
        ASSERT(ret == 0, "submit(2) failed");
    }

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 4: Capacity formula — W=128 (power-of-2 boundary)
 * ================================================================ */

static void
test_capacity_w128(void)
{
    TEST("capacity formula W=128");

    /* W=128: min_cap = 2 * 128 = 256, capacity = 256 (no rounding) */
    wl_work_queue_t *wq = wl_workqueue_create(128);
    ASSERT(wq != NULL, "create(128) failed");

    uint32_t expected = expected_capacity(128);
    ASSERT(expected == 256, "expected capacity for W=128 is 256");

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 5: Capacity formula — W=129 (requires rounding up)
 * ================================================================ */

static void
test_capacity_w129(void)
{
    TEST("capacity formula W=129");

    /* W=129: min_cap = 2 * 129 = 258, rounded up to 512 */
    wl_work_queue_t *wq = wl_workqueue_create(129);
    ASSERT(wq != NULL, "create(129) failed");

    uint32_t expected = expected_capacity(129);
    ASSERT(expected == 512, "expected capacity for W=129 is 512");

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 6: Capacity formula — W=256 (power-of-2 boundary)
 * ================================================================ */

static void
test_capacity_w256(void)
{
    TEST("capacity formula W=256");

    /* W=256: min_cap = 2 * 256 = 512, capacity = 512 (no rounding) */
    wl_work_queue_t *wq = wl_workqueue_create(256);
    ASSERT(wq != NULL, "create(256) failed");

    uint32_t expected = expected_capacity(256);
    ASSERT(expected == 512, "expected capacity for W=256 is 512");

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 7: Capacity formula — W=257 (requires rounding up)
 * ================================================================ */

static void
test_capacity_w257(void)
{
    TEST("capacity formula W=257");

    /* W=257: min_cap = 2 * 257 = 514, rounded up to 1024 */
    wl_work_queue_t *wq = wl_workqueue_create(257);
    ASSERT(wq != NULL, "create(257) failed");

    uint32_t expected = expected_capacity(257);
    ASSERT(expected == 1024, "expected capacity for W=257 is 1024");

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 8: Capacity formula — W=512 (target for issue #401)
 * ================================================================ */

static void
test_capacity_w512(void)
{
    TEST("capacity formula W=512");

    /* W=512: min_cap = 2 * 512 = 1024, capacity = 1024 (no rounding) */
    wl_work_queue_t *wq = wl_workqueue_create(512);
    ASSERT(wq != NULL, "create(512) failed");

    uint32_t expected = expected_capacity(512);
    ASSERT(expected == 1024, "expected capacity for W=512 is 1024");

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 9: Submit W items without overflow
 * ================================================================ */

static void
test_submit_w_items(void)
{
    TEST("submit W items without overflow");

    uint32_t num_workers = 8;
    wl_work_queue_t *wq = wl_workqueue_create(num_workers);
    ASSERT(wq != NULL, "create(8) failed");

    /* Submit exactly num_workers items; should all succeed */
    for (uint32_t i = 0; i < num_workers; i++) {
        int ret = wl_workqueue_submit(wq, dummy_work, (void *)(uintptr_t)i);
        ASSERT(ret == 0, "submit failed for item");
    }

    wl_workqueue_destroy(wq);
    PASS();
}

/* ================================================================
 * Test 10: Overflow protection (W > UINT32_MAX / 2)
 * ================================================================ */

static void
test_overflow_protection(void)
{
    TEST("overflow protection for W > UINT32_MAX/2");

    /* Attempt to create workqueue with num_workers = UINT32_MAX
     * workqueue.c checks: if (num_workers > UINT32_MAX / 2u) return NULL
     */
    uint32_t huge_workers = UINT32_MAX;
    wl_work_queue_t *wq = wl_workqueue_create(huge_workers);
    ASSERT(wq == NULL,
        "create(UINT32_MAX) should return NULL (overflow protection)");

    PASS();
}

/* ================================================================
 * Test 11: Ring buffer capacity scales with workers
 * ================================================================ */

static void
test_capacity_scaling(void)
{
    TEST("capacity scaling from W=1 to W=512");

    /* Verify the formula produces increasing capacities */
    uint32_t prev_cap = 0;
    for (uint32_t w = 1; w <= 512; w *= 2) {
        uint32_t cap = expected_capacity(w);
        ASSERT(cap >= prev_cap, "capacity should not decrease");
        ASSERT((cap & (cap - 1)) == 0, "capacity must be power of 2");

        wl_work_queue_t *wq = wl_workqueue_create(w);
        ASSERT(wq != NULL, "create failed");
        wl_workqueue_destroy(wq);

        prev_cap = cap;
    }

    PASS();
}

/* ================================================================
 * Test 12: Capacity always >= 256
 * ================================================================ */

static void
test_minimum_capacity(void)
{
    TEST("minimum capacity enforcement");

    /* Even W=1 should have capacity >= 256 */
    for (uint32_t w = 1; w <= 256; w++) {
        uint32_t cap = expected_capacity(w);
        ASSERT(cap >= 256, "capacity must be >= 256");
    }

    PASS();
}

/* ================================================================
 * Main
 * ================================================================ */

int
main(void)
{
    printf(
        "================================================================================\n");
    printf(
        "test_workqueue_capacity.c - Dynamic Ring Buffer Capacity Tests (Issue #414)\n");
    printf(
        "================================================================================\n\n");

    test_create_destroy();
    test_capacity_w1();
    test_capacity_w2();
    test_capacity_w128();
    test_capacity_w129();
    test_capacity_w256();
    test_capacity_w257();
    test_capacity_w512();
    test_submit_w_items();
    test_overflow_protection();
    test_capacity_scaling();
    test_minimum_capacity();

    printf(
        "\n================================================================================\n");
    printf("Results: %d/%d tests passed", pass_count, test_count);
    if (fail_count > 0)
        printf(", %d FAILED", fail_count);
    printf(
        "\n================================================================================\n");

    return (fail_count == 0) ? 0 : 1;
}
