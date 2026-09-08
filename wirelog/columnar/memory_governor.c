/*
 * columnar/memory_governor.c - managed session memory foundation
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 */

#include "columnar/memory_governor.h"

#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <limits.h>

#define WL_MEMORY_PATH_MAX 4096

static uint64_t
headroom_for(uint64_t budget)
{
    uint64_t headroom = budget / 20;
    if (headroom > WL_COLUMNAR_MEMORY_HEADROOM_CAP)
        headroom = WL_COLUMNAR_MEMORY_HEADROOM_CAP;
    if (headroom > budget / 2)
        headroom = budget / 2;
    return headroom;
}

static bool
finite_limit(uint64_t value)
{
    return value != 0 && value != UINT64_MAX;
}

static bool
finite_source_limit(wl_columnar_memory_source_t source, uint64_t value)
{
    if (!finite_limit(value))
        return false;
    return source != WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V1
           || value < (UINT64_C(1) << 62);
}

struct wl_columnar_memory_governor_ref {
    wl_columnar_memory_governor_t governor;
    wl_atomic_u64 references;
};

typedef enum {
    WL_MEMORY_PARSE_INVALID,
    WL_MEMORY_PARSE_VALID,
    WL_MEMORY_PARSE_OVERFLOW,
} wl_memory_parse_result_t;

static void
set_resolution(wl_columnar_memory_resolution_t *out, uint64_t budget,
    wl_columnar_memory_mode_t mode, wl_columnar_memory_source_t source)
{
    out->budget_bytes = budget;
    out->headroom_bytes = mode == WL_COLUMNAR_MEMORY_MODE_ENFORCING
        ? headroom_for(budget) : 0;
    out->usable_bytes = mode == WL_COLUMNAR_MEMORY_MODE_ENFORCING
        ? budget - out->headroom_bytes : UINT64_MAX;
    out->mode = mode;
    out->source = source;
    out->status = WL_COLUMNAR_MEMORY_OK;
}

const char *
wl_columnar_memory_source_name(wl_columnar_memory_source_t source)
{
    switch (source) {
    case WL_COLUMNAR_MEMORY_SOURCE_ENV: return "environment";
    case WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V2: return "cgroup-v2";
    case WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V1: return "cgroup-v1";
    case WL_COLUMNAR_MEMORY_SOURCE_RLIMIT_AS: return "rlimit-as";
    case WL_COLUMNAR_MEMORY_SOURCE_WINDOWS_JOB: return "windows-job";
    case WL_COLUMNAR_MEMORY_SOURCE_NONE: return "none";
    }
    return "unknown";
}

static wl_memory_parse_result_t
parse_decimal_bytes(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    const char *p;

    if (!text || !text[0] || !out)
        return WL_MEMORY_PARSE_INVALID;
    for (p = text; *p; p++) {
        uint64_t digit;
        if (*p < '0' || *p > '9')
            return WL_MEMORY_PARSE_INVALID;
        digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return WL_MEMORY_PARSE_OVERFLOW;
        value = value * 10 + digit;
    }
    *out = value;
    return WL_MEMORY_PARSE_VALID;
}

#ifndef _WIN32
static bool
read_limit_file(const char *path, bool cgroup_v1, uint64_t *out)
{
    FILE *file;
    char value[128];
    char *end;
    unsigned long long parsed;

    if (!path || !out)
        return false;
    file = fopen(path, "r");
    if (!file)
        return false;
    if (!fgets(value, sizeof(value), file)) {
        fclose(file);
        return false;
    }
    fclose(file);
    if (strncmp(value, "max", 3) == 0)
        return false;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || end == value)
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\n')
        end++;
    if (*end != '\0')
        return false;
    *out = (uint64_t)parsed;
    if (cgroup_v1 && *out >= (UINT64_C(1) << 62))
        return false;
    return finite_limit(*out);
}

static bool
read_cgroup_path(const char *prefix, char *out, size_t out_size)
{
    FILE *file;
    char line[WL_MEMORY_PATH_MAX];
    size_t prefix_len = strlen(prefix);

    file = fopen("/proc/self/cgroup", "r");
    if (!file)
        return false;
    while (fgets(line, sizeof(line), file)) {
        char *separator = strstr(line, prefix);
        if (!separator)
            continue;
        separator += prefix_len;
        if (*separator == '\0' || *separator == '\n')
            continue;
        separator[strcspn(separator, "\n")] = '\0';
        if (separator[0] != '/')
            continue;
        if (strlen(separator) >= out_size) {
            fclose(file);
            return false;
        }
        memcpy(out, separator, strlen(separator) + 1);
        fclose(file);
        return true;
    }
    fclose(file);
    return false;
}

static void
parent_cgroup_path(char *path)
{
    char *slash;
    size_t len;

    if (!path || strcmp(path, "/") == 0)
        return;
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
    slash = strrchr(path, '/');
    if (!slash || slash == path) {
        path[0] = '/';
        path[1] = '\0';
    } else
        *slash = '\0';
}

static bool
probe_cgroup_tree(const char *mount_root, const char *path,
    const char *max_name, const char *high_name, bool cgroup_v1,
    uint64_t *out)
{
    char current[WL_MEMORY_PATH_MAX];
    uint64_t best = UINT64_MAX;
    bool found = false;

    if (!mount_root || !path || !out || strlen(path) >= sizeof(current))
        return false;
    memcpy(current, path, strlen(path) + 1);
    for (;;) {
        char max_path[WL_MEMORY_PATH_MAX];
        char high_path[WL_MEMORY_PATH_MAX];
        uint64_t value;
        int written;

        written = snprintf(max_path, sizeof(max_path), "%s%s/%s",
                mount_root, current, max_name);
        if (written > 0 && (size_t)written < sizeof(max_path)
            && read_limit_file(max_path, cgroup_v1, &value)
            && value < best) {
            best = value;
            found = true;
        }
        if (high_name) {
            written = snprintf(high_path, sizeof(high_path), "%s%s/%s",
                    mount_root, current, high_name);
            if (written > 0 && (size_t)written < sizeof(high_path)
                && read_limit_file(high_path, cgroup_v1, &value)
                && value < best) {
                best = value;
                found = true;
            }
        }
        if (strcmp(current, "/") == 0)
            break;
        parent_cgroup_path(current);
    }
    if (found)
        *out = best;
    return found;
}
#endif

wl_columnar_memory_status_t
wl_columnar_memory_probe_sources(wl_columnar_memory_sources_t *sources)
{
    if (!sources)
        return WL_COLUMNAR_MEMORY_INVALID;
    memset(sources, 0, sizeof(*sources));
#ifndef _WIN32
    {
        char path[WL_MEMORY_PATH_MAX];
        uint64_t value;
        if (read_cgroup_path("0::", path, sizeof(path))
            && probe_cgroup_tree("/sys/fs/cgroup", path,
            "memory.max", "memory.high", false, &value)) {
            sources->cgroup_v2_available = true;
            sources->cgroup_v2_limit = value;
        }
        if (read_cgroup_path("memory:", path, sizeof(path))
            && probe_cgroup_tree("/sys/fs/cgroup/memory", path,
            "memory.limit_in_bytes", NULL, true, &value)) {
            sources->cgroup_v1_available = true;
            sources->cgroup_v1_limit = value;
        }
    }
    {
        struct rlimit limit;
        if (getrlimit(RLIMIT_AS, &limit) == 0
            && limit.rlim_cur != RLIM_INFINITY
            && limit.rlim_cur > 0
            && (uintmax_t)limit.rlim_cur <= UINT64_MAX) {
            sources->rlimit_as_available = true;
            sources->rlimit_as_limit = (uint64_t)limit.rlim_cur;
        }
    }
#else
    /*
     * A process can only query a Job Object through a job handle. The handle
     * is supplied by the host/job launcher; there is no portable way to
     * recover an unnamed current-job handle. The provider field remains
     * injectable for callers that own that handle and for deterministic tests.
     */
#endif
    return WL_COLUMNAR_MEMORY_OK;
}

wl_columnar_memory_status_t
wl_columnar_memory_probe_windows_job(void *job_handle,
    wl_columnar_memory_sources_t *sources)
{
    if (!sources)
        return WL_COLUMNAR_MEMORY_INVALID;
    sources->windows_job_available = false;
    sources->windows_job_limit = 0;
#ifdef _WIN32
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

        if (!job_handle)
            return WL_COLUMNAR_MEMORY_UNAVAILABLE;
        memset(&info, 0, sizeof(info));
        if (!QueryInformationJobObject((HANDLE)job_handle,
            JobObjectExtendedLimitInformation, &info, sizeof(info), NULL)
            || !(info.BasicLimitInformation.LimitFlags
            & JOB_OBJECT_LIMIT_PROCESS_MEMORY)
            || info.ProcessMemoryLimit == 0)
            return WL_COLUMNAR_MEMORY_UNAVAILABLE;
        sources->windows_job_available = true;
        sources->windows_job_limit = (uint64_t)info.ProcessMemoryLimit;
        return WL_COLUMNAR_MEMORY_OK;
    }
#else
    (void)job_handle;
    return WL_COLUMNAR_MEMORY_UNAVAILABLE;
#endif
}

void
wl_columnar_memory_reservation_init(
    wl_columnar_memory_reservation_t *reservation)
{
    if (!reservation)
        return;
    memset(reservation, 0, sizeof(*reservation));
    atomic_store_explicit(&reservation->state,
        WL_COLUMNAR_MEMORY_RESERVATION_EMPTY, memory_order_relaxed);
}

wl_columnar_memory_status_t
wl_columnar_memory_resolve(const char *env_value,
    const wl_columnar_memory_sources_t *sources,
    wl_columnar_memory_resolution_t *out)
{
    wl_columnar_memory_sources_t detected;
    uint64_t value;
    uint64_t best = UINT64_MAX;
    wl_columnar_memory_source_t source = WL_COLUMNAR_MEMORY_SOURCE_NONE;

    if (!out)
        return WL_COLUMNAR_MEMORY_INVALID;
    memset(out, 0, sizeof(*out));
    out->status = WL_COLUMNAR_MEMORY_INVALID;

    if (env_value) {
        wl_memory_parse_result_t parsed = parse_decimal_bytes(env_value,
                &value);
        if (parsed == WL_MEMORY_PARSE_OVERFLOW) {
            out->status = WL_COLUMNAR_MEMORY_OVERFLOW;
            return WL_COLUMNAR_MEMORY_OVERFLOW;
        }
        if (parsed != WL_MEMORY_PARSE_VALID)
            return WL_COLUMNAR_MEMORY_INVALID;
        if (value < WL_COLUMNAR_MEMORY_MIN_BUDGET)
            return WL_COLUMNAR_MEMORY_INVALID;
        set_resolution(out, value, WL_COLUMNAR_MEMORY_MODE_ENFORCING,
            WL_COLUMNAR_MEMORY_SOURCE_ENV);
        return WL_COLUMNAR_MEMORY_OK;
    }

    if (!sources) {
        if (wl_columnar_memory_probe_sources(&detected)
            != WL_COLUMNAR_MEMORY_OK)
            return WL_COLUMNAR_MEMORY_UNAVAILABLE;
        sources = &detected;
    }
    if (sources->cgroup_v2_available
        && finite_source_limit(WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V2,
        sources->cgroup_v2_limit)
        && sources->cgroup_v2_limit < best) {
        best = sources->cgroup_v2_limit;
        source = WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V2;
    }
    if (sources->cgroup_v1_available
        && finite_source_limit(WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V1,
        sources->cgroup_v1_limit)
        && sources->cgroup_v1_limit < best) {
        best = sources->cgroup_v1_limit;
        source = WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V1;
    }
    if (sources->rlimit_as_available
        && finite_source_limit(WL_COLUMNAR_MEMORY_SOURCE_RLIMIT_AS,
        sources->rlimit_as_limit)
        && sources->rlimit_as_limit < best) {
        best = sources->rlimit_as_limit;
        source = WL_COLUMNAR_MEMORY_SOURCE_RLIMIT_AS;
    }
    if (sources->windows_job_available
        && finite_source_limit(WL_COLUMNAR_MEMORY_SOURCE_WINDOWS_JOB,
        sources->windows_job_limit)
        && sources->windows_job_limit < best) {
        best = sources->windows_job_limit;
        source = WL_COLUMNAR_MEMORY_SOURCE_WINDOWS_JOB;
    }
    if (source == WL_COLUMNAR_MEMORY_SOURCE_NONE) {
        set_resolution(out, 0, WL_COLUMNAR_MEMORY_MODE_ADVISORY,
            WL_COLUMNAR_MEMORY_SOURCE_NONE);
        return WL_COLUMNAR_MEMORY_OK;
    }
    if (best < WL_COLUMNAR_MEMORY_MIN_BUDGET) {
        out->status = WL_COLUMNAR_MEMORY_INVALID;
        return WL_COLUMNAR_MEMORY_INVALID;
    }
    set_resolution(out, best, WL_COLUMNAR_MEMORY_MODE_ENFORCING, source);
    return WL_COLUMNAR_MEMORY_OK;
}

wl_columnar_memory_status_t
wl_columnar_memory_governor_init(wl_columnar_memory_governor_t *governor,
    const wl_columnar_memory_resolution_t *resolution)
{
    if (!governor || !resolution
        || resolution->status != WL_COLUMNAR_MEMORY_OK)
        return WL_COLUMNAR_MEMORY_INVALID;
    memset(governor, 0, sizeof(*governor));
    governor->budget_bytes = resolution->budget_bytes;
    governor->headroom_bytes = resolution->headroom_bytes;
    governor->mode = resolution->mode;
    governor->source = resolution->source;
    atomic_store_explicit(&governor->usable_bytes, resolution->usable_bytes,
        memory_order_relaxed);
    atomic_store_explicit(&governor->reserved_bytes, 0, memory_order_relaxed);
    return WL_COLUMNAR_MEMORY_OK;
}

wl_columnar_memory_governor_ref_t *
wl_columnar_memory_governor_ref_create(
    const wl_columnar_memory_resolution_t *resolution)
{
    wl_columnar_memory_governor_ref_t *ref;

    if (!resolution || resolution->status != WL_COLUMNAR_MEMORY_OK)
        return NULL;
    ref = (wl_columnar_memory_governor_ref_t *)calloc(1, sizeof(*ref));
    if (!ref)
        return NULL;
    if (wl_columnar_memory_governor_init(&ref->governor, resolution)
        != WL_COLUMNAR_MEMORY_OK) {
        free(ref);
        return NULL;
    }
    atomic_store_explicit(&ref->references, 1, memory_order_relaxed);
    return ref;
}

void
wl_columnar_memory_governor_ref_retain(
    wl_columnar_memory_governor_ref_t *ref)
{
    if (ref)
        atomic_fetch_add_explicit(&ref->references, 1, memory_order_relaxed);
}

void
wl_columnar_memory_governor_ref_release(
    wl_columnar_memory_governor_ref_t *ref)
{
    if (ref
        && atomic_fetch_sub_explicit(&ref->references, 1,
        memory_order_release) == 1) {
        (void)atomic_load_explicit(&ref->references, memory_order_acquire);
        free(ref);
    }
}

wl_columnar_memory_governor_t *
wl_columnar_memory_governor_ref_get(
    wl_columnar_memory_governor_ref_t *ref)
{
    return ref ? &ref->governor : NULL;
}

static bool
claim_reservation_state(wl_columnar_memory_reservation_t *reservation,
    uint64_t from, uint64_t to);

bool
wl_columnar_memory_reserve(wl_columnar_memory_governor_t *governor,
    uint64_t bytes, wl_columnar_memory_reservation_t *reservation)
{
    uint64_t old;
    uint64_t expected;

    if (!governor || !reservation || bytes == 0)
        return false;
    expected = WL_COLUMNAR_MEMORY_RESERVATION_EMPTY;
    if (!claim_reservation_state(reservation, expected,
        WL_COLUMNAR_MEMORY_RESERVATION_CLAIMED)) {
        expected = WL_COLUMNAR_MEMORY_RESERVATION_RELEASED;
        if (!claim_reservation_state(reservation, expected,
            WL_COLUMNAR_MEMORY_RESERVATION_CLAIMED))
            return false;
    }
    reservation->governor = governor;
    reservation->bytes = bytes;
    atomic_store_explicit(&reservation->owner_bits, 0, memory_order_relaxed);
    old = atomic_load_explicit(&governor->reserved_bytes, memory_order_relaxed);
    for (;;) {
        uint64_t limit = atomic_load_explicit(&governor->usable_bytes,
                memory_order_relaxed);
        uint64_t next;
        if (bytes > UINT64_MAX - old) {
            atomic_store_explicit(&reservation->state,
                WL_COLUMNAR_MEMORY_RESERVATION_RELEASED, memory_order_release);
            return false;
        }
        next = old + bytes;
        if (next > limit) {
            atomic_store_explicit(&reservation->state,
                WL_COLUMNAR_MEMORY_RESERVATION_RELEASED, memory_order_release);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &governor->reserved_bytes, &old, next,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
    atomic_store_explicit(&reservation->state,
        WL_COLUMNAR_MEMORY_RESERVATION_RESERVED, memory_order_release);
    return true;
}

static bool
transition_reservation(wl_columnar_memory_reservation_t *reservation,
    uint64_t from, uint64_t to)
{
    uint64_t observed = from;

    if (!reservation)
        return false;
    for (;;) {
        if (atomic_compare_exchange_weak_explicit(&reservation->state,
            &observed, to, memory_order_release, memory_order_acquire))
            return true;
        if (observed != from)
            return false;
        observed = from;
    }
}

static bool
claim_reservation_state(wl_columnar_memory_reservation_t *reservation,
    uint64_t from, uint64_t to)
{
    uint64_t observed = from;

    if (!reservation)
        return false;
    for (;;) {
        if (atomic_compare_exchange_weak_explicit(&reservation->state,
            &observed, to, memory_order_release, memory_order_acquire))
            return true;
        if (observed != from)
            return false;
        observed = from;
    }
}

bool
wl_columnar_memory_commit(wl_columnar_memory_reservation_t *reservation,
    const void *owner)
{
    if (!reservation
        || !transition_reservation(reservation,
        WL_COLUMNAR_MEMORY_RESERVATION_RESERVED,
        WL_COLUMNAR_MEMORY_RESERVATION_COMMITTING))
        return false;
    atomic_store_explicit(&reservation->owner_bits,
        (uint64_t)(uintptr_t)owner, memory_order_release);
    atomic_store_explicit(&reservation->state,
        WL_COLUMNAR_MEMORY_RESERVATION_COMMITTED, memory_order_release);
    return true;
}

bool
wl_columnar_memory_transfer(wl_columnar_memory_reservation_t *reservation,
    const void *owner)
{
    uint64_t state;
    if (!reservation)
        return false;
    state = atomic_load_explicit(&reservation->state, memory_order_acquire);
    if (state != WL_COLUMNAR_MEMORY_RESERVATION_RESERVED
        && state != WL_COLUMNAR_MEMORY_RESERVATION_COMMITTED)
        return false;
    if (!claim_reservation_state(reservation, state,
        WL_COLUMNAR_MEMORY_RESERVATION_TRANSFERRING))
        return false;
    atomic_store_explicit(&reservation->owner_bits,
        (uint64_t)(uintptr_t)owner, memory_order_release);
    atomic_store_explicit(&reservation->state, state, memory_order_release);
    return true;
}

static bool
finish_reservation(wl_columnar_memory_reservation_t *reservation,
    bool rollback)
{
    uint64_t expected;
    wl_columnar_memory_governor_t *governor;
    uint64_t bytes;
    uint64_t old;

    if (!reservation)
        return false;
    governor = reservation->governor;
    bytes = reservation->bytes;
    if (!governor || bytes == 0)
        return false;
    if (rollback) {
        if (!transition_reservation(reservation,
            WL_COLUMNAR_MEMORY_RESERVATION_RESERVED,
            WL_COLUMNAR_MEMORY_RESERVATION_RELEASED))
            return false;
    } else {
        expected = atomic_load_explicit(&reservation->state,
                memory_order_acquire);
        if (expected != WL_COLUMNAR_MEMORY_RESERVATION_RESERVED
            && expected != WL_COLUMNAR_MEMORY_RESERVATION_COMMITTED)
            return false;
        if (!transition_reservation(reservation, expected,
            WL_COLUMNAR_MEMORY_RESERVATION_RELEASED))
            return false;
    }
    old = atomic_load_explicit(&governor->reserved_bytes, memory_order_relaxed);
    for (;;) {
        uint64_t next;
        if (bytes > old)
            return false;
        next = old - bytes;
        if (atomic_compare_exchange_weak_explicit(
                &governor->reserved_bytes, &old, next,
                memory_order_release, memory_order_relaxed))
            break;
    }
    return true;
}

bool
wl_columnar_memory_rollback(
    wl_columnar_memory_reservation_t *reservation)
{
    return finish_reservation(reservation, true);
}

bool
wl_columnar_memory_release(wl_columnar_memory_reservation_t *reservation)
{
    if (!reservation)
        return false;
    if (finish_reservation(reservation, false))
        return true;
    return finish_reservation(reservation, true);
}

uint64_t
wl_columnar_memory_reserved(const wl_columnar_memory_governor_t *governor)
{
    if (!governor)
        return 0;
    return atomic_load_explicit(&governor->reserved_bytes,
               memory_order_acquire);
}
