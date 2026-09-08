/* Focused contract tests for issue #1450. */

#include "../wirelog/columnar/internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int produce_calls;
    int destroy_calls;
    bool stale;
    bool empty_temporary;
    bool completion_only;
    bool unsupported;
    bool return_done;
    int cancel_calls;
    unsigned char payload[2];
} fake_producer_t;

typedef struct {
    uint64_t budget;
    uint64_t reserved;
    int begins;
    int appends;
    int commits;
    int aborts;
    int commit_mode; /* 0 success, 1 pre-commit failure, 2 post-commit */
    uint64_t reserved_before;
} fake_sink_t;

static int failures;

#define CHECK(condition, message) do { \
            if (!(condition)) { \
                fprintf(stderr, "FAIL: %s\n", message); \
                failures++; \
            } \
} while (0)

static wl_columnar_continuation_status_t
produce(void *context, const wl_columnar_continuation_cursor_t *cursor,
    wl_columnar_continuation_batch_t *batch)
{
    fake_producer_t *producer = context;

    producer->produce_calls++;
    if (producer->unsupported)
        return WL_COLUMNAR_CONTINUATION_UNSUPPORTED;
    if (producer->return_done)
        return WL_COLUMNAR_CONTINUATION_DONE;
    if (producer->completion_only) {
        batch->payload = NULL;
        batch->bytes = 0;
        batch->rows = 0;
        batch->complete = true;
        batch->next_cursor = *cursor;
        batch->next_cursor.position = UINT64_C(999);
        batch->next_cursor.sequence = UINT64_C(999);
        return WL_COLUMNAR_CONTINUATION_OK;
    }
    if (producer->empty_temporary) {
        batch->payload = producer->payload;
        batch->bytes = 1;
        batch->rows = 0;
        batch->complete = false;
        batch->next_cursor = *cursor;
        return WL_COLUMNAR_CONTINUATION_OK;
    }
    if (cursor->position >= 2)
        return WL_COLUMNAR_CONTINUATION_DONE;
    batch->payload = producer->payload + cursor->position;
    batch->bytes = cursor->position == 0 ? 8 : 16;
    batch->rows = 1;
    batch->complete = cursor->position == 1;
    batch->next_cursor = *cursor;
    batch->next_cursor.position++;
    batch->next_cursor.sequence++;
    return WL_COLUMNAR_CONTINUATION_OK;
}

static bool
validate(void *context, const wl_columnar_continuation_cursor_t *cursor)
{
    fake_producer_t *producer = context;
    (void)cursor;
    return !producer->stale;
}

static void
destroy_producer(void *context)
{
    fake_producer_t *producer = context;
    producer->destroy_calls++;
}

static void
cancel_producer(void *context)
{
    fake_producer_t *producer = context;
    producer->cancel_calls++;
}

static wl_columnar_continuation_status_t
sink_begin(void *context, const wl_columnar_continuation_batch_t *batch)
{
    fake_sink_t *sink = context;
    (void)batch;
    sink->begins++;
    sink->reserved_before = sink->reserved;
    return WL_COLUMNAR_CONTINUATION_OK;
}

static wl_columnar_continuation_status_t
sink_reserve(void *context, uint64_t bytes, uint32_t rows)
{
    fake_sink_t *sink = context;
    (void)rows;
    if (bytes > sink->budget - sink->reserved)
        return WL_COLUMNAR_CONTINUATION_RESERVATION_DENIED;
    sink->reserved += bytes;
    return WL_COLUMNAR_CONTINUATION_OK;
}

static wl_columnar_continuation_status_t
sink_append(void *context, const wl_columnar_continuation_batch_t *batch)
{
    fake_sink_t *sink = context;
    CHECK(batch->rows == 1, "sink receives a non-empty batch");
    sink->appends++;
    return WL_COLUMNAR_CONTINUATION_OK;
}

static wl_columnar_continuation_status_t
sink_commit(void *context, bool *committed)
{
    fake_sink_t *sink = context;
    sink->commits++;
    if (sink->commit_mode == 1) {
        *committed = false;
        return WL_COLUMNAR_CONTINUATION_COMMIT_FAILURE;
    }
    if (sink->commit_mode == 2) {
        sink->commit_mode = 0;
        *committed = true;
        return WL_COLUMNAR_CONTINUATION_COMMIT_FAILURE;
    }
    *committed = true;
    return WL_COLUMNAR_CONTINUATION_OK;
}

static void
sink_abort(void *context)
{
    fake_sink_t *sink = context;
    sink->aborts++;
    sink->reserved = sink->reserved_before;
}

static wl_columnar_continuation_t *
make_continuation(fake_producer_t *producer)
{
    const wl_columnar_continuation_producer_t spec = {
        producer, produce, validate, destroy_producer, cancel_producer
    };
    const wl_columnar_continuation_cursor_t cursor = {
        UINT64_C(11), UINT64_C(22), 7, 0, 0
    };
    return wl_columnar_continuation_create(&spec, &cursor);
}

static wl_columnar_continuation_sink_t
make_sink(fake_sink_t *sink)
{
    const wl_columnar_continuation_sink_t result = {
        sink, sink_begin, sink_reserve, sink_append, sink_commit, sink_abort
    };
    return result;
}

static void
test_retry_and_commit_boundary(void)
{
    fake_producer_t producer = { 0 };
    fake_sink_t sink = { UINT64_C(128), 0, 0, 0, 0, 0, 1 };
    wl_columnar_continuation_t *continuation = make_continuation(&producer);
    wl_columnar_continuation_sink_t sink_spec = make_sink(&sink);
    uint64_t position;

    CHECK(continuation != NULL, "continuation is created");
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_COMMIT_FAILURE,
        "pre-commit failure is reported");
    position = wl_columnar_continuation_cursor(continuation)->position;
    CHECK(position == 0, "pre-commit failure leaves cursor unchanged");
    CHECK(sink.aborts == 1, "pre-commit failure aborts sink");
    CHECK(sink.reserved == 0, "pre-commit abort restores reservation");

    sink.commit_mode = 0;
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_OK,
        "retry publishes the first batch");
    CHECK(wl_columnar_continuation_cursor(continuation)->position == 1,
        "cursor advances only after commit");
    CHECK(sink.appends == 2, "retry consumes the first batch once");

    sink.commit_mode = 2;
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_COMMIT_FAILURE,
        "post-commit failure is reported");
    CHECK(wl_columnar_continuation_cursor(continuation)->position == 1,
        "post-commit failure leaves the cursor unchanged");
    CHECK(!wl_columnar_continuation_is_done(continuation),
        "ambiguous commit does not mark continuation done");
    CHECK(sink.reserved == UINT64_C(24),
        "ambiguous commit does not falsely release reservation");
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_OK,
        "retry commits the still-current batch");
    CHECK(wl_columnar_continuation_is_done(continuation),
        "successful final retry marks continuation done");
    wl_columnar_continuation_destroy(continuation);
    CHECK(producer.destroy_calls == 1, "continuation owns producer lifetime");
}

static void
test_stale_and_empty_batches(void)
{
    fake_producer_t producer = { 0 };
    fake_sink_t sink = { UINT64_C(128), 0, 0, 0, 0, 0, 0 };
    wl_columnar_continuation_sink_t sink_spec = make_sink(&sink);
    wl_columnar_continuation_t *continuation;

    producer.stale = true;
    continuation = make_continuation(&producer);
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_STALE,
        "stale input is rejected before publication");
    CHECK(sink.begins == 0, "stale input does not begin a sink transaction");
    wl_columnar_continuation_destroy(continuation);

    memset(&producer, 0, sizeof(producer));
    producer.return_done = true;
    continuation = make_continuation(&producer);
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_DONE,
        "producer DONE becomes persistent terminal state");
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_DONE,
        "persistent DONE avoids another producer call");
    CHECK(producer.produce_calls == 1, "DONE producer is called only once");
    wl_columnar_continuation_destroy(continuation);

    memset(&producer, 0, sizeof(producer));
    producer.empty_temporary = true;
    continuation = make_continuation(&producer);
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_INVALID,
        "empty temporary batch is not completion");
    CHECK(wl_columnar_continuation_cursor(continuation)->position == 0,
        "invalid empty batch leaves cursor unchanged");
    wl_columnar_continuation_destroy(continuation);

    memset(&producer, 0, sizeof(producer));
    producer.completion_only = true;
    continuation = make_continuation(&producer);
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_DONE,
        "completion-only empty batch completes without publication");
    CHECK(wl_columnar_continuation_cursor(continuation)->position == 0,
        "completion-only batch cannot overwrite the cursor");
    CHECK(sink.begins == 0, "completion-only batch skips sink begin");
    wl_columnar_continuation_destroy(continuation);

    memset(&producer, 0, sizeof(producer));
    producer.unsupported = true;
    continuation = make_continuation(&producer);
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_UNSUPPORTED,
        "unsupported boundary is explicit");
    CHECK(sink.begins == 0, "unsupported boundary does not begin a sink");
    wl_columnar_continuation_cancel(continuation);
    CHECK(producer.cancel_calls == 1, "cancellation is delivered to producer");
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_INVALID,
        "cancelled continuation cannot publish");
    wl_columnar_continuation_destroy(continuation);
}

static void
test_reservation_and_stack_ownership(void)
{
    fake_producer_t producer = { 0 };
    fake_sink_t sink = { UINT64_C(4), 0, 0, 0, 0, 0, 0 };
    wl_columnar_continuation_sink_t sink_spec = make_sink(&sink);
    wl_columnar_continuation_t *continuation = make_continuation(&producer);
    eval_stack_t stack;
    col_rel_t *relation;

    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_RESERVATION_DENIED,
        "reservation denial is explicit");
    CHECK(wl_columnar_continuation_cursor(continuation)->position == 0,
        "reservation denial leaves cursor unchanged");
    wl_columnar_continuation_destroy(continuation);

    memset(&producer, 0, sizeof(producer));
    sink = (fake_sink_t){ UINT64_C(8), 0, 0, 0, 0, 0, 1, 0 };
    sink_spec = make_sink(&sink);
    continuation = make_continuation(&producer);
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_COMMIT_FAILURE,
        "exact-fit pre-commit failure is retryable");
    CHECK(sink.reserved == 0, "abort restores exact-fit reservation");
    sink.commit_mode = 0;
    CHECK(wl_columnar_continuation_publish(continuation, &sink_spec)
        == WL_COLUMNAR_CONTINUATION_OK,
        "exact-fit retry succeeds after reservation rollback");
    CHECK(sink.reserved == UINT64_C(8),
        "successful exact-fit retry retains its reservation");
    wl_columnar_continuation_destroy(continuation);

    eval_stack_init(&stack);
    relation = col_rel_new_auto("complete", 1);
    CHECK(relation != NULL, "complete relation can still be pushed");
    CHECK(eval_stack_push(&stack, relation, true) == 0,
        "complete relation push remains available");
    memset(&producer, 0, sizeof(producer));
    continuation = make_continuation(&producer);
    CHECK(eval_stack_push_continuation(&stack, continuation) == ENOTSUP,
        "unsupported continuation stack insertion is rejected");
    CHECK(producer.destroy_calls == 1,
        "rejected continuation is destroyed by the stack helper");
    eval_stack_drain(&stack);
    CHECK(stack.top == 0, "complete relation stack still drains its owner");
}

int
main(void)
{
    test_retry_and_commit_boundary();
    test_stale_and_empty_batches();
    test_reservation_and_stack_ownership();
    if (failures != 0)
        fprintf(stderr, "%d continuation contract checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
