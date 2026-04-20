/*
 * test_bench_argv.c - Unit tests for bench/bench_argv.h (issue #508).
 *
 * Covers every documented-supported shape plus every documented-rejected
 * shape. Redirects stderr to a temp buffer during error-path tests so the
 * test output stays clean.
 */

#include "../bench/bench_argv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
test_short_space_separated(void)
{
    char *argv[] = { (char *)"prog", (char *)"-x", (char *)"foo", NULL };
    int argc = 3;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "x:", NULL, &st);
    assert(opt == 'x');
    assert(st.optarg != NULL && strcmp(st.optarg, "foo") == 0);
    assert(st.optind == 3);
    opt = bench_argv_next(argc, argv, "x:", NULL, &st);
    assert(opt == -1);
}

static void
test_short_glued(void)
{
    char *argv[] = { (char *)"prog", (char *)"-xfoo", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "x:", NULL, &st);
    assert(opt == 'x');
    assert(strcmp(st.optarg, "foo") == 0);
    assert(st.optind == 2);
}

static void
test_long_space_separated(void)
{
    bench_argv_long_t longs[] = {
        { "name", required_argument, 'n' },
        { NULL, 0, 0 },
    };
    char *argv[] = { (char *)"prog", (char *)"--name", (char *)"foo", NULL };
    int argc = 3;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "", longs, &st);
    assert(opt == 'n');
    assert(strcmp(st.optarg, "foo") == 0);
    assert(st.optind == 3);
}

static void
test_long_bare_flag(void)
{
    bench_argv_long_t longs[] = {
        { "verbose", no_argument, 'v' },
        { NULL, 0, 0 },
    };
    char *argv[] = { (char *)"prog", (char *)"--verbose", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "", longs, &st);
    assert(opt == 'v');
    assert(st.optarg == NULL);
    assert(st.optind == 2);
}

static void
test_double_dash_terminator(void)
{
    char *argv[] = {
        (char *)"prog", (char *)"-x", (char *)"foo",
        (char *)"--", (char *)"-y", NULL,
    };
    int argc = 5;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "x:y", NULL, &st);
    assert(opt == 'x');
    opt = bench_argv_next(argc, argv, "x:y", NULL, &st);
    assert(opt == -1);
    /* "--" should have been consumed; -y after it is untouched residue. */
    assert(st.optind == 4);
}

/* Redirect stderr to an in-memory sink so error paths do not pollute test
 * stdout. We do not assert message content -- only return codes. */
static void
redirect_stderr_to_devnull_(void)
{
#if defined(_WIN32)
    (void)freopen("NUL", "w", stderr);
#else
    (void)freopen("/dev/null", "w", stderr);
#endif
}

static void
test_unknown_short(void)
{
    redirect_stderr_to_devnull_();
    char *argv[] = { (char *)"prog", (char *)"-q", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "x:", NULL, &st);
    assert(opt == '?');
}

static void
test_unknown_long(void)
{
    redirect_stderr_to_devnull_();
    bench_argv_long_t longs[] = {
        { "known", no_argument, 'k' },
        { NULL, 0, 0 },
    };
    char *argv[] = { (char *)"prog", (char *)"--bogus", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "", longs, &st);
    assert(opt == '?');
}

static void
test_missing_short_arg(void)
{
    redirect_stderr_to_devnull_();
    char *argv[] = { (char *)"prog", (char *)"-x", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "x:", NULL, &st);
    assert(opt == '?');
}

static void
test_missing_long_arg(void)
{
    redirect_stderr_to_devnull_();
    bench_argv_long_t longs[] = {
        { "name", required_argument, 'n' },
        { NULL, 0, 0 },
    };
    char *argv[] = { (char *)"prog", (char *)"--name", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "", longs, &st);
    assert(opt == '?');
}

static void
test_help_short(void)
{
    char *argv[] = { (char *)"prog", (char *)"-h", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "h", NULL, &st);
    assert(opt == 'h');
    assert(st.optarg == NULL);
}

static void
test_empty_argv(void)
{
    char *argv[] = { (char *)"prog", NULL };
    int argc = 1;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "x:", NULL, &st);
    assert(opt == -1);
    assert(st.optind == 1);
}

static void
test_reentry_fresh_state(void)
{
    /* Two back-to-back parses with fresh state should yield the same
     * sequence -- proves there is no hidden static state. */
    char *argv[] = { (char *)"prog", (char *)"-x", (char *)"a",
                     (char *)"-x", (char *)"b", NULL };
    int argc = 5;

    bench_argv_state_t st1 = BENCH_ARGV_INIT;
    int o1a = bench_argv_next(argc, argv, "x:", NULL, &st1);
    int o1b = bench_argv_next(argc, argv, "x:", NULL, &st1);
    assert(o1a == 'x' && strcmp(st1.optarg, "b") == 0);
    (void)o1b;

    bench_argv_state_t st2 = BENCH_ARGV_INIT;
    int o2a = bench_argv_next(argc, argv, "x:", NULL, &st2);
    assert(o2a == 'x');
    assert(strcmp(st2.optarg, "a") == 0); /* fresh state starts over */
}

static void
test_bundled_short_rejected(void)
{
    redirect_stderr_to_devnull_();
    char *argv[] = { (char *)"prog", (char *)"-ab", NULL };
    int argc = 2;
    bench_argv_state_t st = BENCH_ARGV_INIT;
    int opt = bench_argv_next(argc, argv, "ab", NULL, &st);
    assert(opt == '?'); /* bundled flags fail loud */
}

int
main(void)
{
    test_short_space_separated();
    test_short_glued();
    test_long_space_separated();
    test_long_bare_flag();
    test_double_dash_terminator();
    test_unknown_short();
    test_unknown_long();
    test_missing_short_arg();
    test_missing_long_arg();
    test_help_short();
    test_empty_argv();
    test_reentry_fresh_state();
    test_bundled_short_rejected();
    /* stderr was redirected; restore via printf to stdout which is the
     * test runner's success channel. */
    printf("test_bench_argv OK\n");
    return 0;
}
