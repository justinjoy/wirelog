/*
 * test_program.c - wirelog Program + IR Conversion Test Suite
 *
 * Copyright (C) CleverPlant
 * Licensed under LGPL-3.0
 * For commercial licenses, contact: inquiry@cleverplant.com
 *
 * Tests written first (TDD) before program implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../wirelog/parser/parser.h"
#include "../wirelog/ir/program.h"

/* ======================================================================== */
/* Test Helpers                                                             */
/* ======================================================================== */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  [TEST] %-55s", name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { \
        tests_passed++; \
        printf(" PASS\n"); \
    } while (0)

#define FAIL(msg) \
    do { \
        tests_failed++; \
        printf(" FAIL: %s\n", msg); \
    } while (0)

/* Helper: parse string and build program */
static struct wirelog_program*
make_program(const char *source)
{
    char errbuf[512] = {0};
    wl_ast_node_t *ast = wl_parse_string(source, errbuf, sizeof(errbuf));
    if (!ast) return NULL;

    struct wirelog_program *prog = wl_program_create();
    if (!prog) {
        wl_ast_node_free(ast);
        return NULL;
    }

    prog->ast = ast;
    if (wl_program_collect_metadata(prog, ast) != 0) {
        wl_program_free(prog);
        return NULL;
    }

    return prog;
}

/* ======================================================================== */
/* Metadata Collection Tests                                                */
/* ======================================================================== */

static void
test_decl_single_relation(void)
{
    TEST("Parse .decl with 2 columns");

    struct wirelog_program *prog = make_program(
        ".decl Arc(x: int32, y: int32)\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    if (prog->relation_count != 1) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected 1 relation, got %u", prog->relation_count);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    if (strcmp(prog->relations[0].name, "Arc") != 0) {
        wl_program_free(prog);
        FAIL("relation name should be Arc");
        return;
    }

    if (prog->relations[0].column_count != 2) {
        wl_program_free(prog);
        FAIL("should have 2 columns");
        return;
    }

    if (strcmp(prog->relations[0].columns[0].name, "x") != 0) {
        wl_program_free(prog);
        FAIL("column 0 name should be x");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_input_directive(void)
{
    TEST("Parse .input marks relation has_input");

    struct wirelog_program *prog = make_program(
        ".decl Arc(x: int32, y: int32)\n"
        ".input Arc(filename=\"data.csv\", delimiter=\"\\t\")\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    if (!prog->relations[0].has_input) {
        wl_program_free(prog);
        FAIL("has_input should be true");
        return;
    }

    if (prog->relations[0].input_param_count < 1) {
        wl_program_free(prog);
        FAIL("should have input params");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_output_directive(void)
{
    TEST("Parse .output marks relation has_output");

    struct wirelog_program *prog = make_program(
        ".decl Reach(x: int32, y: int32)\n"
        ".output Reach\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    if (!prog->relations[0].has_output) {
        wl_program_free(prog);
        FAIL("has_output should be true");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_printsize_directive(void)
{
    TEST("Parse .printsize marks relation has_printsize");

    struct wirelog_program *prog = make_program(
        ".decl Tc(x: int32, y: int32)\n"
        ".printsize Tc\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    if (!prog->relations[0].has_printsize) {
        wl_program_free(prog);
        FAIL("has_printsize should be true");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_full_tc_metadata(void)
{
    TEST("Full TC program: 2 relations with correct metadata");

    struct wirelog_program *prog = make_program(
        ".decl Arc(x: int32, y: int32)\n"
        ".decl Tc(x: int32, y: int32)\n"
        ".input Arc(filename=\"arc.csv\")\n"
        ".output Tc\n"
        "Tc(x, y) :- Arc(x, y).\n"
        "Tc(x, y) :- Tc(x, z), Arc(z, y).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    if (prog->relation_count != 2) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected 2 relations, got %u", prog->relation_count);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    /* Find Arc and Tc */
    int arc_idx = -1, tc_idx = -1;
    for (uint32_t i = 0; i < prog->relation_count; i++) {
        if (strcmp(prog->relations[i].name, "Arc") == 0) arc_idx = (int)i;
        if (strcmp(prog->relations[i].name, "Tc") == 0) tc_idx = (int)i;
    }

    if (arc_idx < 0 || tc_idx < 0) {
        wl_program_free(prog);
        FAIL("should have Arc and Tc relations");
        return;
    }

    if (!prog->relations[arc_idx].has_input) {
        wl_program_free(prog);
        FAIL("Arc should have has_input");
        return;
    }

    if (!prog->relations[tc_idx].has_output) {
        wl_program_free(prog);
        FAIL("Tc should have has_output");
        return;
    }

    if (prog->rule_count != 2) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected 2 rules, got %u", prog->rule_count);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_no_rules_program(void)
{
    TEST("Program with no rules has 0 rule_count");

    struct wirelog_program *prog = make_program(
        ".decl Arc(x: int32, y: int32)\n"
        ".input Arc(filename=\"data.csv\")\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    if (prog->rule_count != 0) {
        wl_program_free(prog);
        FAIL("rule_count should be 0");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_schema_synthesis(void)
{
    TEST("Schema synthesis from relation metadata");

    struct wirelog_program *prog = make_program(
        ".decl Arc(x: int32, y: int32)\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wl_program_build_schemas(prog);

    if (!prog->schemas) {
        wl_program_free(prog);
        FAIL("schemas should not be NULL");
        return;
    }

    if (strcmp(prog->schemas[0].relation_name, "Arc") != 0) {
        wl_program_free(prog);
        FAIL("schema relation_name should be Arc");
        return;
    }

    if (prog->schemas[0].column_count != 2) {
        wl_program_free(prog);
        FAIL("schema column_count should be 2");
        return;
    }

    if (strcmp(prog->schemas[0].columns[0].name, "x") != 0) {
        wl_program_free(prog);
        FAIL("schema column 0 name should be x");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_default_stratum(void)
{
    TEST("Default stratum contains all rule head names");

    struct wirelog_program *prog = make_program(
        ".decl Arc(x: int32, y: int32)\n"
        ".decl Tc(x: int32, y: int32)\n"
        "Tc(x, y) :- Arc(x, y).\n"
        "Tc(x, y) :- Tc(x, z), Arc(z, y).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wl_program_build_default_stratum(prog);

    if (prog->stratum_count != 1) {
        wl_program_free(prog);
        FAIL("stratum_count should be 1");
        return;
    }

    if (prog->strata[0].stratum_id != 0) {
        wl_program_free(prog);
        FAIL("stratum_id should be 0");
        return;
    }

    if (prog->strata[0].rule_count != 2) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected 2 rule names, got %u", prog->strata[0].rule_count);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_program_free_null(void)
{
    TEST("Program free handles NULL safely");

    wl_program_free(NULL);

    PASS();
}

/* Helper: parse string, build program, and convert rules */
static struct wirelog_program*
make_program_with_rules(const char *source)
{
    struct wirelog_program *prog = make_program(source);
    if (!prog) return NULL;

    if (wl_program_convert_rules(prog, prog->ast) != 0) {
        wl_program_free(prog);
        return NULL;
    }

    return prog;
}

/* ======================================================================== */
/* Rule Conversion Tests                                                    */
/* ======================================================================== */

static void
test_simple_rule(void)
{
    TEST("Simple rule r(x) :- a(x). -> PROJECT over SCAN");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }
    if (prog->rule_count != 1 || !prog->rules[0].ir_root) {
        wl_program_free(prog);
        FAIL("should have 1 rule with IR");
        return;
    }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    if (root->child_count != 1 || root->children[0]->type != WIRELOG_IR_SCAN) {
        wl_program_free(prog);
        FAIL("PROJECT child should be SCAN");
        return;
    }

    if (strcmp(root->children[0]->relation_name, "a") != 0) {
        wl_program_free(prog);
        FAIL("SCAN relation should be a");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_two_body_join(void)
{
    TEST("Two-body join: PROJECT over JOIN(SCAN, SCAN)");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl Tc(x: int32, y: int32)\n"
        ".decl Arc(x: int32, y: int32)\n"
        "Tc(x, y) :- Tc(x, z), Arc(z, y).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    wirelog_ir_node_t *join = root->children[0];
    if (join->type != WIRELOG_IR_JOIN) {
        wl_program_free(prog);
        FAIL("child should be JOIN");
        return;
    }

    if (join->child_count != 2) {
        wl_program_free(prog);
        FAIL("JOIN should have 2 children");
        return;
    }

    if (join->children[0]->type != WIRELOG_IR_SCAN ||
        join->children[1]->type != WIRELOG_IR_SCAN) {
        wl_program_free(prog);
        FAIL("JOIN children should be SCANs");
        return;
    }

    /* Verify join key is z */
    if (join->join_key_count != 1) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected 1 join key, got %u", join->join_key_count);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    if (strcmp(join->join_left_keys[0], "z") != 0) {
        wl_program_free(prog);
        FAIL("join key should be z");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_comparison_filter(void)
{
    TEST("Comparison: PROJECT over FILTER over JOIN");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl edge(x: int32, y: int32)\n"
        ".decl sg(x: int32, y: int32)\n"
        "sg(x, y) :- edge(z, x), edge(z, y), x != y.\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    wirelog_ir_node_t *filter = root->children[0];
    if (filter->type != WIRELOG_IR_FILTER) {
        wl_program_free(prog);
        FAIL("child should be FILTER");
        return;
    }

    if (!filter->filter_expr || filter->filter_expr->type != WL_IR_EXPR_CMP) {
        wl_program_free(prog);
        FAIL("filter should have CMP expression");
        return;
    }

    if (filter->filter_expr->cmp_op != WL_CMP_NEQ) {
        wl_program_free(prog);
        FAIL("cmp_op should be NEQ");
        return;
    }

    wirelog_ir_node_t *join = filter->children[0];
    if (join->type != WIRELOG_IR_JOIN) {
        wl_program_free(prog);
        FAIL("filter child should be JOIN");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_negation_antijoin(void)
{
    TEST("Negation: PROJECT over ANTIJOIN(SCAN, SCAN)");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl node(x: int32)\n"
        ".decl edge(x: int32, y: int32)\n"
        ".decl isolated(x: int32)\n"
        "isolated(x) :- node(x), !edge(x, _).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    wirelog_ir_node_t *antijoin = root->children[0];
    if (antijoin->type != WIRELOG_IR_ANTIJOIN) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected ANTIJOIN, got type %d", antijoin->type);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    if (antijoin->child_count != 2) {
        wl_program_free(prog);
        FAIL("ANTIJOIN should have 2 children");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_aggregation_simple(void)
{
    TEST("Aggregation: AGGREGATE over SCAN");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl sssp2(x: int32, d: int32)\n"
        ".decl sssp(x: int32, d: int32)\n"
        "sssp(x, min(d)) :- sssp2(x, d).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_AGGREGATE) {
        wl_program_free(prog);
        FAIL("root should be AGGREGATE");
        return;
    }

    if (root->agg_fn != WL_AGG_MIN) {
        wl_program_free(prog);
        FAIL("agg_fn should be MIN");
        return;
    }

    if (root->child_count != 1 || root->children[0]->type != WIRELOG_IR_SCAN) {
        wl_program_free(prog);
        FAIL("AGGREGATE child should be SCAN");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_aggregation_with_join(void)
{
    TEST("Aggregation+join: AGGREGATE over JOIN");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl sssp2(x: int32, d: int32)\n"
        ".decl arc(x: int32, y: int32, d: int32)\n"
        "sssp2(y, min(d1 + d2)) :- sssp2(x, d1), arc(x, y, d2).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_AGGREGATE) {
        wl_program_free(prog);
        FAIL("root should be AGGREGATE");
        return;
    }

    if (!root->agg_expr) {
        wl_program_free(prog);
        FAIL("agg_expr should not be NULL");
        return;
    }

    /* agg_expr should be an arithmetic expression (d1 + d2) */
    if (root->agg_expr->type != WL_IR_EXPR_ARITH) {
        wl_program_free(prog);
        FAIL("agg_expr should be ARITH");
        return;
    }

    wirelog_ir_node_t *join = root->children[0];
    if (join->type != WIRELOG_IR_JOIN) {
        wl_program_free(prog);
        FAIL("AGGREGATE child should be JOIN");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_aggregation_constant(void)
{
    TEST("Aggregation with constant: min(0)");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl id(x: int32)\n"
        ".decl r(x: int32, d: int32)\n"
        "r(x, min(0)) :- id(x).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_AGGREGATE) {
        wl_program_free(prog);
        FAIL("root should be AGGREGATE");
        return;
    }

    if (!root->agg_expr || root->agg_expr->type != WL_IR_EXPR_CONST_INT) {
        wl_program_free(prog);
        FAIL("agg_expr should be CONST_INT");
        return;
    }

    if (root->agg_expr->int_value != 0) {
        wl_program_free(prog);
        FAIL("agg_expr value should be 0");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_three_body_join(void)
{
    TEST("Three-body join: left-deep JOIN tree");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32, y: int32)\n"
        ".decl b(y: int32, z: int32)\n"
        ".decl c(z: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x, y), b(y, z), c(z).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    /* Should be: PROJECT -> JOIN(JOIN(SCAN_a, SCAN_b), SCAN_c) */
    wirelog_ir_node_t *outer_join = root->children[0];
    if (outer_join->type != WIRELOG_IR_JOIN) {
        wl_program_free(prog);
        FAIL("child should be JOIN");
        return;
    }

    if (outer_join->child_count != 2) {
        wl_program_free(prog);
        FAIL("outer JOIN should have 2 children");
        return;
    }

    wirelog_ir_node_t *inner_join = outer_join->children[0];
    if (inner_join->type != WIRELOG_IR_JOIN) {
        wl_program_free(prog);
        FAIL("left child should be inner JOIN");
        return;
    }

    if (outer_join->children[1]->type != WIRELOG_IR_SCAN) {
        wl_program_free(prog);
        FAIL("right child should be SCAN");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_duplicate_variable(void)
{
    TEST("Duplicate variable a(x,x) -> FILTER(col0=col1)");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32, y: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x, x).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    /* Should be PROJECT -> FILTER -> SCAN */
    wirelog_ir_node_t *filter = root->children[0];
    if (filter->type != WIRELOG_IR_FILTER) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected FILTER, got type %d", filter->type);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    if (!filter->filter_expr || filter->filter_expr->type != WL_IR_EXPR_CMP) {
        wl_program_free(prog);
        FAIL("filter should have CMP expr (col0 = col1)");
        return;
    }

    if (filter->filter_expr->cmp_op != WL_CMP_EQ) {
        wl_program_free(prog);
        FAIL("cmp_op should be EQ");
        return;
    }

    if (filter->children[0]->type != WIRELOG_IR_SCAN) {
        wl_program_free(prog);
        FAIL("FILTER child should be SCAN");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_constant_in_atom(void)
{
    TEST("Constant in atom a(x, 42) -> FILTER(col1=42)");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32, y: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x, 42).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    wirelog_ir_node_t *filter = root->children[0];
    if (filter->type != WIRELOG_IR_FILTER) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected FILTER, got type %d", filter->type);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    /* The filter expression should compare a var with the constant 42 */
    if (!filter->filter_expr || filter->filter_expr->type != WL_IR_EXPR_CMP) {
        wl_program_free(prog);
        FAIL("filter should have CMP expr");
        return;
    }

    /* One child should be CONST_INT with value 42 */
    wl_ir_expr_t *expr = filter->filter_expr;
    bool found_42 = false;
    for (uint32_t i = 0; i < expr->child_count; i++) {
        if (expr->children[i]->type == WL_IR_EXPR_CONST_INT &&
            expr->children[i]->int_value == 42) {
            found_42 = true;
        }
    }

    if (!found_42) {
        wl_program_free(prog);
        FAIL("filter should reference constant 42");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_wildcard_not_join_key(void)
{
    TEST("Wildcard _ excluded from join keys");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32, y: int32)\n"
        ".decl b(z: int32, w: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x, _), b(_, x).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    /* Should be PROJECT -> JOIN with key x only */
    wirelog_ir_node_t *join_node = root->children[0];

    /* May have filters wrapping, find the JOIN */
    while (join_node && join_node->type != WIRELOG_IR_JOIN && join_node->child_count > 0) {
        join_node = join_node->children[0];
    }

    if (!join_node || join_node->type != WIRELOG_IR_JOIN) {
        wl_program_free(prog);
        FAIL("should have a JOIN node");
        return;
    }

    /* Join key should be x, not _ */
    if (join_node->join_key_count != 1) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected 1 join key, got %u", join_node->join_key_count);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    if (strcmp(join_node->join_left_keys[0], "x") != 0) {
        char buf[100];
        snprintf(buf, sizeof(buf), "join key should be x, got %s", join_node->join_left_keys[0]);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_wildcard_in_scan(void)
{
    TEST("Wildcard in SCAN stored as NULL column");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32, y: int32, z: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x, _, _).\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    /* Find the SCAN node */
    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    wirelog_ir_node_t *scan = root;
    while (scan && scan->type != WIRELOG_IR_SCAN && scan->child_count > 0) {
        scan = scan->children[0];
    }

    if (!scan || scan->type != WIRELOG_IR_SCAN) {
        wl_program_free(prog);
        FAIL("should have SCAN node");
        return;
    }

    if (scan->column_count != 3) {
        wl_program_free(prog);
        FAIL("SCAN should have 3 columns");
        return;
    }

    /* column 0 should be "x", columns 1 and 2 should be NULL (wildcard) */
    if (!scan->column_names[0] || strcmp(scan->column_names[0], "x") != 0) {
        wl_program_free(prog);
        FAIL("column 0 should be x");
        return;
    }

    if (scan->column_names[1] != NULL) {
        wl_program_free(prog);
        FAIL("column 1 should be NULL (wildcard)");
        return;
    }

    if (scan->column_names[2] != NULL) {
        wl_program_free(prog);
        FAIL("column 2 should be NULL (wildcard)");
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_boolean_true_noop(void)
{
    TEST("Boolean True in body -> no FILTER added");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x), True.\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    /* Should be PROJECT -> SCAN (no FILTER for True) */
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    if (root->children[0]->type != WIRELOG_IR_SCAN) {
        char buf[100];
        snprintf(buf, sizeof(buf), "expected SCAN, got type %d (True should be no-op)", root->children[0]->type);
        wl_program_free(prog);
        FAIL(buf);
        return;
    }

    wl_program_free(prog);
    PASS();
}

static void
test_boolean_false_filter(void)
{
    TEST("Boolean False in body -> FILTER(false) added");

    struct wirelog_program *prog = make_program_with_rules(
        ".decl a(x: int32)\n"
        ".decl r(x: int32)\n"
        "r(x) :- a(x), False.\n"
    );

    if (!prog) { FAIL("program is NULL"); return; }

    wirelog_ir_node_t *root = prog->rules[0].ir_root;
    if (root->type != WIRELOG_IR_PROJECT) {
        wl_program_free(prog);
        FAIL("root should be PROJECT");
        return;
    }

    wirelog_ir_node_t *filter = root->children[0];
    if (filter->type != WIRELOG_IR_FILTER) {
        wl_program_free(prog);
        FAIL("child should be FILTER");
        return;
    }

    if (!filter->filter_expr || filter->filter_expr->type != WL_IR_EXPR_BOOL) {
        wl_program_free(prog);
        FAIL("filter should have BOOL expr");
        return;
    }

    if (filter->filter_expr->bool_value != false) {
        wl_program_free(prog);
        FAIL("bool_value should be false");
        return;
    }

    wl_program_free(prog);
    PASS();
}

/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */

int
main(void)
{
    printf("\n=== wirelog Program Tests ===\n\n");

    /* Metadata collection */
    test_decl_single_relation();
    test_input_directive();
    test_output_directive();
    test_printsize_directive();
    test_full_tc_metadata();
    test_no_rules_program();

    /* Schema and stratum synthesis */
    test_schema_synthesis();
    test_default_stratum();

    /* Safety */
    test_program_free_null();

    /* Rule conversion */
    test_simple_rule();
    test_two_body_join();
    test_comparison_filter();
    test_negation_antijoin();
    test_aggregation_simple();
    test_aggregation_with_join();
    test_aggregation_constant();
    test_three_body_join();
    test_duplicate_variable();
    test_constant_in_atom();
    test_wildcard_not_join_key();
    test_wildcard_in_scan();
    test_boolean_true_noop();
    test_boolean_false_filter();

    printf("\n=== Results: %d passed, %d failed, %d total ===\n\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
