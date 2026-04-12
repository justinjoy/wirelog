/*
 * test_io_dispatch.c - .input dispatch test scaffold (TDD gate)
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 *
 * Three test scenarios for adapter-based .input dispatch:
 * dispatch_mock_adapter, dispatch_csv_default, dispatch_unknown_scheme.
 * All guarded behind TEST_DISPATCH_PRESENT and currently SKIP until
 * #458 implements the session_facts.c dispatch rewrite.
 *
 * Part of #446 (I/O adapter umbrella).
 */

#include <stdio.h>
#include <string.h>

/* ======================================================================== */
/* Test Harness                                                             */
/* ======================================================================== */

#define TEST(name) do { printf("  [TEST] %-50s ", name); } while (0)
#define PASS()     do { printf("PASS\n"); passed++; } while (0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); failed++; } while (0)
#define SKIP(msg)  do { printf("SKIP: %s\n", msg); skipped++; } while (0)

static int passed = 0, failed = 0, skipped = 0;

/* ======================================================================== */
/* Tests                                                                    */
/* ======================================================================== */

/*
 * test_dispatch_mock_adapter
 *
 * Register a mock adapter with scheme="mock". Parse+compile a program
 * with `.input events(io="mock", key="val")`. Call
 * wl_session_load_input_files. Assert the mock adapter's `read`
 * callback was invoked (use a global flag/counter). Assert the mock
 * received the correct params (key="val").
 */
static void
test_dispatch_mock_adapter(void)
{
    TEST("dispatch_mock_adapter");
#ifdef TEST_DISPATCH_PRESENT
    /* Register a mock adapter with scheme="mock" */
    static int mock_called = 0;
    mock_called = 0;

    /* TODO(#458): Full implementation when dispatch rewrite lands.
     *
     * wl_io_adapter_t mock = {0};
     * mock.scheme = "mock";
     * mock.abi_version = WL_IO_ABI_VERSION;
     * mock.read = mock_read_cb;  // sets mock_called=1, checks params
     * wl_io_register_adapter(&mock);
     *
     * Parse+compile: .input events(io="mock", key="val")
     * Call wl_session_load_input_files(session)
     * Assert mock_called == 1
     * Assert mock received key="val"
     */
    (void)mock_called;
    FAIL("dispatch rewrite not yet implemented (#458)");
#else
    SKIP("dispatch rewrite not yet implemented (#458)");
#endif
}

/*
 * test_dispatch_csv_default
 *
 * Parse a program with `.input events(filename="test.csv")` (no io=).
 * Call wl_session_load_input_files. Assert it routes to the built-in
 * CSV adapter (no crash, produces expected results -- or at minimum,
 * does not call the mock adapter).
 */
static void
test_dispatch_csv_default(void)
{
    TEST("dispatch_csv_default");
#ifdef TEST_DISPATCH_PRESENT
    /* TODO(#458): Full implementation when dispatch rewrite lands.
     *
     * Parse+compile: .input events(filename="test.csv")
     * Call wl_session_load_input_files(session)
     * Assert no crash, routes to built-in CSV adapter
     * Assert mock adapter was NOT called
     */
    FAIL("dispatch rewrite not yet implemented (#458)");
#else
    SKIP("dispatch rewrite not yet implemented (#458)");
#endif
}

/*
 * test_dispatch_unknown_scheme_error
 *
 * Parse a program with `.input events(io="nonexistent", filename="x")`.
 * Call wl_session_load_input_files. Assert it returns an error
 * (non-zero rc).
 */
static void
test_dispatch_unknown_scheme_error(void)
{
    TEST("dispatch_unknown_scheme_error");
#ifdef TEST_DISPATCH_PRESENT
    /* TODO(#458): Full implementation when dispatch rewrite lands.
     *
     * Parse+compile: .input events(io="nonexistent", filename="x")
     * int rc = wl_session_load_input_files(session)
     * Assert rc != 0
     */
    FAIL("dispatch rewrite not yet implemented (#458)");
#else
    SKIP("dispatch rewrite not yet implemented (#458)");
#endif
}

/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int main(void) {
    printf("=== test_io_dispatch ===\n");
    test_dispatch_mock_adapter();
    test_dispatch_csv_default();
    test_dispatch_unknown_scheme_error();
    printf("=== Results: %d passed, %d failed, %d skipped ===\n",
        passed, failed, skipped);
    return failed > 0 ? 1 : 0;
}
