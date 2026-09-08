/*
 * columnar/memory_governor.h - managed session memory foundation
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 *
 * INTERNAL HEADER - not installed, not part of the public API.
 */

#ifndef WL_COLUMNAR_MEMORY_GOVERNOR_H
#define WL_COLUMNAR_MEMORY_GOVERNOR_H

#include "columnar/mem_ledger.h"

#include <stdbool.h>
#include <stdint.h>

#define WL_COLUMNAR_MEMORY_MIN_BUDGET (UINT64_C(256) * 1024 * 1024)
#define WL_COLUMNAR_MEMORY_HEADROOM_CAP (UINT64_C(256) * 1024 * 1024)

typedef enum {
    WL_COLUMNAR_MEMORY_MODE_ADVISORY = 0,
    WL_COLUMNAR_MEMORY_MODE_ENFORCING = 1,
} wl_columnar_memory_mode_t;

typedef enum {
    WL_COLUMNAR_MEMORY_SOURCE_NONE = 0,
    WL_COLUMNAR_MEMORY_SOURCE_ENV,
    WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V2,
    WL_COLUMNAR_MEMORY_SOURCE_CGROUP_V1,
    WL_COLUMNAR_MEMORY_SOURCE_RLIMIT_AS,
    WL_COLUMNAR_MEMORY_SOURCE_WINDOWS_JOB,
} wl_columnar_memory_source_t;

typedef enum {
    WL_COLUMNAR_MEMORY_OK = 0,
    WL_COLUMNAR_MEMORY_INVALID = 1,
    WL_COLUMNAR_MEMORY_UNAVAILABLE = 2,
    WL_COLUMNAR_MEMORY_OVERFLOW = 3,
} wl_columnar_memory_status_t;

/*
 * Testable automatic-limit inputs. An unavailable field means that source did
 * not yield a finite limit. Cgroup values are already the minimum finite
 * values found across that hierarchy; the runtime probe performs that
 * reduction before returning.
 */
typedef struct {
    bool cgroup_v2_available;
    uint64_t cgroup_v2_limit;
    bool cgroup_v1_available;
    uint64_t cgroup_v1_limit;
    bool rlimit_as_available;
    uint64_t rlimit_as_limit;
    bool windows_job_available;
    uint64_t windows_job_limit;
} wl_columnar_memory_sources_t;

typedef struct {
    uint64_t budget_bytes;
    uint64_t headroom_bytes;
    uint64_t usable_bytes;
    wl_columnar_memory_mode_t mode;
    wl_columnar_memory_source_t source;
    wl_columnar_memory_status_t status;
} wl_columnar_memory_resolution_t;

typedef struct {
    wl_atomic_u64 usable_bytes;
    wl_atomic_u64 reserved_bytes;
    uint64_t budget_bytes;
    uint64_t headroom_bytes;
    wl_columnar_memory_mode_t mode;
    wl_columnar_memory_source_t source;
} wl_columnar_memory_governor_t;

typedef enum {
    WL_COLUMNAR_MEMORY_RESERVATION_EMPTY = 0,
    WL_COLUMNAR_MEMORY_RESERVATION_RESERVED = 1,
    WL_COLUMNAR_MEMORY_RESERVATION_COMMITTED = 2,
    WL_COLUMNAR_MEMORY_RESERVATION_RELEASED = 3,
    WL_COLUMNAR_MEMORY_RESERVATION_CLAIMED = 4,
    WL_COLUMNAR_MEMORY_RESERVATION_COMMITTING = 5,
    WL_COLUMNAR_MEMORY_RESERVATION_TRANSFERRING = 6,
} wl_columnar_memory_reservation_state_t;

typedef struct {
    wl_columnar_memory_governor_t *governor;
    uint64_t bytes;
    wl_atomic_u64 owner_bits;
    wl_atomic_u64 state;
} wl_columnar_memory_reservation_t;

/* Initialize a caller-owned token before its first reserve or reuse. */
void
wl_columnar_memory_reservation_init(
    wl_columnar_memory_reservation_t *reservation);

const char *
wl_columnar_memory_source_name(wl_columnar_memory_source_t source);

/*
 * Fill sources from the current process limits. This function is best effort:
 * no finite supported source is a valid result and is represented by all
 * availability flags being false. It never treats physical RAM alone as an
 * enforcing limit.
 */
wl_columnar_memory_status_t
wl_columnar_memory_probe_sources(wl_columnar_memory_sources_t *sources);

/*
 * Query a host-owned Windows Job Object when session integration has a job
 * handle. The opaque handle keeps this internal header portable; NULL or a
 * platform without Job Objects returns unavailable.
 */
wl_columnar_memory_status_t
wl_columnar_memory_probe_windows_job(void *job_handle,
    wl_columnar_memory_sources_t *sources);

/*
 * Resolve a decimal WIRELOG_MEMORY_BUDGET value. A non-NULL env_value is an
 * explicit configuration, including an empty string, and is validated strictly.
 * A NULL value selects the effective minimum from sources and falls back to
 * advisory/unbounded mode when no source is finite.
 */
wl_columnar_memory_status_t
wl_columnar_memory_resolve(const char *env_value,
    const wl_columnar_memory_sources_t *sources,
    wl_columnar_memory_resolution_t *out);

wl_columnar_memory_status_t
wl_columnar_memory_governor_init(wl_columnar_memory_governor_t *governor,
    const wl_columnar_memory_resolution_t *resolution);

/*
 * Reservations are tokenized so release/rollback are idempotent and cannot
 * release another reservation's bytes. A token is caller-owned and must remain
 * alive until its terminal operation returns.
 */
bool
wl_columnar_memory_reserve(wl_columnar_memory_governor_t *governor,
    uint64_t bytes, wl_columnar_memory_reservation_t *reservation);

bool
wl_columnar_memory_commit(wl_columnar_memory_reservation_t *reservation,
    const void *owner);

bool
wl_columnar_memory_transfer(wl_columnar_memory_reservation_t *reservation,
    const void *owner);

bool
wl_columnar_memory_rollback(
    wl_columnar_memory_reservation_t *reservation);

bool
wl_columnar_memory_release(wl_columnar_memory_reservation_t *reservation);

uint64_t
wl_columnar_memory_reserved(const wl_columnar_memory_governor_t *governor);

#endif /* WL_COLUMNAR_MEMORY_GOVERNOR_H */
