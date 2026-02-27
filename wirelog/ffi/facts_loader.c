/*
 * facts_loader.c - Bulk EDB Fact Loading via Rust FFI
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Implements wirelog_load_all_facts(), which bridges the parser's inline
 * fact storage with the DD execution engine.  This file is compiled only
 * into targets that link the Rust FFI (rust_ffi_dep), keeping the core
 * IR library free of Rust dependencies.
 */

#include "dd_ffi.h"
#include "../io/csv_reader.h"
#include "../ir/program.h"
#include "../wirelog.h"

#include <stdlib.h>
#include <string.h>

int
wirelog_load_all_facts(const wirelog_program_t *prog, void *worker)
{
    if (!prog || !worker)
        return -1;

    wl_dd_worker_t *w = (wl_dd_worker_t *)worker;

    for (uint32_t i = 0; i < prog->relation_count; i++) {
        const wl_relation_info_t *rel = &prog->relations[i];
        if (rel->fact_count == 0)
            continue;

        int rc = wl_dd_load_edb(w, rel->name, rel->fact_data, rel->fact_count,
                                rel->column_count);
        if (rc != 0)
            return -1;
    }

    return 0;
}

/* Look up a parameter value by name from .input directive */
static const char *
input_param_get(const wl_relation_info_t *rel, const char *name)
{
    for (uint32_t i = 0; i < rel->input_param_count; i++) {
        if (strcmp(rel->input_param_names[i], name) == 0)
            return rel->input_param_values[i];
    }
    return NULL;
}

int
wirelog_load_input_files(const wirelog_program_t *prog, void *worker)
{
    if (!prog || !worker)
        return -1;

    wl_dd_worker_t *w = (wl_dd_worker_t *)worker;

    for (uint32_t i = 0; i < prog->relation_count; i++) {
        const wl_relation_info_t *rel = &prog->relations[i];
        if (!rel->has_input)
            continue;

        const char *filename = input_param_get(rel, "filename");
        if (!filename)
            return -1; /* .input without filename */

        const char *delim_str = input_param_get(rel, "delimiter");
        char delimiter = ','; /* default */
        if (delim_str && delim_str[0] != '\0') {
            if (strcmp(delim_str, "\\t") == 0)
                delimiter = '\t';
            else
                delimiter = delim_str[0];
        }

        int64_t *data = NULL;
        uint32_t nrows = 0, ncols = 0;
        int rc = wl_csv_read_file(filename, delimiter, &data, &nrows, &ncols);
        if (rc != 0)
            return -1;

        if (nrows > 0) {
            rc = wl_dd_load_edb(w, rel->name, data, nrows, ncols);
            free(data);
            if (rc != 0)
                return -1;
        } else {
            free(data);
        }
    }

    return 0;
}
