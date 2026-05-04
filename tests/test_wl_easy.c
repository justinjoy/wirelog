/*
 * test_wl_easy.c - Unit tests for the wl_easy convenience facade (Issue #441)
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 */

#define _POSIX_C_SOURCE 200809L

#include "../wirelog/wl_easy.h"
#include "../wirelog/util/log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

/* ======================================================================== */
/* Test Harness                                                             */
/* ======================================================================== */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                            \
        do {                                      \
            tests_run++;                          \
            printf("  [%d] %s", tests_run, name); \
        } while (0)
#define PASS()                 \
        do {                       \
            tests_passed++;        \
            printf(" ... PASS\n"); \
        } while (0)
#define FAIL(msg)                         \
        do {                                  \
            tests_failed++;                   \
            printf(" ... FAIL: %s\n", (msg)); \
        } while (0)
#define SKIP(msg)                         \
        do {                                  \
            printf(" ... SKIP: %s\n", (msg)); \
        } while (0)

/* ======================================================================== */
/* Shared Datalog Programs                                                  */
/* ======================================================================== */

static const char *ACCESS_CONTROL_SRC
    = ".decl can(user: symbol, perm: symbol)\n"
    ".decl granted(user: symbol, perm: symbol)\n"
    "granted(U, P) :- can(U, P).\n";

/* ======================================================================== */
/* Delta Collector                                                          */
/* ======================================================================== */

#define MAX_DELTAS 64
#define MAX_COLS 8

typedef struct {
    int count;
    char relations[MAX_DELTAS][32];
    int64_t rows[MAX_DELTAS][MAX_COLS];
    uint32_t ncols[MAX_DELTAS];
    int32_t diffs[MAX_DELTAS];
} delta_collector_t;

static void
collect_delta(const char *relation, const int64_t *row, uint32_t ncols,
    int32_t diff, void *user_data)
{
    delta_collector_t *c = (delta_collector_t *)user_data;
    if (c->count >= MAX_DELTAS)
        return;
    int idx = c->count++;
    strncpy(c->relations[idx], relation, 31);
    c->relations[idx][31] = '\0';
    c->ncols[idx] = ncols;
    c->diffs[idx] = diff;
    for (uint32_t i = 0; i < ncols && i < MAX_COLS; i++)
        c->rows[idx][i] = row[i];
}

/* ======================================================================== */
/* Tuple Collector                                                          */
/* ======================================================================== */

typedef struct {
    int count;
    char relations[MAX_DELTAS][32];
    int64_t rows[MAX_DELTAS][MAX_COLS];
    uint32_t ncols[MAX_DELTAS];
} tuple_collector_t;

static void
collect_tuple(const char *relation, const int64_t *row, uint32_t ncols,
    void *user_data)
{
    tuple_collector_t *c = (tuple_collector_t *)user_data;
    if (c->count >= MAX_DELTAS)
        return;
    int idx = c->count++;
    strncpy(c->relations[idx], relation, 31);
    c->relations[idx][31] = '\0';
    c->ncols[idx] = ncols;
    for (uint32_t i = 0; i < ncols && i < MAX_COLS; i++)
        c->rows[idx][i] = row[i];
}

static bool
drive_access_control_trace(wl_easy_session_t *s)
{
    int64_t alice = wl_easy_intern(s, "alice");
    int64_t read = wl_easy_intern(s, "read");
    if (alice < 0 || read < 0)
        return false;

    delta_collector_t deltas;
    memset(&deltas, 0, sizeof(deltas));
    if (wl_easy_set_delta_cb(s, collect_delta, &deltas) != WIRELOG_OK)
        return false;

    int64_t row[2] = { alice, read };
    if (wl_easy_insert(s, "can", row, 2) != WIRELOG_OK)
        return false;
    if (wl_easy_step(s) != WIRELOG_OK)
        return false;

    bool found_delta = false;
    for (int i = 0; i < deltas.count; i++) {
        if (strcmp(deltas.relations[i], "granted") == 0
            && deltas.ncols[i] == 2 && deltas.rows[i][0] == alice
            && deltas.rows[i][1] == read && deltas.diffs[i] == 1) {
            found_delta = true;
            break;
        }
    }
    if (!found_delta)
        return false;

    tuple_collector_t granted_t;
    memset(&granted_t, 0, sizeof(granted_t));
    if (wl_easy_snapshot(s, "granted", collect_tuple, &granted_t)
        != WIRELOG_OK)
        return false;

    for (int i = 0; i < granted_t.count; i++) {
        if (strcmp(granted_t.relations[i], "granted") == 0
            && granted_t.ncols[i] == 2 && granted_t.rows[i][0] == alice
            && granted_t.rows[i][1] == read)
            return true;
    }
    return false;
}

static bool
file_contains_substring(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

#ifndef _WIN32
static bool
capture_num_workers_log(const wl_easy_open_opts_t *opts, uint32_t expected)
{
    char path[128];
    snprintf(path, sizeof(path), "/tmp/wl-easy-num-test-%ld-%u.log",
        (long)getpid(), expected);
    unlink(path);

    char needle[32];
    snprintf(needle, sizeof(needle), "num_workers=%u", expected);

    setenv("WL_LOG", "SESSION:4", 1);
    wl_log_init();

    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) {
        unsetenv("WL_LOG");
        return false;
    }

    int log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0) {
        close(saved_stderr);
        unsetenv("WL_LOG");
        return false;
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        close(log_fd);
        close(saved_stderr);
        unsetenv("WL_LOG");
        return false;
    }
    close(log_fd);

    wl_easy_session_t *s = NULL;
    wirelog_error_t open_rc = wl_easy_open_opts(ACCESS_CONTROL_SRC, opts, &s);
    wirelog_error_t build_rc = WIRELOG_ERR_EXEC;
    if (open_rc == WIRELOG_OK && s)
        build_rc = wl_easy_set_delta_cb(s, NULL, NULL);

    fflush(stderr);
    bool restored = dup2(saved_stderr, STDERR_FILENO) >= 0;
    close(saved_stderr);

    bool found = file_contains_substring(path, needle);
    unsetenv("WL_LOG");
    wl_log_init();
    if (s)
        wl_easy_close(s);
    unlink(path);

    return restored && open_rc == WIRELOG_OK && build_rc == WIRELOG_OK
           && found;
}
#endif

/* ======================================================================== */
/* Tests                                                                    */
/* ======================================================================== */

static void
test_open_close_null_safe(void)
{
    TEST("open NULL src + NULL out + close(NULL) safe");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(NULL, &s) == WIRELOG_OK) {
        FAIL("expected non-OK on NULL src");
        return;
    }
    if (wl_easy_open(ACCESS_CONTROL_SRC, NULL) == WIRELOG_OK) {
        FAIL("expected non-OK on NULL out");
        return;
    }
    /* Must not crash */
    wl_easy_close(NULL);
    PASS();
}

static void
test_open_parse_error(void)
{
    TEST("open invalid Datalog returns error");

    wl_easy_session_t *s = (wl_easy_session_t *)0xdeadbeef;
    wirelog_error_t rc = wl_easy_open("this is not datalog ::: !!!", &s);
    if (rc == WIRELOG_OK) {
        FAIL("parse should have failed");
        return;
    }
    if (s != NULL) {
        FAIL("*out should be NULL on error");
        return;
    }
    PASS();
}

static void
test_open_opts_null_equiv_to_open(void)
{
    TEST("open_opts NULL opts equivalent to open");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open_opts(ACCESS_CONTROL_SRC, NULL, &s) != WIRELOG_OK || !s) {
        FAIL("open_opts failed");
        return;
    }
    if (!drive_access_control_trace(s)) {
        FAIL("access-control trace failed");
        wl_easy_close(s);
        return;
    }
    wl_easy_close(s);
    PASS();
}

static void
test_open_opts_zero_size_rejected(void)
{
    TEST("open_opts rejects zero size");

    wl_easy_open_opts_t opts = { 0 };
    wl_easy_session_t *s = (wl_easy_session_t *)0xdeadbeef;
    wirelog_error_t rc = wl_easy_open_opts(ACCESS_CONTROL_SRC, &opts, &s);
    if (rc != WIRELOG_ERR_EXEC) {
        FAIL("expected WIRELOG_ERR_EXEC");
        return;
    }
    if (s != NULL) {
        FAIL("*out should be NULL on error");
        return;
    }
    PASS();
}

static void
test_open_opts_reserved_rejected(void)
{
    TEST("open_opts rejects reserved field before parsing");

    wl_easy_open_opts_t opts = WL_EASY_OPEN_OPTS_INIT;
    opts._reserved = (const void *)0x1;
    wl_easy_session_t *s = (wl_easy_session_t *)0xdeadbeef;
    wirelog_error_t rc
        = wl_easy_open_opts("this is not datalog", &opts, &s);
    if (rc != WIRELOG_ERR_EXEC) {
        FAIL("expected WIRELOG_ERR_EXEC");
        return;
    }
    if (s != NULL) {
        FAIL("*out should be NULL on error");
        return;
    }
    PASS();
}

static void
test_open_opts_init_macro(void)
{
    TEST("open_opts init macro sets defaults");

    wl_easy_open_opts_t opts = WL_EASY_OPEN_OPTS_INIT;
    if (opts.size != sizeof(wl_easy_open_opts_t)) {
        FAIL("unexpected size");
        return;
    }
    if (opts.num_workers != 0) {
        FAIL("unexpected num_workers");
        return;
    }
    if (opts.eager_build) {
        FAIL("unexpected eager_build");
        return;
    }
    if (opts._reserved != NULL) {
        FAIL("unexpected _reserved");
        return;
    }
    PASS();
}

static void
test_open_opts_eager_build_ok(void)
{
    TEST("open_opts eager_build opens usable session");

    wl_easy_open_opts_t opts = WL_EASY_OPEN_OPTS_INIT;
    opts.eager_build = true;
    wl_easy_session_t *s = NULL;
    if (wl_easy_open_opts(ACCESS_CONTROL_SRC, &opts, &s) != WIRELOG_OK || !s) {
        FAIL("open_opts eager_build failed");
        return;
    }
    if (!drive_access_control_trace(s)) {
        FAIL("access-control trace failed");
        wl_easy_close(s);
        return;
    }
    wl_easy_close(s);
    PASS();
}

/*
 * The codebase has no fixture for "parses-clean-but-plans-dirty" today, so
 * the eager-build error contract is currently exercised only via the
 * parse-error path (per the architect+critic synthesis plan, scope-narrow per
 * Critic MAJOR #6).
 */
static void
test_open_opts_eager_build_propagates_parse_error(void)
{
    TEST("open_opts eager_build propagates parse error");

    wl_easy_open_opts_t opts = WL_EASY_OPEN_OPTS_INIT;
    opts.eager_build = true;
    wl_easy_session_t *s = (wl_easy_session_t *)0xdeadbeef;
    wirelog_error_t rc
        = wl_easy_open_opts("definitely not datalog", &opts, &s);
    if (rc != WIRELOG_ERR_PARSE) {
        FAIL("expected WIRELOG_ERR_PARSE");
        return;
    }
    if (s != NULL) {
        FAIL("*out should be NULL on error");
        return;
    }
    PASS();
}

static void
test_num_workers_default_is_one(void)
{
    TEST("open_opts default num_workers logs one");

#ifdef _WIN32
    SKIP("POSIX fd redirection not available on Windows");
    return;
#else
    if (!capture_num_workers_log(NULL, 1)) {
        FAIL("expected num_workers=1 log");
        return;
    }
    PASS();
#endif
}

static void
test_num_workers_explicit_four(void)
{
    TEST("open_opts explicit num_workers logs four");

#ifdef _WIN32
    SKIP("POSIX fd redirection not available on Windows");
    return;
#else
    wl_easy_open_opts_t opts = WL_EASY_OPEN_OPTS_INIT;
    opts.num_workers = 4;
    if (!capture_num_workers_log(&opts, 4)) {
        FAIL("expected num_workers=4 log");
        return;
    }
    PASS();
#endif
}

static void
test_intern_returns_same_id(void)
{
    TEST("intern same string returns same id");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }
    int64_t a = wl_easy_intern(s, "alice");
    int64_t b = wl_easy_intern(s, "alice");
    if (a < 0 || b < 0 || a != b) {
        FAIL("intern returned inconsistent ids");
        wl_easy_close(s);
        return;
    }
    wl_easy_close(s);
    PASS();
}

static void
test_insert_step_delta(void)
{
    TEST("insert + step fires delta callback");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }
    int64_t alice = wl_easy_intern(s, "alice");
    int64_t read = wl_easy_intern(s, "read");
    if (alice < 0 || read < 0) {
        FAIL("intern failed");
        wl_easy_close(s);
        return;
    }

    delta_collector_t deltas;
    memset(&deltas, 0, sizeof(deltas));
    wl_easy_set_delta_cb(s, collect_delta, &deltas);

    int64_t row[2] = { alice, read };
    if (wl_easy_insert(s, "can", row, 2) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }
    if (wl_easy_step(s) != WIRELOG_OK) {
        FAIL("step failed");
        wl_easy_close(s);
        return;
    }

    bool found = false;
    for (int i = 0; i < deltas.count; i++) {
        if (strcmp(deltas.relations[i], "granted") == 0
            && deltas.ncols[i] == 2 && deltas.rows[i][0] == alice
            && deltas.rows[i][1] == read && deltas.diffs[i] == 1) {
            found = true;
            break;
        }
    }
    wl_easy_close(s);
    if (!found) {
        FAIL("expected +granted(alice,read) delta not seen");
        return;
    }
    PASS();
}

static void
test_inline_compound_body_binding(void)
{
    TEST("inline compound body pattern binds public Datalog variables");

    const char *src
        = ".decl event(id: int64, payload: metadata/4 inline)\n"
        ".decl hot(id: int64)\n"
        "hot(ID) :- event(ID, metadata(Level, Ts, Host, Risk)), Risk > 80.\n";

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(src, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    int64_t hot_row[5] = { 7, 1, 2, 3, 90 };
    int64_t cold_row[5] = { 8, 1, 2, 3, 40 };
    if (wl_easy_insert(s, "event", hot_row, 5) != WIRELOG_OK
        || wl_easy_insert(s, "event", cold_row, 5) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }

    tuple_collector_t hot;
    memset(&hot, 0, sizeof(hot));
    if (wl_easy_snapshot(s, "hot", collect_tuple, &hot) != WIRELOG_OK) {
        FAIL("snapshot failed");
        wl_easy_close(s);
        return;
    }

    wl_easy_close(s);

    if (hot.count != 1 || strcmp(hot.relations[0], "hot") != 0
        || hot.ncols[0] != 1 || hot.rows[0][0] != 7) {
        FAIL("expected only hot(7)");
        return;
    }
    PASS();
}

static void
test_inline_compound_body_join_binding(void)
{
    TEST("inline compound body variables participate in joins");

    const char *src
        = ".decl event(id: int64, payload: metadata/4 inline)\n"
        ".decl threshold(risk: int64)\n"
        ".decl hot(id: int64)\n"
        "hot(ID) :- event(ID, metadata(Level, Ts, Host, Risk)), "
        "threshold(Risk).\n";

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(src, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    int64_t matched_row[5] = { 7, 1, 2, 3, 90 };
    int64_t unmatched_row[5] = { 8, 1, 2, 3, 40 };
    int64_t threshold_row[1] = { 90 };
    if (wl_easy_insert(s, "event", matched_row, 5) != WIRELOG_OK
        || wl_easy_insert(s, "event", unmatched_row, 5) != WIRELOG_OK
        || wl_easy_insert(s, "threshold", threshold_row, 1) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }

    tuple_collector_t hot;
    memset(&hot, 0, sizeof(hot));
    if (wl_easy_snapshot(s, "hot", collect_tuple, &hot) != WIRELOG_OK) {
        FAIL("snapshot failed");
        wl_easy_close(s);
        return;
    }

    wl_easy_close(s);

    if (hot.count != 1 || strcmp(hot.relations[0], "hot") != 0
        || hot.ncols[0] != 1 || hot.rows[0][0] != 7) {
        FAIL("expected join to derive only hot(7)");
        return;
    }
    PASS();
}

static void
test_inline_compound_functor_mismatch_is_empty(void)
{
    TEST("inline compound functor mismatch does not match");

    const char *src
        = ".decl event(id: int64, payload: metadata/4 inline)\n"
        ".decl bad(id: int64)\n"
        "bad(ID) :- event(ID, other(Level, Ts, Host, Risk)).\n";

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(src, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    int64_t row[5] = { 7, 1, 2, 3, 90 };
    if (wl_easy_insert(s, "event", row, 5) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }

    tuple_collector_t bad;
    memset(&bad, 0, sizeof(bad));
    if (wl_easy_snapshot(s, "bad", collect_tuple, &bad) != WIRELOG_OK) {
        FAIL("snapshot failed");
        wl_easy_close(s);
        return;
    }

    wl_easy_close(s);

    if (bad.count != 0) {
        FAIL("mismatched functor should derive no rows");
        return;
    }
    PASS();
}

static void
test_inline_compound_constant_child_filters(void)
{
    TEST("inline compound constant child filters rows");

    const char *src
        = ".decl event(id: int64, payload: metadata/4 inline)\n"
        ".decl hot(id: int64)\n"
        "hot(ID) :- event(ID, metadata(_, _, _, 90)).\n";

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(src, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    int64_t hot_row[5] = { 7, 1, 2, 3, 90 };
    int64_t cold_row[5] = { 8, 1, 2, 3, 40 };
    if (wl_easy_insert(s, "event", hot_row, 5) != WIRELOG_OK
        || wl_easy_insert(s, "event", cold_row, 5) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }

    tuple_collector_t hot;
    memset(&hot, 0, sizeof(hot));
    if (wl_easy_snapshot(s, "hot", collect_tuple, &hot) != WIRELOG_OK) {
        FAIL("snapshot failed");
        wl_easy_close(s);
        return;
    }

    wl_easy_close(s);

    if (hot.count != 1 || hot.ncols[0] != 1 || hot.rows[0][0] != 7) {
        FAIL("expected only row with constant child value 90");
        return;
    }
    PASS();
}

static void
test_inline_compound_duplicate_child_variables_filter(void)
{
    TEST("inline compound duplicate child variables filter rows");

    const char *src
        = ".decl event(id: int64, payload: metadata/4 inline)\n"
        ".decl same(id: int64)\n"
        "same(ID) :- event(ID, metadata(X, X, _, _)).\n";

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(src, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    int64_t matched_row[5] = { 7, 4, 4, 3, 90 };
    int64_t unmatched_row[5] = { 8, 1, 2, 3, 90 };
    if (wl_easy_insert(s, "event", matched_row, 5) != WIRELOG_OK
        || wl_easy_insert(s, "event", unmatched_row, 5) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }

    tuple_collector_t same;
    memset(&same, 0, sizeof(same));
    if (wl_easy_snapshot(s, "same", collect_tuple, &same) != WIRELOG_OK) {
        FAIL("snapshot failed");
        wl_easy_close(s);
        return;
    }

    wl_easy_close(s);

    if (same.count != 1 || same.ncols[0] != 1 || same.rows[0][0] != 7) {
        FAIL("expected only row with equal duplicate child variables");
        return;
    }
    PASS();
}

static void
test_insert_sym_variadic(void)
{
    TEST("insert_sym variadic helper");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    delta_collector_t deltas;
    memset(&deltas, 0, sizeof(deltas));
    wl_easy_set_delta_cb(s, collect_delta, &deltas);

    if (wl_easy_insert_sym(s, "can", "alice", "read", (const char *)NULL)
        != WIRELOG_OK) {
        FAIL("insert_sym failed");
        wl_easy_close(s);
        return;
    }
    if (wl_easy_step(s) != WIRELOG_OK) {
        FAIL("step failed");
        wl_easy_close(s);
        return;
    }
    bool found = false;
    for (int i = 0; i < deltas.count; i++) {
        if (strcmp(deltas.relations[i], "granted") == 0
            && deltas.diffs[i] == 1) {
            found = true;
            break;
        }
    }
    wl_easy_close(s);
    if (!found) {
        FAIL("no granted delta after insert_sym");
        return;
    }
    PASS();
}

static void
test_remove_sym(void)
{
    TEST("remove_sym fires negative delta");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    delta_collector_t deltas;
    memset(&deltas, 0, sizeof(deltas));
    wl_easy_set_delta_cb(s, collect_delta, &deltas);

    if (wl_easy_insert_sym(s, "can", "alice", "read", (const char *)NULL)
        != WIRELOG_OK) {
        FAIL("insert_sym failed");
        wl_easy_close(s);
        return;
    }
    if (wl_easy_step(s) != WIRELOG_OK) {
        FAIL("step 1 failed");
        wl_easy_close(s);
        return;
    }
    int after_step1 = deltas.count;

    if (wl_easy_remove_sym(s, "can", "alice", "read", (const char *)NULL)
        != WIRELOG_OK) {
        FAIL("remove_sym failed");
        wl_easy_close(s);
        return;
    }
    if (wl_easy_step(s) != WIRELOG_OK) {
        FAIL("step 2 failed");
        wl_easy_close(s);
        return;
    }

    bool found_neg = false;
    for (int i = after_step1; i < deltas.count; i++) {
        if (strcmp(deltas.relations[i], "granted") == 0
            && deltas.diffs[i] == -1) {
            found_neg = true;
            break;
        }
    }
    wl_easy_close(s);
    if (!found_neg) {
        FAIL("no -granted delta after remove_sym + step");
        return;
    }
    PASS();
}

static void
test_snapshot_filter(void)
{
    TEST("snapshot filters by relation name");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }

    if (wl_easy_insert_sym(s, "can", "alice", "read", (const char *)NULL)
        != WIRELOG_OK
        || wl_easy_insert_sym(s, "can", "bob", "write", (const char *)NULL)
        != WIRELOG_OK) {
        FAIL("insert_sym failed");
        wl_easy_close(s);
        return;
    }
    /* NOTE: Do NOT call wl_easy_step() before wl_easy_snapshot().  The
     * columnar backend's snapshot path re-evaluates all strata and appends
     * to the IDB relation rows; a prior step() already derived the IDB
     * tuples, so combining the two would double-count.  See the doc
     * comment on wl_easy_snapshot() in wl_easy.h. */

    tuple_collector_t granted_t;
    memset(&granted_t, 0, sizeof(granted_t));
    if (wl_easy_snapshot(s, "granted", collect_tuple, &granted_t)
        != WIRELOG_OK) {
        FAIL("snapshot granted failed");
        wl_easy_close(s);
        return;
    }

    tuple_collector_t can_t;
    memset(&can_t, 0, sizeof(can_t));
    if (wl_easy_snapshot(s, "can", collect_tuple, &can_t) != WIRELOG_OK) {
        FAIL("snapshot can failed");
        wl_easy_close(s);
        return;
    }

    wl_easy_close(s);

    if (granted_t.count != 2) {
        FAIL("expected 2 granted tuples in snapshot");
        return;
    }
    /* Filter must reject tuples whose relation != "granted" */
    for (int i = 0; i < granted_t.count; i++) {
        if (strcmp(granted_t.relations[i], "granted") != 0) {
            FAIL("granted snapshot leaked non-granted tuple");
            return;
        }
    }
    for (int i = 0; i < can_t.count; i++) {
        if (strcmp(can_t.relations[i], "can") != 0) {
            FAIL("can snapshot leaked non-can tuple");
            return;
        }
    }
    PASS();
}

static void
test_print_delta_integer_column(void)
{
    TEST("print_delta on integer column does not abort");

    static const char *SRC = ".decl a(x: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x).\n";
    wl_easy_session_t *s = NULL;
    if (wl_easy_open(SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }
    wl_easy_set_delta_cb(s, wl_easy_print_delta, s);
    int64_t row[1] = { 42 };
    if (wl_easy_insert(s, "a", row, 1) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }
    if (wl_easy_step(s) != WIRELOG_OK) {
        FAIL("step failed");
        wl_easy_close(s);
        return;
    }
    wl_easy_close(s);
    PASS();
}

static void
test_print_delta_unknown_relation_integer_fallback(void)
{
    TEST("print_delta on unknown relation falls back to integer rendering");

#ifdef _WIN32
    SKIP("fork not available on Windows");
    return;
#else
    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: exits 0 only if print_delta completes without aborting.
         * We pass a relation name that the program does NOT declare, so
         * wirelog_program_get_schema() returns NULL.  Pre-fix, the
         * fallback set as_string=true for every column and the ids below
         * would trigger abort() on reverse-intern.  Post-fix, the
         * schema-less branch renders raw int64 values and returns
         * cleanly. */
        fclose(stdout);
        fclose(stderr);

        wl_easy_session_t *s = NULL;
        if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s)
            _exit(2);
        int64_t row[2] = { 123456789, 987654321 };
        wl_easy_print_delta("no_such_relation", row, 2, 1, s);
        wl_easy_close(s);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        FAIL("waitpid failed");
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAIL("print_delta aborted on schema-unavailable path");
        return;
    }
    PASS();
#endif
}

static void
test_print_delta_abort_on_missed_symbol(void)
{
    TEST("print_delta aborts on missed reverse-intern");

#ifdef _WIN32
    SKIP("fork not available on Windows");
    return;
#else
    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork failed");
        return;
    }
    if (pid == 0) {
        /* Child: silence stdio so the abort message does not pollute
         * the parent's test log. */
        fclose(stdout);
        fclose(stderr);

        wl_easy_session_t *s = NULL;
        if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s)
            _exit(2);
        wl_easy_set_delta_cb(s, wl_easy_print_delta, s);
        /* Bogus, never-interned ids — printer must abort. */
        int64_t row[2] = { 999999, 888888 };
        wl_easy_insert(s, "can", row, 2);
        wl_easy_step(s);
        wl_easy_close(s);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        FAIL("waitpid failed");
        return;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        FAIL("child did not abort as expected");
        return;
    }
    PASS();
#endif
}

static void
test_cleanup_order_no_use_after_free(void)
{
    TEST("open/use/close repeated has no leaks");

    for (int iter = 0; iter < 2; iter++) {
        wl_easy_session_t *s = NULL;
        if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
            FAIL("open failed");
            return;
        }
        if (wl_easy_insert_sym(s, "can", "alice", "read", (const char *)NULL)
            != WIRELOG_OK) {
            FAIL("insert_sym failed");
            wl_easy_close(s);
            return;
        }
        if (wl_easy_step(s) != WIRELOG_OK) {
            FAIL("step failed");
            wl_easy_close(s);
            return;
        }
        wl_easy_close(s);
    }
    PASS();
}

static void
test_intern_after_step_succeeds(void)
{
    TEST("intern after first step still succeeds (Option B contract)");

    wl_easy_session_t *s = NULL;
    if (wl_easy_open(ACCESS_CONTROL_SRC, &s) != WIRELOG_OK || !s) {
        FAIL("open failed");
        return;
    }
    int64_t alice = wl_easy_intern(s, "alice");
    int64_t read = wl_easy_intern(s, "read");
    if (alice < 0 || read < 0) {
        FAIL("intern failed");
        wl_easy_close(s);
        return;
    }
    int64_t row[2] = { alice, read };
    if (wl_easy_insert(s, "can", row, 2) != WIRELOG_OK) {
        FAIL("insert failed");
        wl_easy_close(s);
        return;
    }
    if (wl_easy_step(s) != WIRELOG_OK) {
        FAIL("step failed");
        wl_easy_close(s);
        return;
    }
    /* After the plan has been built and stepped, interning a brand new
     * symbol must still succeed and return a fresh id, because the intern
     * table is aliased through the whole session lifetime. */
    int64_t late = wl_easy_intern(s, "late_symbol");
    /* And a new insert using that id must also succeed, proving the id is
     * actually visible to the running backend. */
    int64_t late_row[2] = { late, read };
    wirelog_error_t ins_rc
        = wl_easy_insert(s, "can", late_row, 2);
    wirelog_error_t step_rc = wl_easy_step(s);
    wl_easy_close(s);
    if (late < 0) {
        FAIL("late intern should have returned a non-negative id");
        return;
    }
    if (ins_rc != WIRELOG_OK || step_rc != WIRELOG_OK) {
        FAIL("insert/step using late-interned id failed");
        return;
    }
    PASS();
}

/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int
main(void)
{
    printf("wl_easy Tests (Issue #441)\n");
    printf("==========================\n\n");

    test_open_close_null_safe();
    test_open_parse_error();
    test_open_opts_null_equiv_to_open();
    test_open_opts_zero_size_rejected();
    test_open_opts_reserved_rejected();
    test_open_opts_init_macro();
    test_open_opts_eager_build_ok();
    test_open_opts_eager_build_propagates_parse_error();
    test_num_workers_default_is_one();
    test_num_workers_explicit_four();
    test_intern_returns_same_id();
    test_insert_step_delta();
    test_inline_compound_body_binding();
    test_inline_compound_body_join_binding();
    test_inline_compound_functor_mismatch_is_empty();
    test_inline_compound_constant_child_filters();
    test_inline_compound_duplicate_child_variables_filter();
    test_insert_sym_variadic();
    test_remove_sym();
    test_snapshot_filter();
    test_print_delta_integer_column();
    test_print_delta_unknown_relation_integer_fallback();
    test_print_delta_abort_on_missed_symbol();
    test_cleanup_order_no_use_after_free();
    test_intern_after_step_succeeds();

    printf("\nPassed: %d/%d\n", tests_passed, tests_run);
    printf("Failed: %d/%d\n", tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
