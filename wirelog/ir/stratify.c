/*
 * stratify.c - wirelog Stratification & SCC Detection
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Implements dependency graph construction, Tarjan's SCC detection,
 * and stratification for Datalog programs with negation.
 */

#include "stratify.h"
#include "program.h"

#include <stdlib.h>
#include <string.h>

#define EDGE_INITIAL_CAPACITY 16

/* ======================================================================== */
/* Dependency Graph — Helpers                                               */
/* ======================================================================== */

/**
 * Find the graph-local index for a relation name.
 * Returns the index if found, or UINT32_MAX if not in graph.
 */
static uint32_t
graph_find_relation(const wl_dep_graph_t *g, const struct wirelog_program *prog,
                    const char *name)
{
    for (uint32_t i = 0; i < g->relation_count; i++) {
        uint32_t ri = g->relation_map[i];
        if (ri < prog->relation_count
            && strcmp(prog->relations[ri].name, name) == 0) {
            return i;
        }
    }
    return UINT32_MAX;
}

/**
 * Add an edge to the dependency graph.
 * Returns 0 on success, -1 on memory error.
 */
static int
graph_add_edge(wl_dep_graph_t *g, uint32_t from, uint32_t to,
               wl_dep_type_t type)
{
    if (g->edge_count >= g->edge_capacity) {
        uint32_t new_cap = g->edge_capacity == 0 ? EDGE_INITIAL_CAPACITY
                                                 : g->edge_capacity * 2;
        wl_dep_edge_t *tmp = (wl_dep_edge_t *)realloc(
            g->edges, new_cap * sizeof(wl_dep_edge_t));
        if (!tmp)
            return -1;
        g->edges = tmp;
        g->edge_capacity = new_cap;
    }

    g->edges[g->edge_count].from = from;
    g->edges[g->edge_count].to = to;
    g->edges[g->edge_count].type = type;
    g->edge_count++;
    return 0;
}

/**
 * Walk an IR tree recursively, collecting dependency edges.
 *
 * @param node    Current IR node
 * @param head_gi Graph-local index of the head (defining) relation
 * @param g       Dependency graph (edges appended)
 * @param prog    Program (for relation name lookup)
 * @param dep     Dependency type context (POSITIVE normally, NEGATION inside ANTIJOIN)
 */
static void
walk_ir_tree(const wirelog_ir_node_t *node, uint32_t head_gi, wl_dep_graph_t *g,
             const struct wirelog_program *prog, wl_dep_type_t dep)
{
    if (!node)
        return;

    switch (node->type) {
    case WIRELOG_IR_SCAN: {
        /* SCAN references a relation — add edge from head to this relation */
        const char *rel = node->relation_name;
        if (rel) {
            uint32_t to_gi = graph_find_relation(g, prog, rel);
            if (to_gi != UINT32_MAX) {
                graph_add_edge(g, head_gi, to_gi, dep);
            }
            /* EDB relations (not in graph) are silently ignored */
        }
        break;
    }

    case WIRELOG_IR_ANTIJOIN:
        /* children[0] = positive side, children[1] = negated side */
        if (node->child_count >= 1)
            walk_ir_tree(node->children[0], head_gi, g, prog, dep);
        if (node->child_count >= 2)
            walk_ir_tree(node->children[1], head_gi, g, prog, WL_DEP_NEGATION);
        break;

    case WIRELOG_IR_AGGREGATE:
        /* Aggregate child SCANs get AGGREGATION edges */
        for (uint32_t i = 0; i < node->child_count; i++)
            walk_ir_tree(node->children[i], head_gi, g, prog,
                         WL_DEP_AGGREGATION);
        break;

    default:
        /* PROJECT, FILTER, JOIN, FLATMAP, UNION — recurse into children */
        for (uint32_t i = 0; i < node->child_count; i++)
            walk_ir_tree(node->children[i], head_gi, g, prog, dep);
        break;
    }
}

/* ======================================================================== */
/* Dependency Graph — Build                                                 */
/* ======================================================================== */

wl_dep_graph_t *
wl_dep_graph_build(const struct wirelog_program *prog)
{
    if (!prog || prog->rule_count == 0)
        return NULL;

    wl_dep_graph_t *g = (wl_dep_graph_t *)calloc(1, sizeof(wl_dep_graph_t));
    if (!g)
        return NULL;

    /*
     * Step 1: Identify IDB relations (those that appear as rule heads).
     * Build relation_map: graph node index -> program relation index.
     */
    uint32_t *idb_flags
        = (uint32_t *)calloc(prog->relation_count, sizeof(uint32_t));
    if (!idb_flags) {
        free(g);
        return NULL;
    }

    /* Mark relations that are rule heads as IDB */
    for (uint32_t r = 0; r < prog->rule_count; r++) {
        const char *head = prog->rules[r].head_relation;
        if (!head)
            continue;
        for (uint32_t i = 0; i < prog->relation_count; i++) {
            if (strcmp(prog->relations[i].name, head) == 0) {
                idb_flags[i] = 1;
                break;
            }
        }
    }

    /* Count IDB relations */
    uint32_t idb_count = 0;
    for (uint32_t i = 0; i < prog->relation_count; i++) {
        if (idb_flags[i])
            idb_count++;
    }

    if (idb_count == 0) {
        free(idb_flags);
        free(g);
        return NULL;
    }

    g->relation_map = (uint32_t *)calloc(idb_count, sizeof(uint32_t));
    if (!g->relation_map) {
        free(idb_flags);
        free(g);
        return NULL;
    }

    g->relation_count = idb_count;
    uint32_t gi = 0;
    for (uint32_t i = 0; i < prog->relation_count; i++) {
        if (idb_flags[i])
            g->relation_map[gi++] = i;
    }
    free(idb_flags);

    /*
     * Step 2: Walk each rule's IR tree to extract dependency edges.
     */
    for (uint32_t r = 0; r < prog->rule_count; r++) {
        const char *head = prog->rules[r].head_relation;
        if (!head)
            continue;

        uint32_t head_gi = graph_find_relation(g, prog, head);
        if (head_gi == UINT32_MAX)
            continue;

        walk_ir_tree(prog->rules[r].ir_root, head_gi, g, prog, WL_DEP_POSITIVE);
    }

    return g;
}

void
wl_dep_graph_free(wl_dep_graph_t *graph)
{
    if (!graph)
        return;
    free(graph->relation_map);
    free(graph->edges);
    free(graph);
}

/* ======================================================================== */
/* SCC Detection — Iterative Tarjan's Algorithm                             */
/* ======================================================================== */

/* Tarjan stack frame for iterative implementation */
typedef struct {
    uint32_t node;
    uint32_t edge_idx; /* Next edge index to process for this node */
} tarjan_frame_t;

wl_scc_result_t *
wl_scc_detect(const wl_dep_graph_t *graph)
{
    if (!graph || graph->relation_count == 0)
        return NULL;

    uint32_t n = graph->relation_count;

    wl_scc_result_t *result
        = (wl_scc_result_t *)calloc(1, sizeof(wl_scc_result_t));
    if (!result)
        return NULL;

    result->scc_id = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!result->scc_id) {
        free(result);
        return NULL;
    }

    /*
     * Tarjan's algorithm state:
     * - disc[i]: discovery index of node i (UINT32_MAX = unvisited)
     * - low[i]: lowest reachable discovery index
     * - on_stack[i]: whether node is on Tarjan's stack
     * - stack[]: the Tarjan stack (node indices)
     */
    uint32_t *disc = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *low = (uint32_t *)malloc(n * sizeof(uint32_t));
    bool *on_stack = (bool *)calloc(n, sizeof(bool));
    uint32_t *stack = (uint32_t *)malloc(n * sizeof(uint32_t));
    tarjan_frame_t *call_stack
        = (tarjan_frame_t *)malloc(n * sizeof(tarjan_frame_t));

    /*
     * Build adjacency list from edges for O(V+E) traversal.
     * adj_start[i] = index into adj[] where node i's neighbors begin
     * adj[k]       = neighbor node index
     */
    uint32_t *adj_count_arr = (uint32_t *)calloc(n, sizeof(uint32_t));
    uint32_t *adj_start = (uint32_t *)calloc(n + 1, sizeof(uint32_t));
    uint32_t *adj = NULL;

    if (!disc || !low || !on_stack || !stack || !call_stack || !adj_count_arr
        || !adj_start) {
        goto cleanup_error;
    }

    /* Count outgoing edges per node */
    for (uint32_t e = 0; e < graph->edge_count; e++) {
        uint32_t from = graph->edges[e].from;
        if (from < n)
            adj_count_arr[from]++;
    }

    /* Prefix sum for adj_start */
    adj_start[0] = 0;
    for (uint32_t i = 0; i < n; i++)
        adj_start[i + 1] = adj_start[i] + adj_count_arr[i];

    uint32_t total_adj = adj_start[n];
    if (total_adj > 0) {
        adj = (uint32_t *)malloc(total_adj * sizeof(uint32_t));
        if (!adj)
            goto cleanup_error;
    }

    /* Fill adjacency list */
    memset(adj_count_arr, 0, n * sizeof(uint32_t));
    for (uint32_t e = 0; e < graph->edge_count; e++) {
        uint32_t from = graph->edges[e].from;
        uint32_t to = graph->edges[e].to;
        if (from < n && to < n) {
            adj[adj_start[from] + adj_count_arr[from]] = to;
            adj_count_arr[from]++;
        }
    }

    /* Initialize discovery array */
    for (uint32_t i = 0; i < n; i++)
        disc[i] = UINT32_MAX;

    uint32_t timer = 0;
    uint32_t stack_top = 0;
    uint32_t scc_count = 0;

    /* Iterative Tarjan's — process all nodes */
    for (uint32_t start = 0; start < n; start++) {
        if (disc[start] != UINT32_MAX)
            continue; /* Already visited */

        uint32_t cs_top = 0; /* call stack depth */

        /* Push initial frame */
        disc[start] = low[start] = timer++;
        on_stack[start] = true;
        stack[stack_top++] = start;

        call_stack[0].node = start;
        call_stack[0].edge_idx = adj_start[start];
        cs_top = 1;

        while (cs_top > 0) {
            tarjan_frame_t *frame = &call_stack[cs_top - 1];
            uint32_t v = frame->node;
            uint32_t edge_end = adj_start[v + 1];

            if (frame->edge_idx < edge_end) {
                uint32_t w = adj[frame->edge_idx];
                frame->edge_idx++;

                if (disc[w] == UINT32_MAX) {
                    /* Unvisited — push new frame (like recursive call) */
                    disc[w] = low[w] = timer++;
                    on_stack[w] = true;
                    stack[stack_top++] = w;

                    call_stack[cs_top].node = w;
                    call_stack[cs_top].edge_idx = adj_start[w];
                    cs_top++;
                } else if (on_stack[w]) {
                    if (disc[w] < low[v])
                        low[v] = disc[w];
                }
            } else {
                /* All neighbors processed — check if v is SCC root */
                if (low[v] == disc[v]) {
                    /* Pop SCC from stack */
                    uint32_t w;
                    do {
                        w = stack[--stack_top];
                        on_stack[w] = false;
                        result->scc_id[w] = scc_count;
                    } while (w != v);
                    scc_count++;
                }

                /* Pop call stack and update parent's low-link */
                cs_top--;
                if (cs_top > 0) {
                    uint32_t parent = call_stack[cs_top - 1].node;
                    if (low[v] < low[parent])
                        low[parent] = low[v];
                }
            }
        }
    }

    result->scc_count = scc_count;

    /* Cleanup success path */
    free(adj);
    free(adj_start);
    free(adj_count_arr);
    free(call_stack);
    free(stack);
    free(on_stack);
    free(low);
    free(disc);
    return result;

cleanup_error:
    free(adj);
    free(adj_start);
    free(adj_count_arr);
    free(call_stack);
    free(stack);
    free(on_stack);
    free(low);
    free(disc);
    wl_scc_free(result);
    return NULL;
}

void
wl_scc_free(wl_scc_result_t *result)
{
    if (!result)
        return;
    free(result->scc_id);
    free(result);
}

/* ======================================================================== */
/* Stratification Pipeline                                                  */
/* ======================================================================== */

int
wl_program_stratify(struct wirelog_program *program)
{
    (void)program;
    return -1; /* STUB: to be implemented in Commit 3 */
}
