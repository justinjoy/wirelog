/*
 * session_options.h - internal session creation options
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 *
 * INTERNAL HEADER - not installed and not part of the public API.
 */

#ifndef WL_SESSION_OPTIONS_H
#define WL_SESSION_OPTIONS_H

#include <stdint.h>

#define WL_SESSION_OPTIONS_VERSION UINT32_C(1)

/*
 * Options are valid only for the duration of session creation.  The Windows
 * Job Object handle is borrowed by the backend, queried synchronously, and
 * never stored or closed by Wirelog.  A void pointer keeps this header
 * portable and prevents windows.h from reaching installed headers.
 */
typedef struct {
    uint32_t size;
    uint32_t version;
    void *windows_job_handle;
} wl_session_options_t;

void
wl_session_options_init(wl_session_options_t *options);

#endif /* WL_SESSION_OPTIONS_H */
