/*
 * arena.h - wirelog Columnar Backend Arena Allocator
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * INTERNAL HEADER - not installed, not part of public API.
 *
 * ========================================================================
 * Overview
 * ========================================================================
 *
 * The columnar backend uses a per-epoch arena allocator for all temporary
 * relation storage during fixed-point iteration.  After each iteration
 * converges (no new deltas produced), the entire arena is freed at once.
 *
 * This avoids per-tuple malloc/free overhead and prevents heap fragmentation
 * across Polonius's 1,487 iterations.
 *
 * ========================================================================
 * Lifecycle
 * ========================================================================
 *
 *   // Evaluator loop (one arena per iteration)
 *   for (uint32_t iter = 0; iter < MAX_ITERATIONS; iter++) {
 *       wl_arena_t *arena = wl_arena_create(256 * 1024 * 1024); // 256MB
 *       if (!arena) { handle_oom(); break; }
 *
 *       bool has_delta = execute_strata(plan, arena);
 *
 *       wl_arena_free(arena);   // releases all iteration temporaries
 *
 *       if (!has_delta) break;  // convergence
 *   }
 *
 * ========================================================================
 * Allocation Semantics
 * ========================================================================
 *
 * - wl_arena_alloc() returns 8-byte aligned memory (suitable for int64_t).
 * - Allocations from a freed arena are undefined behaviour.
 * - wl_arena_reset() reuses the same backing buffer (no malloc/free).
 * - Thread safety: NOT thread-safe.  Each worker thread must own its arena.
 *
 * ========================================================================
 * Memory Profiling Targets (Phase 2A)
 * ========================================================================
 *
 *   Benchmark          | Peak Heap  | Target
 *   -------------------|------------|--------
 *   TC (4 nodes)       | <10MB      | Pass
 *   Reach (graph)      | <50MB      | Pass
 *   CC                 | <100MB     | Pass
 *   SSSP               | <200MB     | Pass
 *   Polonius (1487 it) | <20GB      | Pass
 */

#ifndef WL_ARENA_H
#define WL_ARENA_H

#include "columnar/memory_governor.h"

#include <stddef.h>

/* ======================================================================== */
/* Arena Allocator                                                          */
/* ======================================================================== */

/**
 * wl_arena_t:
 *
 * Bump-pointer arena allocator.  All allocations are served from a single
 * contiguous backing buffer.  Freeing individual allocations is not
 * supported; the entire arena is reset or destroyed atomically.
 *
 * @base:     Backing memory buffer (malloc-owned).
 * @capacity: Total size of the backing buffer in bytes.
 * @used:     Bytes allocated so far (always <= capacity).
 */
typedef struct wl_arena wl_arena_t;

struct wl_arena {
    void *base;
    size_t capacity;
    size_t used;
    /* Optional ownership hook. Keeping admission behind this callback means
     * legacy users that link arena.c alone do not acquire governor symbols. */
    void *admission_context;
    void (*admission_release)(void *context);
};

/**
 * wl_arena_create:
 * @capacity: Size of the backing buffer in bytes.  Must be > 0.
 *
 * Allocate a new arena with the given capacity.  The backing buffer is
 * allocated with malloc and aligned to at least 8 bytes.
 *
 * Returns:
 *   non-NULL: Pointer to the new arena (caller must free with wl_arena_free).
 *   NULL:     Memory allocation failure.
 */
wl_arena_t *
wl_arena_create(size_t capacity);

/**
 * wl_arena_create_managed:
 * @capacity: Size of the backing buffer in bytes.  Must be > 0.
 * @governor: Shared session governor that admits the complete backing slab.
 *
 * Create an arena whose fixed backing capacity is admitted before allocation
 * and released only when wl_arena_free() destroys the arena.  Reset preserves
 * the admission because it retains the backing slab.  On allocation failure
 * or denial, no bytes remain reserved in @governor.
 */
wl_arena_t *
wl_arena_create_managed(size_t capacity,
    wl_columnar_memory_governor_t *governor);

/* Internal constructor used by the managed-admission glue. */
wl_arena_t *
wl_arena_create_with_admission(size_t capacity, void *context,
    void (*admission_release)(void *context));

/**
 * wl_arena_alloc:
 * @arena: Arena to allocate from.  Must not be NULL.
 * @size:  Number of bytes to allocate.  Must be > 0.
 *
 * Bump-allocate @size bytes from the arena.  The returned pointer is
 * aligned to 8 bytes (suitable for int64_t row storage).
 *
 * Returns:
 *   non-NULL: Pointer to the allocated region.
 *   NULL:     Insufficient capacity (arena is exhausted).
 */
void *
wl_arena_alloc(wl_arena_t *arena, size_t size);

/**
 * wl_arena_reset:
 * @arena: Arena to reset.  Must not be NULL.
 *
 * Reset the arena's used counter to zero, making all previously allocated
 * memory available for reuse.  The backing buffer is NOT freed or
 * reallocated.  Existing pointers into the arena become invalid after reset.
 *
 * Use this to reuse an arena across iterations without malloc/free overhead.
 */
void
wl_arena_reset(wl_arena_t *arena);

/**
 * wl_arena_free:
 * @arena: (transfer full): Arena to free.  NULL-safe.
 *
 * Free the arena's backing buffer and the arena struct itself.
 * All pointers previously returned by wl_arena_alloc() become invalid.
 */
void
wl_arena_free(wl_arena_t *arena);

#endif /* WL_ARENA_H */
