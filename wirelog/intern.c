/*
 * intern.c - Symbol Intern Table
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Open-addressing hash table (FNV-1a + linear probing) for string
 * interning.  Maps strings to sequential integer IDs and supports
 * reverse lookup by ID.
 */

#include "intern.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* Constants                                                                */
/* ======================================================================== */

#define INTERN_INITIAL_CAP 64
#define INTERN_LOAD_FACTOR_NUM 3 /* 75% load factor = 3/4 */
#define INTERN_LOAD_FACTOR_DEN 4

/* Sentinel: empty hash table slot */
#define SLOT_EMPTY UINT32_MAX

/* ======================================================================== */
/* Hash Table Entry                                                         */
/* ======================================================================== */

typedef struct {
    uint32_t string_id; /* index into strings array, or SLOT_EMPTY */
} wl_intern_slot_t;

/* ======================================================================== */
/* Intern Table Structure                                                   */
/* ======================================================================== */

struct wl_intern {
    /* Reverse array: id -> string (owned copies) */
    char **strings;
    uint32_t count;
    uint32_t string_capacity;

    /* Hash table: string -> id (open addressing) */
    wl_intern_slot_t *slots;
    uint32_t slot_capacity;
};

/* ======================================================================== */
/* FNV-1a Hash                                                              */
/* ======================================================================== */

static uint32_t
fnv1a(const char *str)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

/* ======================================================================== */
/* Internal Helpers                                                         */
/* ======================================================================== */

static int
intern_resize(wl_intern_t *intern)
{
    if (intern->slot_capacity > UINT32_MAX / 2U)
        return -1;
    uint32_t new_cap = intern->slot_capacity * 2U;
    wl_intern_slot_t *new_slots
        = (wl_intern_slot_t *)malloc((size_t)new_cap
            * sizeof(wl_intern_slot_t));
    if (!new_slots)
        return -1;

    memset(new_slots, 0xff, (size_t)new_cap * sizeof(wl_intern_slot_t));

    /* Re-insert all existing entries */
    for (uint32_t i = 0; i < intern->count; i++) {
        uint32_t h = fnv1a(intern->strings[i]) & (new_cap - 1);
        while (new_slots[h].string_id != SLOT_EMPTY)
            h = (h + 1) & (new_cap - 1);
        new_slots[h].string_id = i;
    }

    free(intern->slots);
    intern->slots = new_slots;
    intern->slot_capacity = new_cap;
    return 0;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

wl_intern_t *
wl_intern_create(void)
{
    wl_intern_t *intern = (wl_intern_t *)calloc(1, sizeof(wl_intern_t));
    if (!intern)
        return NULL;

    intern->string_capacity = INTERN_INITIAL_CAP;
    intern->strings = (char **)malloc(intern->string_capacity * sizeof(char *));
    if (!intern->strings) {
        free(intern);
        return NULL;
    }

    intern->slot_capacity = INTERN_INITIAL_CAP;
    intern->slots = (wl_intern_slot_t *)malloc(intern->slot_capacity
            * sizeof(wl_intern_slot_t));
    if (!intern->slots) {
        free((void *)intern->strings);
        free(intern);
        return NULL;
    }

    for (uint32_t i = 0; i < intern->slot_capacity; i++)
        intern->slots[i].string_id = SLOT_EMPTY;

    return intern;
}

void
wl_intern_free(wl_intern_t *intern)
{
    if (!intern)
        return;

    for (uint32_t i = 0; i < intern->count; i++)
        free(intern->strings[i]);

    free((void *)intern->strings);
    free(intern->slots);
    free(intern);
}

int64_t
wl_intern_put(wl_intern_t *intern, const char *str)
{
    if (!intern || !str)
        return -1;

    /* Check if already interned */
    uint32_t mask = intern->slot_capacity - 1;
    uint32_t h = fnv1a(str) & mask;

    while (intern->slots[h].string_id != SLOT_EMPTY) {
        uint32_t sid = intern->slots[h].string_id;
        if (strcmp(intern->strings[sid], str) == 0)
            return (int64_t)sid;
        h = (h + 1) & mask;
    }

    /* New string: grow string array if needed */
    if (intern->count >= intern->string_capacity) {
        if (intern->string_capacity > UINT32_MAX / 2U)
            return -1;
        uint32_t new_cap = intern->string_capacity * 2U;
        char **new_strs = (char **)realloc((void *)intern->strings,
                (size_t)new_cap * sizeof(char *));
        if (!new_strs)
            return -1;
        intern->strings = new_strs;
        intern->string_capacity = new_cap;
    }

    size_t slen = strlen(str);
    char *copy = (char *)malloc(slen + 1);
    if (!copy)
        return -1;
    memcpy(copy, str, slen + 1);

    /* Resize hash table before insertion if the new entry would exceed the load factor. */
    if ((uint64_t)(intern->count + 1U) * INTERN_LOAD_FACTOR_DEN
        > (uint64_t)intern->slot_capacity * INTERN_LOAD_FACTOR_NUM) {
        if (intern_resize(intern) != 0) {
            free(copy);
            return -1;
        }
        mask = intern->slot_capacity - 1U;
        h = fnv1a(str) & mask;
        while (intern->slots[h].string_id != SLOT_EMPTY)
            h = (h + 1U) & mask;
    }

    uint32_t new_id = intern->count;
    intern->strings[new_id] = copy;
    intern->count++;
    intern->slots[h].string_id = new_id;

    return (int64_t)new_id;
}

int64_t
wl_intern_get(const wl_intern_t *intern, const char *str)
{
    if (!intern || !str)
        return -1;

    uint32_t mask = intern->slot_capacity - 1;
    uint32_t h = fnv1a(str) & mask;

    while (intern->slots[h].string_id != SLOT_EMPTY) {
        uint32_t sid = intern->slots[h].string_id;
        if (strcmp(intern->strings[sid], str) == 0)
            return (int64_t)sid;
        h = (h + 1) & mask;
    }

    return -1;
}

const char *
wl_intern_reverse(const wl_intern_t *intern, int64_t id)
{
    if (!intern || id < 0 || (uint32_t)id >= intern->count)
        return NULL;

    return intern->strings[(uint32_t)id];
}

uint32_t
wl_intern_count(const wl_intern_t *intern)
{
    if (!intern)
        return 0;

    return intern->count;
}
