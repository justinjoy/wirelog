/*
 * tests/test_arena_observability.c - WL_LOG ARENA observability (Issue #558).
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Drives the ARENA section of the structured logger end-to-end. Each test
 * configures WL_LOG with a section-specific level for ARENA, routes output
 * to a tmpfile via WL_LOG_FILE, exercises the relevant arena entry points,
 * and asserts the emitted text shape.
 *
 *   test_arena_alloc_reset_logged    -- DEBUG-level alloc + INFO-level reset
 *                                       lines appear with size/capacity tags.
 *   test_arena_free_warn_when_used   -- free() with outstanding bytes emits
 *                                       a WARN line.
 *   test_compound_arena_gc_logged    -- gc_epoch_boundary INFO line carries
 *                                       epoch + freed_handles + remaining;
 *                                       handle_alloc TRACE line is present
 *                                       at TRACE level.
 *   test_observability_gates         -- WL_LOG unset => zero output.
 */

#define _POSIX_C_SOURCE 200809L

#include "../wirelog/arena/arena.h"
#include "../wirelog/arena/compound_arena.h"
#include "../wirelog/util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER) && !defined(__clang__)
#  include <process.h>
static int
wl_test_setenv_(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
    return _putenv_s(name, (value && *value) ? value : "1");
}
static int
wl_test_unsetenv_(const char *name)
{
    return _putenv_s(name, "");
}
#  define setenv   wl_test_setenv_
#  define unsetenv wl_test_unsetenv_
#  define getpid   _getpid
#else
#  include <unistd.h>
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                              \
        do {                                        \
            tests_run++;                            \
            printf("  [%d] %s", tests_run, name);   \
        } while (0)

#define PASS()                                  \
        do {                                        \
            tests_passed++;                         \
            printf(" ... PASS\n");                  \
        } while (0)

#define FAIL(msg)                               \
        do {                                        \
            tests_failed++;                         \
            printf(" ... FAIL: %s\n", (msg));       \
            goto cleanup;                           \
        } while (0)

#define ASSERT(cond, msg)                       \
        do {                                        \
            if (!(cond)) {                          \
                FAIL(msg);                          \
            }                                       \
        } while (0)

static char tmp_path_[256];

static const char *
tmpdir_(void)
{
    const char *d = getenv("TMPDIR");
    if (d && *d) return d;
#if defined(_WIN32)
    d = getenv("TEMP");
    if (d && *d) return d;
    d = getenv("TMP");
    if (d && *d) return d;
    return ".";
#else
    return "/tmp";
#endif
}

static void
make_tmpfile_(const char *tag)
{
    const char *d = tmpdir_();
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    snprintf(tmp_path_, sizeof(tmp_path_),
        "%s%cwl_arena_obs_%s_%ld_%ld.log",
        d, sep, tag, (long)getpid(), (long)time(NULL));
    (void)remove(tmp_path_);
}

static size_t
read_file_(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

static void
clear_env_(void)
{
    unsetenv("WL_LOG");
    unsetenv("WL_LOG_FILE");
    unsetenv("WL_DEBUG_JOIN");
    unsetenv("WL_CONSOLIDATION_LOG");
}

static void
test_arena_alloc_reset_logged(void)
{
    TEST("arena: alloc(DEBUG) + reset(INFO) lines emitted at ARENA:4");
    wl_arena_t *arena = NULL;

    clear_env_();
    make_tmpfile_("alloc");
    setenv("WL_LOG_FILE", tmp_path_, 1);
    setenv("WL_LOG", "ARENA:4", 1);
    wl_log_init();

    arena = wl_arena_create(1024);
    ASSERT(arena != NULL, "arena_create failed");

    void *p = wl_arena_alloc(arena, 64);
    ASSERT(p != NULL, "alloc failed");

    wl_arena_reset(arena);
    wl_arena_free(arena);
    arena = NULL;

    wl_log_shutdown();

    char buf[2048] = {0};
    size_t n = read_file_(tmp_path_, buf, sizeof(buf));
    ASSERT(n > 0, "log file empty");
    ASSERT(strstr(buf, "[DEBUG][ARENA]") != NULL,
        "missing [DEBUG][ARENA] alloc line");
    ASSERT(strstr(buf, "alloc(size=64") != NULL,
        "missing alloc size tag");
    ASSERT(strstr(buf, "capacity=1024") != NULL,
        "missing capacity tag");
    ASSERT(strstr(buf, "[INFO][ARENA]") != NULL,
        "missing [INFO][ARENA] reset line");
    ASSERT(strstr(buf, "reset(used=") != NULL,
        "missing reset(used=...) tag");

    PASS();
cleanup:
    if (arena) wl_arena_free(arena);
    (void)remove(tmp_path_);
    clear_env_();
}

static void
test_arena_free_warn_when_used(void)
{
    TEST("arena: free() with outstanding bytes emits WARN");
    wl_arena_t *arena = NULL;

    clear_env_();
    make_tmpfile_("freewarn");
    setenv("WL_LOG_FILE", tmp_path_, 1);
    setenv("WL_LOG", "ARENA:2", 1);
    wl_log_init();

    arena = wl_arena_create(512);
    ASSERT(arena != NULL, "arena_create failed");

    void *p = wl_arena_alloc(arena, 32);
    ASSERT(p != NULL, "alloc failed");

    wl_arena_free(arena);
    arena = NULL;

    wl_log_shutdown();

    char buf[1024] = {0};
    size_t n = read_file_(tmp_path_, buf, sizeof(buf));
    ASSERT(n > 0, "log file empty");
    ASSERT(strstr(buf, "[WARN][ARENA]") != NULL,
        "missing [WARN][ARENA] prefix");
    ASSERT(strstr(buf, "still allocated") != NULL,
        "missing 'still allocated' tag");
    /* DEBUG alloc must NOT appear under ARENA:2 (WARN ceiling). */
    ASSERT(strstr(buf, "[DEBUG][ARENA]") == NULL,
        "DEBUG line leaked past WARN ceiling");

    PASS();
cleanup:
    if (arena) wl_arena_free(arena);
    (void)remove(tmp_path_);
    clear_env_();
}

static void
test_compound_arena_gc_logged(void)
{
    TEST("compound arena: gc_epoch_boundary INFO + handle_alloc TRACE");
    wl_compound_arena_t *carena = NULL;

    clear_env_();
    make_tmpfile_("gc");
    setenv("WL_LOG_FILE", tmp_path_, 1);
    setenv("WL_LOG", "ARENA:5", 1);
    wl_log_init();

    carena = wl_compound_arena_create(0x12345u, 256u, 8u);
    ASSERT(carena != NULL, "compound_arena_create failed");

    uint64_t h1 = wl_compound_arena_alloc(carena, 16);
    uint64_t h2 = wl_compound_arena_alloc(carena, 24);
    ASSERT(h1 != 0u && h2 != 0u, "compound alloc failed");

    /* Drop one to zero so gc reports a freed handle. */
    ASSERT(wl_compound_arena_retain(carena, h2, -1) == 0, "retain failed");

    uint32_t reclaimed = wl_compound_arena_gc_epoch_boundary(carena);
    ASSERT(reclaimed >= 1u, "gc reported no reclaimed handles");

    wl_compound_arena_free(carena);
    carena = NULL;

    wl_log_shutdown();

    char buf[4096] = {0};
    size_t n = read_file_(tmp_path_, buf, sizeof(buf));
    ASSERT(n > 0, "log file empty");
    ASSERT(strstr(buf, "[TRACE][ARENA]") != NULL,
        "missing [TRACE][ARENA] handle_alloc line");
    ASSERT(strstr(buf, "handle_alloc(epoch=") != NULL,
        "missing handle_alloc tag");
    ASSERT(strstr(buf, "[INFO][ARENA]") != NULL,
        "missing [INFO][ARENA] gc line");
    ASSERT(strstr(buf, "gc_epoch_boundary(epoch=") != NULL,
        "missing gc_epoch_boundary tag");
    ASSERT(strstr(buf, "freed_handles=") != NULL,
        "missing freed_handles tag");
    ASSERT(strstr(buf, "remaining=") != NULL,
        "missing remaining tag");

    PASS();
cleanup:
    if (carena) wl_compound_arena_free(carena);
    (void)remove(tmp_path_);
    clear_env_();
}

static void
test_observability_gates(void)
{
    TEST("observability: WL_LOG unset -> ARENA silent");
    wl_arena_t *arena = NULL;

    clear_env_();
    make_tmpfile_("gate_off");
    setenv("WL_LOG_FILE", tmp_path_, 1);
    wl_log_init();

    arena = wl_arena_create(512);
    ASSERT(arena != NULL, "arena_create failed");
    (void)wl_arena_alloc(arena, 8);
    wl_arena_reset(arena);
    /* Free with zero outstanding bytes — no WARN to emit either. */
    wl_arena_free(arena);
    arena = NULL;

    wl_log_shutdown();

    char buf[512] = {0};
    size_t n = read_file_(tmp_path_, buf, sizeof(buf));
    ASSERT(n == 0, "ARENA emitted output with WL_LOG unset");

    PASS();
cleanup:
    if (arena) wl_arena_free(arena);
    (void)remove(tmp_path_);
    clear_env_();
}

int
main(void)
{
    printf("test_arena_observability (Issue #558)\n");
    printf("=====================================\n");

    test_arena_alloc_reset_logged();
    test_arena_free_warn_when_used();
    test_compound_arena_gc_logged();
    test_observability_gates();

    printf("\nResults: %d/%d passed, %d failed\n",
        tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
