/*
 * arena/arena.c - wirelog Columnar Backend Arena Allocator
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 */

#include "arena.h"

#include "wirelog/util/log.h"

#include <stdlib.h>
#include <string.h>

/* Alignment for all arena allocations (8 bytes = sizeof(int64_t)). */
#define WL_ARENA_ALIGN 8

/* Round @n up to the next multiple of WL_ARENA_ALIGN. */
#define WL_ARENA_ALIGN_UP(n) \
        (((n) + (WL_ARENA_ALIGN - 1)) & ~(size_t)(WL_ARENA_ALIGN - 1))

static wl_arena_t *
wl_arena_create_impl(size_t capacity, void *admission_context,
    void (*admission_release)(void *context))
{
    if (capacity == 0)
        return NULL;

    wl_arena_t *arena = (wl_arena_t *)malloc(sizeof(wl_arena_t));
    if (!arena)
        return NULL;

    memset(arena, 0, sizeof(*arena));
    arena->base = malloc(capacity);
    if (!arena->base) {
        free(arena);
        return NULL;
    }

    arena->capacity = capacity;
    arena->used = 0;
    arena->admission_context = admission_context;
    arena->admission_release = admission_release;
    return arena;
}

wl_arena_t *
wl_arena_create(size_t capacity)
{
    return wl_arena_create_impl(capacity, NULL, NULL);
}

wl_arena_t *
wl_arena_create_with_admission(size_t capacity, void *context,
    void (*admission_release)(void *context))
{
    return wl_arena_create_impl(capacity, context, admission_release);
}

void *
wl_arena_alloc(wl_arena_t *arena, size_t size)
{
    if (!arena || size == 0)
        return NULL;

    size_t aligned = WL_ARENA_ALIGN_UP(size);
    if (arena->used + aligned > arena->capacity)
        return NULL;

    void *ptr = (char *)arena->base + arena->used;
    arena->used += aligned;
    WL_LOG(WL_LOG_SEC_ARENA, WL_LOG_DEBUG,
        "alloc(size=%zu, new_offset=%zu, capacity=%zu)",
        size, arena->used, arena->capacity);
    return ptr;
}

void
wl_arena_reset(wl_arena_t *arena)
{
    if (!arena)
        return;
    WL_LOG(WL_LOG_SEC_ARENA, WL_LOG_INFO,
        "reset(used=%zu -> 0, capacity=%zu)",
        arena->used, arena->capacity);
    arena->used = 0;
}

void
wl_arena_free(wl_arena_t *arena)
{
    if (!arena)
        return;
    if (arena->used > 0) {
        WL_LOG(WL_LOG_SEC_ARENA, WL_LOG_WARN,
            "free() with %zu bytes still allocated",
            arena->used);
    }
    if (arena->admission_release)
        arena->admission_release(arena->admission_context);
    free(arena->base);
    free(arena);
}
