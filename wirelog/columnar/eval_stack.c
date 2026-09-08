/*
 * columnar/eval_stack.c - Columnar operator evaluation stack
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 */

#include "columnar/internal.h"

void
eval_stack_init(eval_stack_t *s)
{
    memset(s, 0, sizeof(*s));
}

int
eval_stack_push(eval_stack_t *s, col_rel_t *r, bool owned)
{
    if (s->top >= COL_STACK_MAX)
        return ENOBUFS;
    s->items[s->top].rel = r;
    s->items[s->top].owned = owned;
    s->items[s->top].is_delta = false;
    s->items[s->top].seg_boundaries = NULL;
    s->items[s->top].seg_count = 0;
    s->items[s->top].kind = WL_COLUMNAR_EVAL_ENTRY_RELATION;
    s->items[s->top].continuation = NULL;
    s->top++;
    return 0;
}

int
eval_stack_push_continuation(eval_stack_t *s,
    wl_columnar_continuation_t *continuation)
{
    if (!s || !continuation)
        return EINVAL;
    if (s->top >= COL_STACK_MAX)
        return ENOBUFS;
    s->items[s->top].rel = NULL;
    s->items[s->top].owned = true;
    s->items[s->top].is_delta = false;
    s->items[s->top].seg_boundaries = NULL;
    s->items[s->top].seg_count = 0;
    s->items[s->top].kind = WL_COLUMNAR_EVAL_ENTRY_CONTINUATION;
    s->items[s->top].continuation = continuation;
    s->top++;
    return 0;
}

/* Push with explicit delta flag (used by VARIABLE and JOIN to tag delta results). */
int
eval_stack_push_delta(eval_stack_t *s, col_rel_t *r, bool owned, bool is_delta)
{
    int rc = eval_stack_push(s, r, owned);
    if (rc == 0)
        s->items[s->top - 1].is_delta = is_delta;
    return rc;
}

eval_entry_t
eval_stack_pop(eval_stack_t *s)
{
    eval_entry_t e = { 0 };
    if (s && s->top > 0)
        e = s->items[--s->top];
    return e;
}

void
eval_entry_dispose(eval_entry_t *entry)
{
    if (!entry)
        return;

    free(entry->seg_boundaries);
    entry->seg_boundaries = NULL;
    entry->seg_count = 0;

    if (entry->kind == WL_COLUMNAR_EVAL_ENTRY_CONTINUATION) {
        wl_columnar_continuation_destroy(entry->continuation);
        entry->continuation = NULL;
    } else if (entry->owned) {
        col_rel_destroy(entry->rel);
        entry->rel = NULL;
    }
    entry->owned = false;
}

int
eval_stack_pop_relation(eval_stack_t *s, eval_entry_t *out)
{
    eval_entry_t e;

    if (!out)
        return EINVAL;
    memset(out, 0, sizeof(*out));
    if (!s || s->top == 0)
        return EINVAL;

    e = eval_stack_pop(s);
    if (e.kind == WL_COLUMNAR_EVAL_ENTRY_CONTINUATION) {
        eval_entry_dispose(&e);
        return ENOTSUP;
    }
    if (e.kind != WL_COLUMNAR_EVAL_ENTRY_RELATION) {
        eval_entry_dispose(&e);
        return EINVAL;
    }
    /* A NULL relation is the legacy no-result stack value.  Preserve it and
     * let the caller apply its existing no-result behavior. */
    *out = e;
    return 0;
}

void
eval_stack_drain(eval_stack_t *s)
{
    while (s->top > 0) {
        eval_entry_t e = eval_stack_pop(s);
        eval_entry_dispose(&e);
    }
}
