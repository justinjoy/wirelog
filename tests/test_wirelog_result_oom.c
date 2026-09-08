/* Failure-injection coverage for issue #1418 public result admission. */

#include <stddef.h>
#include <stdio.h>

#include "wirelog/wirelog.h"

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

static long fail_at = -1;
static long allocation_calls;

void *
__wrap_malloc(size_t size)
{
    if (fail_at >= 0 && allocation_calls++ == fail_at)
        return NULL;
    return __real_malloc(size);
}

void *
__wrap_calloc(size_t count, size_t size)
{
    if (fail_at >= 0 && allocation_calls++ == fail_at)
        return NULL;
    return __real_calloc(count, size);
}

void *
__wrap_realloc(void *ptr, size_t size)
{
    if (fail_at >= 0 && allocation_calls++ == fail_at)
        return NULL;
    return __real_realloc(ptr, size);
}

int
main(void)
{
    const char *source =
        ".decl edge(x:int32,y:int32)\n"
        ".decl path(x:int32,y:int32)\n"
        "edge(1,2).\n"
        "edge(2,3).\n"
        "edge(3,4).\n"
        "edge(4,5).\n"
        "edge(5,6).\n"
        "edge(6,7).\n"
        "edge(7,8).\n"
        "edge(8,9).\n"
        "edge(9,10).\n"
        "edge(10,11).\n"
        "edge(11,12).\n"
        "edge(12,13).\n"
        "edge(13,14).\n"
        "edge(14,15).\n"
        "edge(15,16).\n"
        "edge(16,17).\n"
        "edge(17,18).\n"
        "edge(18,19).\n"
        "edge(19,20).\n"
        "edge(20,21).\n"
        "edge(21,22).\n"
        "edge(22,23).\n"
        "edge(23,24).\n"
        "edge(24,25).\n"
        "edge(25,26).\n"
        "edge(26,27).\n"
        "edge(27,28).\n"
        "edge(28,29).\n"
        "edge(29,30).\n"
        "edge(30,31).\n"
        "edge(31,32).\n"
        "edge(32,33).\n"
        "path(X,Y) :- edge(X,Y).\n";
    wirelog_error_t error = WIRELOG_ERR_UNKNOWN;
    wirelog_program_t *program = wirelog_parse_string(source, &error);
    wirelog_executor_t *executor;

    if (!program || error != WIRELOG_OK)
        return 1;
    executor = wirelog_executor_create(program, &error);
    if (!executor || error != WIRELOG_OK) {
        wirelog_program_free(program);
        return 1;
    }

    /* Each injected failure must either be reported or leave a complete,
     * valid result.  In particular, no allocation failure may be returned as
     * a successful result with truncated rows. */
    for (fail_at = 0; fail_at < 256; fail_at++) {
        wirelog_result_t *result;
        allocation_calls = 0;
        error = WIRELOG_ERR_UNKNOWN;
        result = wirelog_evaluate(executor, &error);
        if (!result) {
            if (error != WIRELOG_ERR_MEMORY && error != WIRELOG_ERR_EXEC)
                goto fail;
            continue;
        }
        if (error != WIRELOG_OK
            || wirelog_result_relation_cardinality(result, "path") != 32) {
            wirelog_result_free(result);
            goto fail;
        }
        wirelog_result_free(result);
    }

    fail_at = -1;
    allocation_calls = 0;
    error = WIRELOG_ERR_UNKNOWN;
    wirelog_result_t *result = wirelog_evaluate(executor, &error);
    if (!result || error != WIRELOG_OK
        || wirelog_result_relation_cardinality(result, "path") != 32) {
        wirelog_result_free(result);
        goto fail;
    }
    wirelog_result_free(result);
    wirelog_executor_free(executor);
    wirelog_program_free(program);
    puts("test_wirelog_result_oom: OK");
    return 0;

fail:
    wirelog_executor_free(executor);
    wirelog_program_free(program);
    return 1;
}
