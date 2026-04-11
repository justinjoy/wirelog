/*
 * io_adapter.c - wirelog I/O Adapter Registry
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <https://www.gnu.org/licenses/lgpl-3.0.html>.
 *
 * Mutex-guarded 32-slot adapter registry with thread-local error
 * reporting and a one-shot built-in CSV stub.
 *
 * Part of #446 (I/O adapter umbrella).
 */

#include "wirelog/io/io_adapter.h"
#include "wirelog/thread.h"
#include <string.h>
#include <stdbool.h>

/* ======================================================================== */
/* Thread-Local Error Reporting                                             */
/* ======================================================================== */

/* Thread-local error buffer: C11 _Thread_local, GCC/Clang __thread,
 * MSVC __declspec(thread). */
#if defined(_MSC_VER)
static __declspec(thread) char s_errbuf[256];
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L \
    && !defined(__STDC_NO_THREADS__)
static _Thread_local char s_errbuf[256];
#else
static __thread char s_errbuf[256];
#endif

static void
set_error(const char *msg)
{
    if (msg) {
        strncpy(s_errbuf, msg, sizeof(s_errbuf) - 1);
        s_errbuf[sizeof(s_errbuf) - 1] = '\0';
    } else {
        s_errbuf[0] = '\0';
    }
}

/* ======================================================================== */
/* Static Registry State                                                    */
/* ======================================================================== */

#define SCHEME_MAX_LEN 64

typedef struct {
    const wl_io_adapter_t *adapter;
    char scheme[SCHEME_MAX_LEN];
} registry_entry_t;

static registry_entry_t s_registry[WL_IO_MAX_ADAPTERS];
static uint32_t s_count;
/* POSIX: statically initialized via PTHREAD_MUTEX_INITIALIZER.
 * Windows: CRITICAL_SECTION cannot be statically initialized, so we
 * use a volatile flag + InterlockedCompareExchange for one-shot init. */
#if defined(_WIN32) || defined(_WIN64)
static mutex_t s_mutex;
static volatile long s_mutex_ready;  /* 0=uninit, 1=ready */
#else
static mutex_t s_mutex = { PTHREAD_MUTEX_INITIALIZER };
#endif
static bool s_initialized;

/* Built-in CSV stub adapter */
static const wl_io_adapter_t s_csv_builtin = {
    .abi_version = WL_IO_ABI_VERSION,
    .scheme = "csv",
    .description = "Built-in CSV adapter (stub)",
    .read = NULL,
    .validate = NULL,
    .user_data = NULL,
};

/* ======================================================================== */
/* One-Shot Initialization                                                  */
/* ======================================================================== */

static void
ensure_builtins(void)
{
    /* On Windows, CRITICAL_SECTION requires explicit init. Use a volatile
    * flag with InterlockedCompareExchange for the one-shot mutex_init. */
#if defined(_WIN32) || defined(_WIN64)
    if (!s_mutex_ready) {
        if (InterlockedCompareExchange(&s_mutex_ready, 1, 0) == 0)
            mutex_init(&s_mutex);
        /* Spin until the initializer thread finishes mutex_init. The flag
         * is set before mutex_init returns above, so the mutex is ready. */
    }
#endif
    /* Use the mutex (statically initialized on POSIX, dynamically on
     * Windows) to guard the one-shot builtin registration. */
    mutex_lock(&s_mutex);
    if (!s_initialized) {
        s_registry[0].adapter = &s_csv_builtin;
        strncpy(s_registry[0].scheme, "csv", SCHEME_MAX_LEN - 1);
        s_registry[0].scheme[SCHEME_MAX_LEN - 1] = '\0';
        s_count = 1;
        s_initialized = true;
    }
    mutex_unlock(&s_mutex);
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int
wl_io_register_adapter(const wl_io_adapter_t *adapter)
{
    ensure_builtins();

    if (!adapter) {
        set_error("adapter is NULL");
        return -1;
    }
    if (!adapter->scheme) {
        set_error("adapter scheme is NULL");
        return -1;
    }
    if (adapter->abi_version != WL_IO_ABI_VERSION) {
        set_error("ABI version mismatch");
        return -1;
    }

    mutex_lock(&s_mutex);

    /* Check for duplicate scheme */
    for (uint32_t i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].scheme, adapter->scheme) == 0) {
            mutex_unlock(&s_mutex);
            set_error("duplicate scheme");
            return -1;
        }
    }

    /* Check capacity */
    if (s_count >= WL_IO_MAX_ADAPTERS) {
        mutex_unlock(&s_mutex);
        set_error("registry full");
        return -1;
    }

    /* Store pointer and copy scheme for safe lookup */
    s_registry[s_count].adapter = adapter;
    strncpy(s_registry[s_count].scheme, adapter->scheme, SCHEME_MAX_LEN - 1);
    s_registry[s_count].scheme[SCHEME_MAX_LEN - 1] = '\0';
    s_count++;

    mutex_unlock(&s_mutex);
    set_error(NULL);
    return 0;
}

int
wl_io_unregister_adapter(const char *scheme)
{
    ensure_builtins();

    if (!scheme) {
        set_error("scheme is NULL");
        return -1;
    }

    mutex_lock(&s_mutex);

    for (uint32_t i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].scheme, scheme) == 0) {
            /* Shift remaining entries down */
            for (uint32_t j = i; j + 1 < s_count; j++) {
                s_registry[j] = s_registry[j + 1];
            }
            s_count--;
            memset(&s_registry[s_count], 0, sizeof(registry_entry_t));
            mutex_unlock(&s_mutex);
            set_error(NULL);
            return 0;
        }
    }

    mutex_unlock(&s_mutex);
    set_error("scheme not found");
    return -1;
}

const wl_io_adapter_t *
wl_io_find_adapter(const char *scheme)
{
    ensure_builtins();

    if (!scheme) {
        set_error("scheme is NULL");
        return NULL;
    }

    mutex_lock(&s_mutex);

    for (uint32_t i = 0; i < s_count; i++) {
        if (strcmp(s_registry[i].scheme, scheme) == 0) {
            const wl_io_adapter_t *found = s_registry[i].adapter;
            mutex_unlock(&s_mutex);
            set_error(NULL);
            return found;
        }
    }

    mutex_unlock(&s_mutex);
    set_error(NULL);
    return NULL;
}

const char *
wl_io_last_error(void)
{
    return s_errbuf;
}
