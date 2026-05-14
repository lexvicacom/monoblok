#include "pb_eval.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) fail(__FILE__, __LINE__, #expr); } while (0)

typedef struct published {
    pb_slice subjects[8];
    pb_slice payloads[8];
    int count;
} published;

static void fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    exit(1);
}

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    published *p = ctx;
    if (p->count < 8) {
        p->subjects[p->count] = subject;
        p->payloads[p->count] = payload;
    }
    p->count += 1;
    return true;
}

static void check_text(pb_slice s, const char *want) {
    CHECK(s.len == strlen(want));
    CHECK(memcmp(s.ptr, want, s.len) == 0);
}

static pb_eval_result eval_src_with_payload_state_clock(pb_arena *arena, pb_eval_state *state,
                                                        const char *src, const char *payload,
                                                        uint64_t now_ms, int64_t wall_ms, published *pub);

static pb_eval_result eval_src_with_payload_and_state(pb_arena *arena, pb_eval_state *state,
                                                      const char *src, const char *payload, published *pub) {
    return eval_src_with_payload_state_clock(arena, state, src, payload, 0, 0, pub);
}

static pb_eval_result eval_src_with_payload_state_clock(pb_arena *arena, pb_eval_state *state,
                                                        const char *src, const char *payload,
                                                        uint64_t now_ms, int64_t wall_ms, published *pub) {
    pb_parse_result parsed = pb_parse_all(arena, src, strlen(src));
    CHECK(parsed.err == PB_PARSE_OK);
    CHECK(parsed.forms.len == 1);
    pb_eval_ctx ctx = {
        .arena = arena,
        .state = state,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .subject = {.ptr = "sensors.temp", .len = 12},
        .payload = {.ptr = payload, .len = strlen(payload)},
        .publish = publish_cb,
        .publish_ctx = pub,
    };
    return pb_eval(&ctx, parsed.forms.items[0]);
}

static pb_eval_result eval_src_with_payload(pb_arena *arena, const char *src, const char *payload, published *pub) {
    pb_eval_state state = {0};
    pb_eval_result r = eval_src_with_payload_and_state(arena, &state, src, payload, pub);
    pb_eval_state_free(&state);
    return r;
}

static pb_eval_result eval_src(pb_arena *arena, const char *src, published *pub) {
    return eval_src_with_payload(arena, src, "42", pub);
}

static void test_bound_symbols_and_math(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src(&arena, "(+ payload-float 8)", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 50.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload(&arena, "(+ payload-float 8)", "-5", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 3.0) < 0.00001);
    pb_arena_free(&arena);
}

static void test_contains_string_and_vector(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src(&arena, "(contains? payload \"4\")", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(contains? [1 42 100] payload-float)", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);
}

static void test_if_when_do(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src(&arena, "(when (> payload-float 30) (str-concat subject \":\" payload))", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_STRING);
    check_text(r.value.text, "sensors.temp:42");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(and (> payload-float 30) (starts-with? subject \"sensors.\"))", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(or nil false (ends-with? payload \"2\"))", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);
}

static void test_subject_helpers(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src(&arena, "(subject-append \"high\")", &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "sensors.temp.high");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(subject-token 1)", &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "temp");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(subject-with [\"alerts\" (subject-token 1)])", &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "alerts.temp");
    pb_arena_free(&arena);
}

static void test_publish(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src(&arena, "(publish! (subject-append \"high\") payload)", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NIL);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.high");
    check_text(pub.payloads[0], "42");
    pb_arena_free(&arena);
}

static void test_thread_and_numeric_helpers(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src(&arena, "(-> payload-float (clamp 0 10) (publish! \"clamped\"))", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "clamped");
    check_text(pub.payloads[0], "10");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(+ (round 1 42.567) (min 3 1 2) (max 3 1 2) (abs -7) (sign -42))", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 52.6) < 0.00001);
    pb_arena_free(&arena);
}

static void test_stateful_helpers(void) {
    pb_eval_state state = {0};
    published pub = {0};

    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload_and_state(&arena, &state, "(squelch payload)", "red", &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "red");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(squelch payload)", "red", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(changed? payload)", "blue", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(deadband 0.5 payload-float)", "10", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(deadband 0.5 payload-float)", "10.2", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(delta payload-float)", "12", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(delta payload-float)", "15", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 3.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(count!)", "15", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.count");
    check_text(pub.payloads[0], "1");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(moving-avg 3 payload-float)", "10", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 10.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(moving-avg 3 payload-float)", "20", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 15.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(moving-avg :ms 5000 payload-float)", "20", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 20.0) < 0.00001);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
}

static void test_now_and_bar(void) {
    pb_eval_state state = {0};
    published pub = {0};

    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload_state_clock(&arena, &state, "(now :date)", "0", 0, 1777903425000, &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "2026-05-04");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! 3 payload-float)", "10", 0, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! 3 payload-float)", "12", 0, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! 3 payload-float)", "9", 0, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 4);
    check_text(pub.subjects[0], "sensors.temp.bar.open");
    check_text(pub.payloads[0], "10");
    check_text(pub.subjects[1], "sensors.temp.bar.high");
    check_text(pub.payloads[1], "12");
    check_text(pub.subjects[2], "sensors.temp.bar.low");
    check_text(pub.payloads[2], "9");
    check_text(pub.subjects[3], "sensors.temp.bar.close");
    check_text(pub.payloads[3], "9");
    pb_arena_free(&arena);

    pub = (published){0};
    pb_eval_state_free(&state);
    state = (pb_eval_state){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! :ms 1000 payload-float)", "10", 1100, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! :ms 1000 payload-float)", "12", 1300, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! :ms 1000 payload-float)", "15", 2100, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 4);
    check_text(pub.payloads[0], "10");
    check_text(pub.payloads[1], "12");
    check_text(pub.payloads[2], "10");
    check_text(pub.payloads[3], "12");
    pb_arena_free(&arena);

    pub = (published){0};
    pb_eval_state_free(&state);
    state = (pb_eval_state){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! :ms 1000 payload-float)", "10", 1100, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && state.len == 1);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(bar! :ms 1000 payload-float)", "12", 1300, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pb_eval_ctx tick_ctx = {
        .arena = &arena,
        .state = &state,
        .now_ms = 2000,
        .subject = {.ptr = "sensors.temp", .len = 12},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick_ctx, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 4);
    check_text(pub.payloads[0], "10");
    check_text(pub.payloads[1], "12");
    check_text(pub.payloads[2], "10");
    check_text(pub.payloads[3], "12");
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
}

static void test_json_get(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src_with_payload(&arena, "(json-get \"reading.temp\" payload)",
                                             "{\"reading\":{\"temp\":31.5,\"ok\":true}}", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 31.5) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload(&arena, "(json-get \"reading.ok\" payload)",
                              "{\"reading\":{\"temp\":31.5,\"ok\":true}}", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload(&arena, "(json-get \"reading.missing\" payload)",
                              "{\"reading\":{\"temp\":31.5}}", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NIL);
    pb_arena_free(&arena);
}

static void test_json_demux(void) {
    pb_arena arena = {0};
    published pub = {0};
    pb_eval_result r = eval_src_with_payload(&arena,
        "(json-demux! [\"reading.temp\" [\"reading.status\" \"state\"]] payload)",
        "{\"reading\":{\"temp\":31.5,\"status\":\"warm\",\"missing\":null}}",
        &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 2);
    check_text(pub.subjects[0], "sensors.temp.temp");
    check_text(pub.payloads[0], "31.5");
    check_text(pub.subjects[1], "sensors.temp.state");
    check_text(pub.payloads[1], "warm");
    pb_arena_free(&arena);
}

int main(void) {
    test_bound_symbols_and_math();
    test_contains_string_and_vector();
    test_if_when_do();
    test_subject_helpers();
    test_publish();
    test_thread_and_numeric_helpers();
    test_stateful_helpers();
    test_now_and_bar();
    test_json_get();
    test_json_demux();
    puts("eval tests passed");
    return 0;
}
