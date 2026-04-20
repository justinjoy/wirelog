/*
 * bench_argv.h - Minimal portable argv parser for bench/ + datagen.
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Scope: a strict subset of POSIX getopt_long that covers exactly what
 * bench_flowlog and datagen use today. Intentionally unlike glibc getopt:
 *
 *   Supported:
 *     -x              bare short flag
 *     -x arg          short with required argument (space-separated)
 *     -xarg           short with glued argument
 *     --name          bare long flag
 *     --name arg      long with required argument (space-separated)
 *     --              end-of-options terminator
 *
 *   NOT supported (by design - no caller needs them; adding them is a
 *   measurement-design decision that belongs elsewhere):
 *     --name=value    explicit equals form
 *     -abc            bundled short flags
 *     optional_argument / OPTIONAL_ARG
 *     optreset, POSIXLY_CORRECT, W;, getopt_long_only
 *     argv permutation (stops at first non-option)
 *
 * All state is caller-owned via bench_argv_state_t - no hidden globals,
 * no thread-safety gymnastics. Return sentinel matches POSIX getopt:
 *   - matched short char or long_opts[i].val on match
 *   - '?' on unknown option or missing required argument (diagnostic to stderr)
 *   - -1 at end of options (including after --)
 *
 * Callers replace getopt_long's `optarg` global with `state.optarg`.
 */

#ifndef BENCH_ARGV_H
#define BENCH_ARGV_H

#include <stdio.h>
#include <string.h>

/* --- libc portability shim used by bench binaries --- */
/* MSVC C runtime does not expose strtok_r; the single-threaded bench
 * binaries tolerate strtok's static state, so the shim is a straight
 * macro substitution. Kept here so bench_flowlog.c and datagen.c are
 * free of any #ifdef _MSC_VER of their own. */
#if defined(_MSC_VER) && !defined(__clang__)
#  ifndef strtok_r
#    define strtok_r(str, delim, saveptr) strtok((str), (delim))
#  endif
#endif

/* Keep the POSIX spelling so long_opts tables read identically to the
 * pre-port code. Guarded so callers that still include <getopt.h> for
 * other reasons do not hit a redefinition. */
#ifndef no_argument
#  define no_argument       0
#endif
#ifndef required_argument
#  define required_argument 1
#endif

typedef struct {
    const char *name;
    int         has_arg;  /* no_argument (0) or required_argument (1) */
    int         val;      /* code returned when this option matches */
} bench_argv_long_t;

typedef struct {
    int   optind;   /* next argv index to process; init to 1 */
    char *optarg;   /* argument of the most recent match, or NULL */
} bench_argv_state_t;

#define BENCH_ARGV_INIT { 1, NULL }

/* Single step of argv parsing. Returns matched option code, '?' on error,
 * or -1 at end. Diagnostics are written to stderr prefixed with argv[0]. */
static inline int
bench_argv_next(int argc, char *const argv[],
                const char *short_opts,
                const bench_argv_long_t *long_opts,
                bench_argv_state_t *st)
{
    st->optarg = NULL;
    if (st->optind >= argc)
        return -1;
    const char *a = argv[st->optind];
    if (a == NULL || a[0] != '-' || a[1] == '\0')
        return -1; /* non-option argv: stop (no permutation) */
    if (a[1] == '-' && a[2] == '\0') {
        st->optind += 1; /* consume "--" */
        return -1;
    }

    if (a[1] == '-') {
        /* long option: "--name" */
        const char *name = a + 2;
        st->optind += 1;
        if (long_opts) {
            for (int i = 0; long_opts[i].name != NULL; ++i) {
                if (strcmp(long_opts[i].name, name) != 0)
                    continue;
                if (long_opts[i].has_arg == 1) {
                    if (st->optind >= argc) {
                        fprintf(stderr,
                            "%s: option '--%s' requires an argument\n",
                            argv[0], name);
                        return '?';
                    }
                    st->optarg = argv[st->optind];
                    st->optind += 1;
                }
                return long_opts[i].val;
            }
        }
        fprintf(stderr, "%s: unrecognized option '--%s'\n", argv[0], name);
        return '?';
    }

    /* short option: -x or -xarg or -x arg */
    int c = (unsigned char)a[1];
    st->optind += 1;
    const char *hit = (short_opts != NULL) ? strchr(short_opts, c) : NULL;
    if (hit == NULL) {
        fprintf(stderr, "%s: unrecognized option '-%c'\n",
            argv[0], (char)c);
        return '?';
    }
    if (hit[1] == ':') {
        if (a[2] != '\0') {
            st->optarg = (char *)(a + 2); /* glued: -xarg */
        } else {
            if (st->optind >= argc) {
                fprintf(stderr,
                    "%s: option '-%c' requires an argument\n",
                    argv[0], (char)c);
                return '?';
            }
            st->optarg = argv[st->optind];
            st->optind += 1;
        }
    } else if (a[2] != '\0') {
        /* Unsupported bundled flags: '-abc'. Fail loudly so we never
         * silently mis-parse an option string the caller expected. */
        fprintf(stderr,
            "%s: bundled short options not supported ('-%c%s')\n",
            argv[0], (char)c, a + 2);
        return '?';
    }
    return c;
}

#endif /* BENCH_ARGV_H */
