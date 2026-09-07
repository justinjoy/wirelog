/*
 * csv_adapter_internal.h - built-in CSV streaming entry point
 *
 * Internal header; this is deliberately not installed and does not extend
 * the public wirelog_io_adapter_t ABI.
 */

#ifndef WL_IO_CSV_ADAPTER_INTERNAL_H
#define WL_IO_CSV_ADAPTER_INTERNAL_H

#include "io_adapter.h"
#include "csv_reader.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WL_CSV_STREAM_DEFAULT_BATCH_ROWS 1024u

int
wl_csv_adapter_stream_read(wirelog_io_ctx_t *ctx, uint32_t max_batch_rows,
    wl_csv_batch_cb batch_cb, void *opaque);

extern const wirelog_io_adapter_t wl_csv_adapter;

#ifdef __cplusplus
}
#endif

#endif /* WL_IO_CSV_ADAPTER_INTERNAL_H */
