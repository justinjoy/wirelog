/*
 * intern.h - Symbol Intern Table
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * INTERNAL HEADER - not installed, not part of public API.
 * Bidirectional mapping between strings and integer IDs for symbol
 * interning.  Strings are interned at parse/load time; integer IDs
 * are used throughout the DD execution pipeline.
 */

#ifndef WIRELOG_INTERN_H
#define WIRELOG_INTERN_H

#include <stdint.h>

typedef struct wl_intern wl_intern_t;

/**
 * wl_intern_create:
 *
 * Create a new empty intern table.
 *
 * Returns: new intern table, or NULL on allocation failure.
 */
wl_intern_t *
wl_intern_create(void);

/**
 * wl_intern_free:
 * @intern: (transfer full): Intern table to free (NULL-safe).
 *
 * Free an intern table and all interned strings.
 */
void
wl_intern_free(wl_intern_t *intern);

/**
 * wl_intern_put:
 * @intern: Intern table.
 * @str:    String to intern (must not be NULL).
 *
 * Intern a string.  If the string was previously interned, returns
 * the existing ID.  Otherwise assigns a new sequential ID.
 *
 * Returns: non-negative ID on success, -1 if @str is NULL.
 */
int64_t
wl_intern_put(wl_intern_t *intern, const char *str);

/**
 * wl_intern_get:
 * @intern: Intern table.
 * @str:    String to look up (must not be NULL).
 *
 * Look up a string without inserting it.
 *
 * Returns: non-negative ID if found, -1 if not found or @str is NULL.
 */
int64_t
wl_intern_get(const wl_intern_t *intern, const char *str);

/**
 * wl_intern_reverse:
 * @intern: Intern table.
 * @id:     ID to look up.
 *
 * Reverse-map an ID back to the original string.
 *
 * Returns: interned string (owned by table, do not free), or NULL
 *          if @id is out of range.
 */
const char *
wl_intern_reverse(const wl_intern_t *intern, int64_t id);

/**
 * wl_intern_count:
 * @intern: Intern table.
 *
 * Returns: number of interned strings.
 */
uint32_t
wl_intern_count(const wl_intern_t *intern);

#endif /* WIRELOG_INTERN_H */
