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
#include "../ir/program.h"
#include "../wirelog.h"

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
