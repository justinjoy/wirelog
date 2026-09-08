/* arena/arena_admission.c - shared-governor glue for fixed arena slabs */

#include "arena.h"
#include "columnar/memory_governor.h"

#include <stdlib.h>

typedef struct {
    wl_columnar_memory_reservation_t reservation;
} wl_arena_admission_t;

static void
release_arena_admission(void *context)
{
    wl_arena_admission_t *admission = context;

    if (!admission)
        return;
    (void)wl_columnar_memory_release(&admission->reservation);
    free(admission);
}

wl_arena_t *
wl_arena_create_managed(size_t capacity,
    wl_columnar_memory_governor_t *governor)
{
    wl_arena_admission_t *admission;
    wl_columnar_memory_admission_status_t status;
    wl_arena_t *arena;

    if (!governor || capacity == 0)
        return wl_arena_create(capacity);

    admission = (wl_arena_admission_t *)malloc(sizeof(*admission));
    if (!admission)
        return NULL;
    wl_columnar_memory_reservation_init(&admission->reservation);
    status = wl_columnar_memory_reserve_checked(governor, capacity,
            &admission->reservation);
    if (status != WL_COLUMNAR_MEMORY_ADMISSION_OK
        && status != WL_COLUMNAR_MEMORY_ADMISSION_ADVISORY) {
        free(admission);
        return NULL;
    }

    arena = wl_arena_create_with_admission(capacity, admission,
            release_arena_admission);
    if (!arena) {
        (void)wl_columnar_memory_rollback(&admission->reservation);
        free(admission);
        return NULL;
    }
    if (!wl_columnar_memory_commit(&admission->reservation, arena)) {
        wl_arena_free(arena);
        return NULL;
    }
    return arena;
}
