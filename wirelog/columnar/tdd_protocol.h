/*
 * tdd_protocol.h - fake TDD publication protocol contract for tests
 *
 * INTERNAL TEST HARNESS ONLY.  This is a small executable model of the
 * ownership and ordering contract discussed by issues #1454, #1373, #1372,
 * #1369, and #1451.  It is not the production evaluator transport.
 */
#ifndef WL_COLUMNAR_TDD_PROTOCOL_H
#define WL_COLUMNAR_TDD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct wl_columnar_tdd_protocol wl_columnar_tdd_protocol_t;

typedef struct wl_columnar_tdd_reservation {
    uint64_t id;
    unsigned transfer_count;
    unsigned release_count;
} wl_columnar_tdd_reservation_t;

typedef struct wl_columnar_tdd_payload {
    uint64_t id;
    uint32_t worker;
    uint32_t relation;
    uint32_t round;
    uint32_t batch;
    wl_columnar_tdd_reservation_t *reservation;
    unsigned release_count;
} wl_columnar_tdd_payload_t;

typedef enum wl_columnar_tdd_submit_result {
    WL_COLUMNAR_TDD_SUBMITTED = 0,
    WL_COLUMNAR_TDD_DUPLICATE = 1,
    WL_COLUMNAR_TDD_SUBMIT_CANCELLED = 2,
    WL_COLUMNAR_TDD_ALREADY_COMPLETE = 3
} wl_columnar_tdd_submit_result_t;

typedef enum wl_columnar_tdd_pump_result {
    WL_COLUMNAR_TDD_EMPTY = 0, /* temporary: publication is still possible */
    WL_COLUMNAR_TDD_DATA = 1,
    WL_COLUMNAR_TDD_COMPLETE = 2,
    WL_COLUMNAR_TDD_CANCELLED = 3
} wl_columnar_tdd_pump_result_t;

typedef struct wl_columnar_tdd_message {
    wl_columnar_tdd_pump_result_t kind;
    wl_columnar_tdd_payload_t *payload; /* owned by caller after DATA */
} wl_columnar_tdd_message_t;

wl_columnar_tdd_protocol_t *wl_columnar_tdd_protocol_create(size_t capacity);
void wl_columnar_tdd_protocol_cancel(wl_columnar_tdd_protocol_t *protocol);
void wl_columnar_tdd_protocol_destroy(wl_columnar_tdd_protocol_t *protocol);

wl_columnar_tdd_submit_result_t wl_columnar_tdd_protocol_submit(
    wl_columnar_tdd_protocol_t *protocol,
    wl_columnar_tdd_payload_t *payload);
wl_columnar_tdd_submit_result_t wl_columnar_tdd_protocol_complete(
    wl_columnar_tdd_protocol_t *protocol);

wl_columnar_tdd_pump_result_t wl_columnar_tdd_protocol_pump(
    wl_columnar_tdd_protocol_t *protocol,
    wl_columnar_tdd_message_t *message);

bool wl_columnar_tdd_protocol_wait_full_with_blocked_submitter(
    wl_columnar_tdd_protocol_t *protocol,
    unsigned timeout_ms);

/* Release is used by both the consumer and cancellation/discard paths. */
void wl_columnar_tdd_payload_release(wl_columnar_tdd_payload_t *payload);
void wl_columnar_tdd_message_release(wl_columnar_tdd_message_t *message);

#endif
