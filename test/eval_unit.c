#include "pb_eval.h"
#include "pb_eval_internal.h"
#include "test_check.h"
#include "test_pb_check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct published {
    pb_slice subjects[8];
    pb_slice payloads[8];
    int count;
} published;

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    published *p = ctx;
    if (p->count < 8) {
        p->subjects[p->count] = subject;
        p->payloads[p->count] = payload;
    }
    p->count += 1;
    return true;
}

static pb_eval_result eval_src_core(pb_arena *arena, pb_eval_state *state, const char *src, const char *payload,
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

static pb_eval_result eval_src_with_payload_state_clock(pb_arena *arena, pb_eval_state *state,
                                                        const char *src, const char *payload,
                                                        uint64_t now_ms, int64_t wall_ms, published *pub) {
    return eval_src_core(arena, state, src, payload, now_ms, wall_ms, pub);
}

static pb_eval_result eval_src_with_payload_and_state(pb_arena *arena, pb_eval_state *state,
                                                      const char *src, const char *payload, published *pub) {
    return eval_src_core(arena, state, src, payload, 0, 0, pub);
}

static pb_eval_result eval_src_with_payload(pb_arena *arena, const char *src, const char *payload, published *pub) {
    pb_eval_state state = {0};
    pb_eval_result r = eval_src_core(arena, &state, src, payload, 0, 0, pub);
    pb_eval_state_free(&state);
    return r;
}

static pb_eval_result eval_src(pb_arena *arena, const char *src, published *pub) {
    pb_eval_state state = {0};
    pb_eval_result r = eval_src_core(arena, &state, src, "42", 0, 0, pub);
    pb_eval_state_free(&state);
    return r;
}

static pb_eval_result eval_src_replaying(pb_arena *arena, const char *src, bool replaying) {
    pb_parse_result parsed = pb_parse_all(arena, src, strlen(src));
    CHECK(parsed.err == PB_PARSE_OK);
    CHECK(parsed.forms.len == 1);
    pb_eval_state state = {0};
    pb_eval_ctx ctx = {
        .arena = arena,
        .state = &state,
        .replaying = replaying,
        .subject = {.ptr = "sensors.temp", .len = 12},
        .payload = {.ptr = "42", .len = 2},
    };
    pb_eval_result r = pb_eval(&ctx, parsed.forms.items[0]);
    pb_eval_state_free(&state);
    return r;
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

static void test_replaying_symbol(void) {
    pb_arena arena = {0};
    pb_eval_result r = eval_src_replaying(&arena, "replaying?", false);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && !r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_replaying(&arena, "replaying?", true);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_BOOL && r.value.boolean);
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

    arena = (pb_arena){0};
    r = eval_src(&arena, "(subject-with \"alerts\" (subject-token 1 \"rack.7\") true 2)", &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "alerts.7.true.2");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src(&arena, "(subject-with \"alerts\" \"bad.*\")", &pub);
    CHECK(r.err == PB_EVAL_INVALID_SUBJECT);
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

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src(&arena, "(publish-to \"alias\" payload)", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "alias");
    check_text(pub.payloads[0], "42");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload(&arena, "(publish! \"rounded\" :dp 2 payload-float)", "42.1234", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "rounded");
    check_text(pub.payloads[0], "42.12");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload(&arena, "(publish! \"rounded\" payload-float :dp 1)", "42.16", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.payloads[0], "42.2");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload(&arena, "(-> payload-float (publish! \"threaded\" :dp 1))", "42.16", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "threaded");
    check_text(pub.payloads[0], "42.2");
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

    pb_eval_state state = {0};
    pub = (published){0};
    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state,
                                        "(-> payload-float (deadband 0.5) (round 1) (publish! \"stable\"))",
                                        "10", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.payloads[0], "10");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state,
                                        "(-> payload-float (deadband 0.5) (round 1) (publish! \"stable\"))",
                                        "10.2", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(r.value.kind == PB_NIL);
    CHECK(pub.count == 1);
    pb_arena_free(&arena);
    pb_eval_state_free(&state);
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
    pub = (published){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(count! :subject \"metrics.temp.total\")", "15", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "metrics.temp.total");
    check_text(pub.payloads[0], "2");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(-> true (count! :subject \"metrics.temp.edges\"))", "15", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "metrics.temp.edges");
    check_text(pub.payloads[0], "3");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(count! false :subject \"metrics.temp.quiet\")", "15", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(count! :subject)", "15", &pub);
    CHECK(r.err == PB_EVAL_ARITY);
    CHECK(pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(count! :subject \"metrics.temp.after-bad\")", "15", &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "metrics.temp.after-bad");
    check_text(pub.payloads[0], "4");
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

static void test_edge_and_holdoff_helpers(void) {
    pb_eval_state state = {0};
    published pub = {0};

    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload_and_state(&arena, &state, "(rising-edge (> payload-float 50))", "40", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(rising-edge (> payload-float 50))", "55", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(rising-edge (> payload-float 50))", "60", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(falling-edge (> payload-float 50))", "60", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(falling-edge (> payload-float 50))", "40", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(hold-off 100 payload)", "a", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "a");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(hold-off 100 payload)", "b", 1050, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(hold-off 100 payload)", "c", 1100, 0, &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "c");
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
}

static void test_windowed_analytics(void) {
    pb_eval_state state = {0};
    published pub = {0};

    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload_and_state(&arena, &state, "(quantize 50 payload-float)", "126", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
    CHECK(fabs(r.value.number - 150.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(moving-sum 3 payload-float)", "10", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 10.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(moving-sum 3 payload-float)", "20", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 30.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(- (moving-max 3 payload-float) (moving-min 3 payload-float))", "5", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(- (moving-max 3 payload-float) (moving-min 3 payload-float))", "9", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 4.0) < 0.00001);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(median 3 payload-float)", "10", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 10.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(median 3 payload-float)", "30", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 10.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(percentile 3 1.0 payload-float)", "20", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 20.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(variance 3 payload-float)", "2", &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state, "(stddev 3 payload-float)", "4", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.number >= 0.0);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
}

static void test_rate_throttle_and_clock_forms(void) {
    pb_eval_state state = {0};
    published pub = {0};

    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload_state_clock(&arena, &state, "(rate :ms 1000 payload)", "x", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 1.0) < 0.00001);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(rate :ms 1000 payload)", "x", 1500, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - 2.0) < 0.00001);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(throttle :ms 1000 1 payload)", "a", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK);
    check_text(r.value.text, "a");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(throttle :ms 1000 1 payload)", "b", 1100, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};
    pub = (published){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(debounce! :ms 100 (subject-append \"settled\") payload)", "42", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && state.len == 1);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pb_eval_ctx tick_ctx = {
        .arena = &arena,
        .state = &state,
        .now_ms = 1100,
        .subject = (pb_slice){.ptr = state.items[0].subject, .len = state.items[0].subject_len},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick_ctx, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.settled");
    check_text(pub.payloads[0], "42");
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};
    pub = (published){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(sample! :ms 100 (subject-append \"sampled\") payload)", "7", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && state.len == 1);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    tick_ctx = (pb_eval_ctx){
        .arena = &arena,
        .state = &state,
        .now_ms = 1100,
        .subject = (pb_slice){.ptr = state.items[0].subject, .len = state.items[0].subject_len},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick_ctx, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.sampled");
    check_text(pub.payloads[0], "7");
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};
    pub = (published){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(aggregate! :ms 100 (subject-append \"avg\") :avg payload-float)", "10", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && state.len == 1);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state, "(aggregate! :ms 100 (subject-append \"avg\") :avg payload-float)", "20", 1050, 0, &pub);
    CHECK(r.err == PB_EVAL_OK);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    tick_ctx = (pb_eval_ctx){
        .arena = &arena,
        .state = &state,
        .now_ms = 1100,
        .subject = (pb_slice){.ptr = state.items[0].subject, .len = state.items[0].subject_len},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick_ctx, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.avg");
    check_text(pub.payloads[0], "15");
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
    state = (pb_eval_state){0};
    pub = (published){0};

    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state,
                                          "(dropout :ms 100 :lost (publish! (subject-append \"stale\") \"true\") :found (publish! (subject-append \"stale\") \"false\"))",
                                          "1", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 0 && state.len == 1);

    pb_arena tick_arena = {0};
    tick_ctx = (pb_eval_ctx){
        .arena = &tick_arena,
        .state = &state,
        .now_ms = 1100,
        .subject = (pb_slice){.ptr = state.items[0].subject, .len = state.items[0].subject_len},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick_ctx, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.stale");
    check_text(pub.payloads[0], "true");
    pb_arena_free(&tick_arena);

    r = eval_src_with_payload_state_clock(&arena, &state,
                                          "(dropout :ms 100 :lost (publish! (subject-append \"stale\") \"true\") :found (publish! (subject-append \"stale\") \"false\"))",
                                          "1", 1200, 0, &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 2);
    check_text(pub.subjects[1], "sensors.temp.stale");
    check_text(pub.payloads[1], "false");
    pb_arena_free(&arena);

    pb_eval_state_free(&state);
}

static void test_transition_helper(void) {
    pb_eval_state state = {0};
    published pub = {0};

    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload_and_state(&arena, &state,
                                                       "(transition (> payload-float 50) (publish! \"hot\" payload) (publish! \"cool\" payload))",
                                                       "40", &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL && pub.count == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state,
                                        "(transition (> payload-float 50) (publish! \"hot\" payload) (publish! \"cool\" payload))",
                                        "60", &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "hot");
    check_text(pub.payloads[0], "60");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state,
                                        "(transition (> payload-float 50) (publish! \"hot\" payload) (publish! \"cool\" payload))",
                                        "70", &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload_and_state(&arena, &state,
                                        "(transition (> payload-float 50) (publish! \"hot\" payload) (publish! \"cool\" payload))",
                                        "45", &pub);
    CHECK(r.err == PB_EVAL_OK && pub.count == 2);
    check_text(pub.subjects[1], "cool");
    check_text(pub.payloads[1], "45");
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

    state = (pb_eval_state){0};
    pub = (published){0};
    arena = (pb_arena){0};
    r = eval_src_with_payload_state_clock(&arena, &state,
                                          "(on-silence :ms 100 (publish! (subject-append \"quiet\") payload))",
                                          "last", 1000, 0, &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 0);

    tick_ctx = (pb_eval_ctx){
        .arena = &arena,
        .state = &state,
        .now_ms = 1100,
        .subject = {.ptr = "sensors.temp", .len = 12},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick_ctx, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.quiet");
    check_text(pub.payloads[0], "last");
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
    check_text(pub.subjects[0], "sensors.temp.reading.temp");
    check_text(pub.payloads[0], "31.5");
    check_text(pub.subjects[1], "sensors.temp.state");
    check_text(pub.payloads[1], "warm");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload(&arena,
                              "(json-demux! :leaf \"reading.temp\" [\"reading.status\" \"state\"] payload)",
                              "{\"reading\":{\"temp\":31.5,\"status\":\"warm\"}}",
                              &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 2);
    check_text(pub.subjects[0], "sensors.temp.temp");
    check_text(pub.payloads[0], "31.5");
    check_text(pub.subjects[1], "sensors.temp.state");
    check_text(pub.payloads[1], "warm");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload(&arena,
                              "(json-demux \"temp\" payload)",
                              "{\"temp\":12.5}",
                              &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "sensors.temp.temp");
    check_text(pub.payloads[0], "12.5");
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    pub = (published){0};
    r = eval_src_with_payload(&arena,
                              "(json-demux! [\"sensor.thermal.temp\" \"temp\"] "
                              "[\"sensor.atmo.pressure\" \"pressure\"] "
                              "[\"sensor.atmo.dust\" \"dust\"] "
                              "[\"sensor.prospect.ice\" \"ice\"] payload)",
                              "{\"sensor\":{\"thermal\":{\"temp\":-142.1},"
                              "\"atmo\":{\"pressure\":69.4,\"dust\":0.12},"
                              "\"prospect\":{\"ice\":41.2}}}",
                              &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(pub.count == 4);
    check_text(pub.subjects[0], "sensors.temp.temp");
    check_text(pub.payloads[0], "-142.1");
    check_text(pub.subjects[1], "sensors.temp.pressure");
    check_text(pub.payloads[1], "69.4");
    check_text(pub.subjects[2], "sensors.temp.dust");
    check_text(pub.payloads[2], "0.12");
    check_text(pub.subjects[3], "sensors.temp.ice");
    check_text(pub.payloads[3], "41.2");
    pb_arena_free(&arena);
}

// Evaluate a resolved expression against shared state; returns the publish count.
static int eval_resolved_publishes(pb_arena *arena, pb_eval_state *state, pb_value expr, const char *payload) {
    published pub = {0};
    pb_eval_ctx ctx = {
        .arena = arena,
        .state = state,
        .subject = {.ptr = "t.x", .len = 3},
        .payload = {.ptr = payload, .len = strlen(payload)},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    pb_eval_result r = pb_eval(&ctx, expr);
    CHECK(r.err == PB_EVAL_OK);
    return pub.count;
}

static pb_value parse_resolved(pb_arena *arena, const char *src, char *err, size_t err_cap, bool *resolved) {
    pb_parse_result parsed = pb_parse_all(arena, src, strlen(src));
    CHECK(parsed.err == PB_PARSE_OK);
    CHECK(parsed.forms.len == 1);
    *resolved = pb_resolve_rule(arena, &parsed.forms.items[0], false, false, err, err_cap);
    return parsed.forms.items[0];
}

static void test_call_site_state_identity(void) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    // Two squelch calls in one rule must not share a slot.
    pb_value expr = parse_resolved(&arena,
                                   "(do (publish! \"out.a\" (squelch payload))"
                                   "    (publish! \"out.b\" (squelch (str-concat payload \"!\"))))",
                                   err, sizeof err, &resolved);
    CHECK(resolved);
    CHECK(eval_resolved_publishes(&arena, &state, expr, "1") == 2);
    CHECK(eval_resolved_publishes(&arena, &state, expr, "1") == 0);
    CHECK(eval_resolved_publishes(&arena, &state, expr, "2") == 2);
    CHECK(state.len == 2);
    CHECK(state.items[0].site != state.items[1].site);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

static void test_threaded_steps_keep_call_site(void) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    // The bare symbol step `squelch` becomes a call with its own site; the call
    // rewritten at eval time must keep it so the slot is stable across publishes.
    pb_value expr = parse_resolved(&arena, "(-> payload squelch (publish! \"out.t\"))", err, sizeof err, &resolved);
    CHECK(resolved);
    CHECK(eval_resolved_publishes(&arena, &state, expr, "7") == 1);
    CHECK(eval_resolved_publishes(&arena, &state, expr, "7") == 0);
    CHECK(eval_resolved_publishes(&arena, &state, expr, "8") == 1);
    CHECK(state.len == 1);
    CHECK(state.items[0].site != 0);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

static void test_resolve_rejects_unknown_forms_in_any_branch(void) {
    pb_arena arena = {0};
    char err[160];
    bool resolved = true;
    parse_resolved(&arena, "(when (> payload-float 5) (bogus 1 2))", err, sizeof err, &resolved);
    CHECK(!resolved);
    CHECK(strstr(err, "unknown form: bogus") != NULL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    parse_resolved(&arena, "(publish! \"x\" nonsense)", err, sizeof err, &resolved);
    CHECK(!resolved);
    CHECK(strstr(err, "unknown symbol: nonsense") != NULL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    parse_resolved(&arena, "(-> payload nonsense-step)", err, sizeof err, &resolved);
    CHECK(!resolved);
    pb_arena_free(&arena);
}

static void test_resolve_rejects_threaded_on_silence(void) {
    pb_arena arena = {0};
    char err[160];
    bool resolved = true;
    parse_resolved(&arena, "(-> payload (on-silence :ms 50 (publish! \"s\" \"gone\")))", err, sizeof err, &resolved);
    CHECK(!resolved);
    CHECK(strstr(err, "cannot be a -> step") != NULL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    parse_resolved(&arena, "(-> payload (dropout :ms 50 :lost (publish! \"l\" 1) :found (publish! \"f\" 1)))", err, sizeof err, &resolved);
    CHECK(!resolved);
    pb_arena_free(&arena);

    // Even unresolved, eval refuses to thread into on-silence: its body would
    // point into scratch memory that the timer outlives.
    arena = (pb_arena){0};
    pb_eval_state state = {0};
    published pub = {0};
    pb_eval_result r = eval_src_with_payload_and_state(&arena, &state, "(-> payload (on-silence :ms 50 (publish! \"s\" \"gone\")))", "1", &pub);
    CHECK(r.err == PB_EVAL_TYPE);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

static void test_resolve_allows_user_hooks_to_own_unknowns(void) {
    pb_arena arena = {0};
    const char *src = "(custom 1 other)";
    pb_parse_result parsed = pb_parse_all(&arena, src, strlen(src));
    CHECK(parsed.err == PB_PARSE_OK);
    char err[160];
    CHECK(pb_resolve_rule(&arena, &parsed.forms.items[0], true, true, err, sizeof err));
    pb_arena_free(&arena);
}

// One resolved evaluation with a chosen subject, payload, clock, and last hook.
typedef struct eval_at_args {
    const char *subject;
    const char *payload;
    uint64_t now_ms;
    pb_last_fn last;
    void *last_ctx;
    const char *reply_to; // NULL means no reply subject
} eval_at_args;

static pb_eval_result eval_at(pb_arena *arena, pb_eval_state *state, pb_value expr, eval_at_args a, published *pub) {
    pb_eval_ctx ctx = {
        .arena = arena,
        .state = state,
        .now_ms = a.now_ms,
        .subject = {.ptr = a.subject, .len = strlen(a.subject)},
        .payload = {.ptr = a.payload, .len = strlen(a.payload)},
        .reply_to = a.reply_to != NULL ? (pb_slice){.ptr = a.reply_to, .len = strlen(a.reply_to)} : (pb_slice){0},
        .publish = publish_cb,
        .publish_ctx = pub,
        .last = a.last,
        .last_ctx = a.last_ctx,
    };
    return pb_eval(&ctx, expr);
}

// Parses and resolves `src`, then evaluates it once per payload and checks the bool results.
static void check_bool_sequence(const char *src, const char *const *payloads, const bool *want, size_t n) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena, src, err, sizeof err, &resolved);
    CHECK(resolved);
    for (size_t i = 0; i < n; i += 1) {
        published pub = {0};
        pb_eval_result r = eval_at(&arena, &state, expr, (eval_at_args){.subject = "t.x", .payload = payloads[i]}, &pub);
        CHECK(r.err == PB_EVAL_OK);
        CHECK(r.value.kind == PB_BOOL && r.value.boolean == want[i]);
    }
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

static pb_eval_error eval_error_of(const char *src, const char *payload) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena, src, err, sizeof err, &resolved);
    CHECK(resolved);
    published pub = {0};
    pb_eval_result r = eval_at(&arena, &state, expr, (eval_at_args){.subject = "t.x", .payload = payload}, &pub);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
    return r.err;
}

static void test_hysteresis(void) {
    const char *in[] = {"22", "26", "23", "20", "22", "25", "21", "19"};
    const bool want[] = {false, true, true, false, false, true, true, false};
    check_bool_sequence("(hysteresis 20 25 payload-float)", in, want, 8);

    // First sight at or above HI is true; at or below LO is false.
    const char *hi[] = {"25"};
    const bool hi_want[] = {true};
    check_bool_sequence("(hysteresis 20 25 payload-float)", hi, hi_want, 1);
    const char *lo[] = {"20"};
    const bool lo_want[] = {false};
    check_bool_sequence("(hysteresis 20 25 payload-float)", lo, lo_want, 1);

    // Composes with rising-edge.
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena, "(rising-edge (hysteresis 20 25 payload-float))", err, sizeof err, &resolved);
    CHECK(resolved);
    const char *seq[] = {"10", "30", "22", "30", "15", "30"};
    const bool fired[] = {false, true, false, false, false, true};
    for (size_t i = 0; i < 6; i += 1) {
        published pub = {0};
        pb_eval_result r = eval_at(&arena, &state, expr, (eval_at_args){.subject = "t.x", .payload = seq[i]}, &pub);
        CHECK(r.err == PB_EVAL_OK);
        CHECK((r.value.kind == PB_BOOL) == fired[i]);
    }
    CHECK(state.len == 2);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);

    CHECK(eval_error_of("(hysteresis 25 20 payload-float)", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(hysteresis 20 25 \"abc\")", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(hysteresis 20 25)", "1") == PB_EVAL_ARITY);
    CHECK(eval_error_of("(hysteresis 20 20 payload-float)", "20") == PB_EVAL_OK);
}

static void test_ewma(void) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena, "(ewma 0.5 payload-float)", err, sizeof err, &resolved);
    CHECK(resolved);
    const char *in[] = {"10", "20", "10"};
    const double want[] = {10.0, 15.0, 12.5};
    for (size_t i = 0; i < 3; i += 1) {
        published pub = {0};
        pb_eval_result r = eval_at(&arena, &state, expr, (eval_at_args){.subject = "t.x", .payload = in[i]}, &pub);
        CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NUMBER);
        CHECK(fabs(r.value.number - want[i]) < 1e-9);
    }
    CHECK(state.len == 1);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);

    // ALPHA 1 follows the input.
    arena = (pb_arena){0};
    state = (pb_eval_state){0};
    expr = parse_resolved(&arena, "(ewma 1 payload-float)", err, sizeof err, &resolved);
    CHECK(resolved);
    for (size_t i = 0; i < 3; i += 1) {
        published pub = {0};
        pb_eval_result r = eval_at(&arena, &state, expr, (eval_at_args){.subject = "t.x", .payload = in[i]}, &pub);
        CHECK(r.err == PB_EVAL_OK && fabs(r.value.number - atof(in[i])) < 1e-9);
    }
    pb_eval_state_free(&state);
    pb_arena_free(&arena);

    CHECK(eval_error_of("(ewma 0 payload-float)", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(ewma -0.5 payload-float)", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(ewma 1.5 payload-float)", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(ewma \"x\" payload-float)", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(ewma 0.5 \"nan\")", "1") == PB_EVAL_TYPE);
    CHECK(eval_error_of("(ewma 0.5)", "1") == PB_EVAL_ARITY);
}

// keyed was removed; it is an ordinary unknown form now.
static void test_keyed_is_unknown(void) {
    pb_arena arena = {0};
    char err[160];
    bool resolved = true;
    parse_resolved(&arena, "(keyed \"k\" 1)", err, sizeof err, &resolved);
    CHECK(!resolved);
    CHECK(strstr(err, "unknown form: keyed") != NULL);
    pb_arena_free(&arena);
}

// A clock slot remembers the subject that armed it, so the tick-time body sees
// the same `subject` a call-time body would.
static void test_clock_tick_binds_slot_subject(void) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena,
                                   "(on-silence :ms 50 (publish! (subject-append \"quiet\") \"gone\"))",
                                   err, sizeof err, &resolved);
    CHECK(resolved);
    published pub = {0};
    pb_eval_result r = eval_at(&arena, &state, expr, (eval_at_args){.subject = "temp.a", .payload = "1", .now_ms = 1000}, &pub);
    CHECK(r.err == PB_EVAL_OK);
    CHECK(state.len == 1);

    pb_arena tick_arena = {0};
    pb_eval_ctx tick = {
        .arena = &tick_arena,
        .state = &state,
        .now_ms = 1100,
        .subject = (pb_slice){.ptr = state.items[0].subject, .len = state.items[0].subject_len},
        .payload = {.ptr = "", .len = 0},
        .publish = publish_cb,
        .publish_ctx = &pub,
    };
    r = pb_eval_tick_state_entry(&tick, &state.items[0]);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1);
    check_text(pub.subjects[0], "temp.a.quiet");
    pb_arena_free(&tick_arena);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

// Fake last-value source: one row, payload kept in a mutable buffer so tests
// can prove the evaluator copied it.
typedef struct fake_last {
    char payload[32];
} fake_last;

static bool fake_last_cb(void *ctx, pb_slice subject, pb_slice *payload) {
    fake_last *f = ctx;
    if (!pb_slice_eq_lit(subject, "temp.outside")) {
        return false;
    }
    *payload = (pb_slice){.ptr = f->payload, .len = strlen(f->payload)};
    return true;
}

static pb_eval_result eval_with_last(pb_arena *arena, const char *src, pb_last_fn last, void *last_ctx) {
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(arena, src, err, sizeof err, &resolved);
    CHECK(resolved);
    published pub = {0};
    pb_eval_result r = eval_at(arena, &state, expr,
                               (eval_at_args){.subject = "t.x", .payload = "1", .last = last, .last_ctx = last_ctx}, &pub);
    pb_eval_state_free(&state);
    return r;
}

static void test_last(void) {
    fake_last f = {.payload = "31.5"};
    pb_arena arena = {0};
    pb_eval_result r = eval_with_last(&arena, "(last \"temp.outside\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_STRING);
    check_text(r.value.text, "31.5");
    // The result is an arena copy, not a view of the source buffer.
    memcpy(f.payload, "99.9", 5);
    check_text(r.value.text, "31.5");
    CHECK(r.value.text.ptr != f.payload);
    pb_arena_free(&arena);

    // Numeric coercion makes comparisons work on the string result.
    arena = (pb_arena){0};
    f = (fake_last){.payload = "31.5"};
    r = eval_with_last(&arena, "(> (last \"temp.outside\") 30)", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(> (- (last \"temp.outside\") 1.5) 30)", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && !r.value.boolean);
    pb_arena_free(&arena);

    // Unknown subject and missing hook are nil, not errors.
    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last \"temp.inside\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last \"temp.outside\")", NULL, NULL);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);
    pb_arena_free(&arena);

    // An empty stored payload is an empty string, not nil.
    arena = (pb_arena){0};
    f = (fake_last){.payload = ""};
    r = eval_with_last(&arena, "(last \"temp.outside\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_STRING && r.value.text.len == 0);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last \"temp.*\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_INVALID_SUBJECT);
    pb_arena_free(&arena);
    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last \"temp.>\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_INVALID_SUBJECT);
    pb_arena_free(&arena);
    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last \"\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_INVALID_SUBJECT);
    pb_arena_free(&arena);
    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last 5)", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_TYPE);
    pb_arena_free(&arena);
    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last)", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_ARITY);
    pb_arena_free(&arena);
    arena = (pb_arena){0};
    r = eval_with_last(&arena, "(last \"a\" \"b\")", fake_last_cb, &f);
    CHECK(r.err == PB_EVAL_ARITY);
    pb_arena_free(&arena);
}

// reply-to is the reply subject or nil; reply! publishes to it and is a counted no-op without one.
static void test_reply_to_and_reply(void) {
    pb_arena arena = {0};
    char err[160];
    bool resolved = false;
    pb_value sym = parse_resolved(&arena, "reply-to", err, sizeof err, &resolved);
    CHECK(resolved);
    pb_eval_state state = {0};
    published pub = {0};
    pb_eval_result r = eval_at(&arena, &state, sym, (eval_at_args){.subject = "q", .payload = "", .reply_to = "_INBOX.7"}, &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_STRING);
    check_text(r.value.text, "_INBOX.7");
    r = eval_at(&arena, &state, sym, (eval_at_args){.subject = "q", .payload = ""}, &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL);

    pb_value reply = parse_resolved(&arena, "(reply! (str-concat \"got \" payload))", err, sizeof err, &resolved);
    CHECK(resolved);
    r = eval_at(&arena, &state, reply, (eval_at_args){.subject = "q", .payload = "5", .reply_to = "_INBOX.7"}, &pub);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_NIL && pub.count == 1);
    check_text(pub.subjects[0], "_INBOX.7");
    check_text(pub.payloads[0], "got 5");

    // No reply subject: silent no-op, counted as suppressed.
    uint64_t suppressed = 0;
    pb_eval_ctx ctx = {.arena = &arena, .state = &state, .subject = {.ptr = "q", .len = 1}, .publish = publish_cb,
                       .publish_ctx = &pub, .publishes_suppressed = &suppressed};
    r = pb_eval(&ctx, reply);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1 && suppressed == 1);

    // A nil value is also a counted no-op.
    pb_value nil_reply = parse_resolved(&arena, "(reply! (last \"nothing.here\"))", err, sizeof err, &resolved);
    CHECK(resolved);
    ctx.reply_to = (pb_slice){.ptr = "_INBOX.7", .len = 8};
    r = pb_eval(&ctx, nil_reply);
    CHECK(r.err == PB_EVAL_OK && pub.count == 1 && suppressed == 2);

    // :dp formatting, direct and threaded.
    pb_value dp = parse_resolved(&arena, "(reply! :dp 1 payload-float)", err, sizeof err, &resolved);
    CHECK(resolved);
    ctx.payload = (pb_slice){.ptr = "21.37", .len = 5};
    r = pb_eval(&ctx, dp);
    CHECK(r.err == PB_EVAL_OK && pub.count == 2);
    check_text(pub.payloads[1], "21.4");
    pb_value threaded = parse_resolved(&arena, "(-> payload-float (round 1) (reply!))", err, sizeof err, &resolved);
    CHECK(resolved);
    r = pb_eval(&ctx, threaded);
    CHECK(r.err == PB_EVAL_OK && pub.count == 3);
    check_text(pub.payloads[2], "21.4");

    // Arity, and reply subjects go through the same validation as publish!.
    pb_value arity = parse_resolved(&arena, "(reply!)", err, sizeof err, &resolved);
    CHECK(resolved);
    CHECK(pb_eval(&ctx, arity).err == PB_EVAL_ARITY);
    pb_value plain = parse_resolved(&arena, "(reply! \"x\")", err, sizeof err, &resolved);
    CHECK(resolved);
    ctx.reply_to = (pb_slice){.ptr = "$LVC.a", .len = 6};
    CHECK(pb_eval(&ctx, plain).err == PB_EVAL_INVALID_SUBJECT);
    ctx.reply_to = (pb_slice){.ptr = "$STATS.a", .len = 8};
    CHECK(pb_eval(&ctx, plain).err == PB_EVAL_INVALID_SUBJECT);
    ctx.reply_to = (pb_slice){.ptr = "a.*", .len = 3};
    CHECK(pb_eval(&ctx, plain).err == PB_EVAL_INVALID_SUBJECT);
    CHECK(pub.count == 3);

    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

// A slot created under an every body has the empty subject as identity, so a
// clock body armed inside it binds the empty subject at tick time.
static void test_slot_under_every_has_empty_subject(void) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena,
                                   "(on-silence :ms 50 (publish! (str-concat \"s\" subject \"-x\") \"gone\"))",
                                   err, sizeof err, &resolved);
    CHECK(resolved);
    published pub = {0};
    pb_eval_ctx ctx = {.arena = &arena, .state = &state, .now_ms = 1000,
                       .publish = publish_cb, .publish_ctx = &pub};
    CHECK(pb_eval(&ctx, expr).err == PB_EVAL_OK);
    CHECK(state.len == 1);
    pb_slice subject = (pb_slice){.ptr = state.items[0].subject, .len = state.items[0].subject_len};
    CHECK(subject.len == 0);

    pb_arena tick_arena = {0};
    pb_eval_ctx tick = {.arena = &tick_arena, .state = &state, .now_ms = 1100, .subject = subject,
                        .publish = publish_cb, .publish_ctx = &pub};
    CHECK(pb_eval_tick_state_entry(&tick, &state.items[0]).err == PB_EVAL_OK);
    CHECK(pub.count == 1);
    check_text(pub.subjects[0], "s-x");
    pb_arena_free(&tick_arena);
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
}

static void test_payload_bool(void) {
    const char *truthy_payloads[] = {"true", "TRUE", "True", "1", "on", "ON", "oN"};
    const char *falsy_payloads[] = {"false", "FALSE", "False", "0", "off", "Off"};
    const char *bad_payloads[] = {"", " true", "true ", "yes", "no", "2", "tru", "onn", "-1"};
    for (size_t i = 0; i < sizeof truthy_payloads / sizeof truthy_payloads[0]; i += 1) {
        pb_arena arena = {0};
        pb_eval_result r = eval_src_with_payload(&arena, "payload-bool", truthy_payloads[i], NULL);
        CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && r.value.boolean);
        pb_arena_free(&arena);
    }
    for (size_t i = 0; i < sizeof falsy_payloads / sizeof falsy_payloads[0]; i += 1) {
        pb_arena arena = {0};
        pb_eval_result r = eval_src_with_payload(&arena, "payload-bool", falsy_payloads[i], NULL);
        CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && !r.value.boolean);
        pb_arena_free(&arena);
    }
    for (size_t i = 0; i < sizeof bad_payloads / sizeof bad_payloads[0]; i += 1) {
        pb_arena arena = {0};
        pb_eval_result r = eval_src_with_payload(&arena, "payload-bool", bad_payloads[i], NULL);
        CHECK(r.err == PB_EVAL_TYPE);
        pb_arena_free(&arena);
    }
}

static void test_empty_payload_is_not_a_number(void) {
    pb_arena arena = {0};
    pb_eval_result r = eval_src_with_payload(&arena, "(+ payload-float 1)", "", NULL);
    CHECK(r.err == PB_EVAL_TYPE);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload(&arena, "payload-int", "", NULL);
    CHECK(r.err == PB_EVAL_TYPE);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = eval_src_with_payload(&arena, "(+ 1 \"\")", "5", NULL);
    CHECK(r.err == PB_EVAL_TYPE);
    pb_arena_free(&arena);

    // The empty payload is still a usable string.
    arena = (pb_arena){0};
    r = eval_src_with_payload(&arena, "(= payload \"\")", "", NULL);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_BOOL && r.value.boolean);
    pb_arena_free(&arena);
}

// Regex forms. Results are copied out of the arena so checks outlive it.
typedef struct rx_result {
    pb_eval_error err;
    pb_value_kind kind;
    bool boolean;
    char text[128];
    size_t len;
} rx_result;

// Resolves `src` (regex literals compile here) and evaluates it against `payload` of `payload_len` bytes.
static rx_result rx_eval(const char *src, const char *payload, size_t payload_len) {
    pb_arena arena = {0};
    pb_eval_state state = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena, src, err, sizeof err, &resolved);
    CHECK(resolved);
    pb_eval_ctx ctx = {
        .arena = &arena,
        .state = &state,
        .subject = {.ptr = "t.x", .len = 3},
        .payload = {.ptr = payload, .len = payload_len},
    };
    pb_eval_result r = pb_eval(&ctx, expr);
    rx_result out = {.err = r.err};
    if (r.err == PB_EVAL_OK) {
        out.kind = r.value.kind;
        out.boolean = r.value.kind == PB_BOOL && r.value.boolean;
        if (r.value.kind == PB_STRING) {
            CHECK(r.value.text.len < sizeof out.text);
            out.len = r.value.text.len;
            memcpy(out.text, r.value.text.ptr, r.value.text.len);
        }
    }
    pb_eval_state_free(&state);
    pb_arena_free(&arena);
    return out;
}

static bool rx_is_string(const char *src, const char *payload, const char *want) {
    rx_result r = rx_eval(src, payload, strlen(payload));
    return r.err == PB_EVAL_OK && r.kind == PB_STRING && r.len == strlen(want) && memcmp(r.text, want, r.len) == 0;
}

static bool rx_is_nil(const char *src, const char *payload) {
    rx_result r = rx_eval(src, payload, strlen(payload));
    return r.err == PB_EVAL_OK && r.kind == PB_NIL;
}

static bool rx_is_bool(const char *src, const char *payload, bool want) {
    rx_result r = rx_eval(src, payload, strlen(payload));
    return r.err == PB_EVAL_OK && r.kind == PB_BOOL && r.boolean == want;
}

static void test_regex_matches(void) {
    CHECK(rx_is_bool("(matches? \"[0-9]+\" payload)", "abc 42", true));
    CHECK(rx_is_bool("(matches? \"[0-9]+\" payload)", "abc", false));
    CHECK(rx_is_bool("(matches? \"^[0-9]+$\" payload)", "42x", false));
    CHECK(rx_is_bool("(-> payload (matches? \"^err\"))", "error: x", true));
    CHECK(rx_is_bool("(-> payload (matches? \"^err\"))", "ok", false));
    // Text coercion matches contains?: bool is "true"/"false", numbers are a type error.
    CHECK(rx_is_bool("(matches? \"^tru\" (= 1 1))", "x", true));
    CHECK(rx_eval("(matches? \"1\" 1)", "x", 1).err == PB_EVAL_TYPE);
    rx_result r = rx_eval("(matches? \"b\\\\x00c\" payload)", "ab\0cd", 5);
    CHECK(r.err == PB_EVAL_OK && r.boolean);
    r = rx_eval("(matches? \"\\\\xff\\\\xfe\" payload)", "a\xff\xfe", 3);
    CHECK(r.err == PB_EVAL_OK && r.boolean);
}

static void test_regex_capture(void) {
    CHECK(rx_is_string("(re-capture \"t=([0-9]+)\" payload)", "id=7 t=215 z", "215"));
    CHECK(rx_is_string("(re-capture \"[0-9]+\" payload)", "id=7 t=215", "7"));
    CHECK(rx_is_string("(-> payload (re-capture \"t=(\\\\d+)\"))", "t=9", "9"));
    CHECK(rx_is_nil("(re-capture \"t=([0-9]+)\" payload)", "nope"));
    CHECK(rx_is_nil("(-> payload (re-capture \"t=([0-9]+)\") (publish! \"x\"))", "nope"));
    // Group 1 did not participate.
    CHECK(rx_is_nil("(re-capture \"(a)|b\" payload)", "b"));
    CHECK(rx_is_string("(re-capture \"(a)|b\" payload)", "a", "a"));
    // Empty group 1 is an empty string, not nil.
    CHECK(rx_is_string("(re-capture \"x(a*)\" payload)", "x", ""));
    CHECK(rx_is_string("(re-capture \"(a)(b)\" payload)", "ab", "a"));
    rx_result r = rx_eval("(re-capture \"b(.)d\" payload)", "ab\0d", 4);
    CHECK(r.err == PB_EVAL_OK && r.kind == PB_STRING && r.len == 1 && r.text[0] == '\0');
    CHECK(rx_eval("(re-capture \"1\" 1)", "x", 1).err == PB_EVAL_TYPE);
}

static void test_regex_replace(void) {
    CHECK(rx_is_string("(re-replace \"[0-9]+\" \"#\" payload)", "no digits", "no digits"));
    CHECK(rx_is_string("(re-replace \"[0-9]+\" \"#\" payload)", "a1b", "a#b"));
    CHECK(rx_is_string("(re-replace \"[0-9]+\" \"#\" payload)", "a1b22c333", "a#b#c#"));
    CHECK(rx_is_string("(re-replace \"a\" \"-\" payload)", "aaa", "---"));
    CHECK(rx_is_string("(re-replace \"[0-9]+\" \"\" payload)", "a1b22c", "abc"));
    CHECK(rx_is_string("(re-replace \"[0-9]+\" \"\" payload)", "123", ""));
    CHECK(rx_is_string("(re-replace \"a\" \"longer\" payload)", "banana", "blongernlongernlonger"));
    CHECK(rx_is_string("(re-replace \"abc\" \"x\" payload)", "abcabc", "xx"));
    CHECK(rx_is_string("(re-replace \"(a)\" \"$1$\" payload)", "bab", "b$1$b"));
    // Empty matches follow Go's ReplaceAll.
    CHECK(rx_is_string("(re-replace \"x*\" \"-\" payload)", "abc", "-a-b-c-"));
    CHECK(rx_is_string("(re-replace \"x*\" \"-\" payload)", "", "-"));
    CHECK(rx_is_string("(re-replace \"b*\" \"-\" payload)", "abc", "-a-c-"));
    CHECK(rx_is_string("(-> payload (re-replace \"[ ]+\" \"_\"))", "a  b c", "a_b_c"));
    rx_result r = rx_eval("(re-replace \"b\" \"X\" payload)", "a\0b\xff", 4);
    CHECK(r.err == PB_EVAL_OK && r.len == 4 && memcmp(r.text, "a\0X\xff", 4) == 0);
    // Many matches grow the span array past its first allocation.
    r = rx_eval("(re-replace \"a\" \"bb\" payload)", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32);
    CHECK(r.err == PB_EVAL_OK && r.len == 64 && r.text[0] == 'b' && r.text[63] == 'b');
    CHECK(rx_eval("(re-replace \"a\" 1 payload)", "a", 1).err == PB_EVAL_TYPE);
    CHECK(rx_eval("(re-replace \"a\" \"b\" 1)", "a", 1).err == PB_EVAL_TYPE);
}

// Wall-clock bound for the budget tests; sanitizer builds run several times slower.
#if MB_TEST_ASAN
#define PERF_LIMIT_SECONDS 20.0
#else
#define PERF_LIMIT_SECONDS 2.0
#endif

// Work budget: a quadratic replace-all and a huge-program scan fail the rule instead of stalling.
static void test_regex_budget(void) {
    enum { RUN = 64 * 1024, MB = 1024 * 1024 };
    static char buf[MB];
    memset(buf, 'a', RUN);
    clock_t start = clock();
    CHECK(rx_eval("(re-replace \"a+b|a\" \"x\" payload)", buf, RUN).err == PB_EVAL_REGEX_BUDGET);
    memset(buf, '7', RUN);
    CHECK(rx_eval("(re-replace \"\\\\d+%|\\\\d\" \"x\" payload)", buf, RUN).err == PB_EVAL_REGEX_BUDGET);
    memset(buf, 'a', MB);
    CHECK(rx_eval("(matches? \"(?:a|b)*a(?:a|b){100}c\" payload)", buf, MB).err == PB_EVAL_REGEX_BUDGET);
    CHECK(rx_eval("(re-capture \"(?:a|b)*a(?:a|b){100}c\" payload)", buf, MB).err == PB_EVAL_REGEX_BUDGET);
    CHECK((double)(clock() - start) / CLOCKS_PER_SEC < PERF_LIMIT_SECONDS);
    CHECK(strcmp(pb_eval_error_name(PB_EVAL_REGEX_BUDGET), "regex work budget exceeded") == 0);
    memset(buf, '7', 100);
    rx_result r = rx_eval("(re-replace \"\\\\d\" \"x\" payload)", buf, 100);
    CHECK(r.err == PB_EVAL_OK && r.len == 100);
}

static void test_regex_arity_and_types(void) {
    CHECK(rx_eval("(matches? \"a\")", "a", 1).err == PB_EVAL_ARITY);
    CHECK(rx_eval("(matches? \"a\" payload payload)", "a", 1).err == PB_EVAL_ARITY);
    CHECK(rx_eval("(re-capture \"a\")", "a", 1).err == PB_EVAL_ARITY);
    CHECK(rx_eval("(re-replace \"a\" payload)", "a", 1).err == PB_EVAL_ARITY);
    CHECK(rx_eval("(re-replace \"a\" \"b\" payload payload)", "a", 1).err == PB_EVAL_ARITY);

    // An unresolved AST has a string where PB_REGEX should be: type error, no crash.
    const char *unresolved[] = {"(matches? \"a\" payload)", "(re-capture \"a\" payload)", "(re-replace \"a\" \"b\" payload)"};
    for (size_t i = 0; i < 3; i += 1) {
        pb_arena arena = {0};
        published pub = {0};
        CHECK(eval_src_with_payload(&arena, unresolved[i], "a", &pub).err == PB_EVAL_TYPE);
        pb_arena_free(&arena);
    }
}

static void test_regex_resolve_errors(void) {
    const char *bad[] = {
        "(matches? payload payload)",                  // symbol pattern
        "(matches? (str-concat \"a\" \"b\") payload)", // computed pattern
        "(re-replace payload \"x\" payload)",
        "(-> payload (matches? payload))", // threaded form, non-literal
        "(-> payload matches?)",           // bare step has no pattern
        "(matches?)",
        "(matches? 5 payload)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i += 1) {
        pb_arena arena = {0};
        char err[160];
        bool resolved = true;
        parse_resolved(&arena, bad[i], err, sizeof err, &resolved);
        CHECK(!resolved);
        CHECK(strstr(err, "pattern must be a string literal") != NULL);
        pb_arena_free(&arena);
    }

    pb_arena arena = {0};
    char err[160];
    bool resolved = true;
    parse_resolved(&arena, "(re-capture \"ab(cd\" payload)", err, sizeof err, &resolved);
    CHECK(!resolved);
    CHECK(strstr(err, "re-capture: bad pattern") != NULL);
    CHECK(strstr(err, "at byte") != NULL);
    CHECK(strstr(err, "\"ab(cd\"") != NULL);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    parse_resolved(&arena, "(matches? \"(a)\\\\1\" payload)", err, sizeof err, &resolved);
    CHECK(!resolved);
    pb_arena_free(&arena);

    // Resolving the same tree twice leaves the compiled node alone.
    arena = (pb_arena){0};
    const char *src = "(matches? \"a+\" payload)";
    pb_parse_result parsed = pb_parse_all(&arena, src, strlen(src));
    CHECK(parsed.err == PB_PARSE_OK);
    CHECK(pb_resolve_rule(&arena, &parsed.forms.items[0], false, false, err, sizeof err));
    CHECK(parsed.forms.items[0].seq.items[1].kind == PB_REGEX);
    struct pb_regex *first = parsed.forms.items[0].seq.items[1].regex;
    CHECK(pb_resolve_rule(&arena, &parsed.forms.items[0], false, false, err, sizeof err));
    CHECK(parsed.forms.items[0].seq.items[1].regex == first);
    pb_arena_free(&arena);
}

static void test_regex_value_is_not_a_string(void) {
    // A PB_REGEX value never coerces to text or a payload, is truthy, and equals only itself.
    pb_arena arena = {0};
    char err[160];
    bool resolved = false;
    pb_value expr = parse_resolved(&arena, "(matches? \"a\" payload)", err, sizeof err, &resolved);
    CHECK(resolved);
    pb_value re = expr.seq.items[1];
    CHECK(re.kind == PB_REGEX);
    pb_slice s = {0};
    CHECK(!pb_eval_as_string(re, &s));
    double n = 0;
    CHECK(!pb_eval_as_number(re, &n));
    pb_eval_ctx ctx = {.arena = &arena};
    CHECK(!pb_eval_coerce_payload(&ctx, re, &s));
    CHECK(pb_eval_truthy(re));
    CHECK(pb_eval_value_eq(re, re));
    CHECK(!pb_eval_value_eq(re, (pb_value){.kind = PB_STRING, .text = {.ptr = "a", .len = 1}}));
    pb_eval_result r = pb_eval(&ctx, re);
    CHECK(r.err == PB_EVAL_OK && r.value.kind == PB_REGEX && r.value.regex == re.regex);

    // Other forms handed a regex node by hand fail with a type error.
    pb_value call_items[3] = {
        {.kind = PB_SYMBOL, .text = {.ptr = "str-concat", .len = 10}},
        re,
        {.kind = PB_STRING, .text = {.ptr = "x", .len = 1}},
    };
    r = pb_eval(&ctx, (pb_value){.kind = PB_LIST, .seq = {.items = call_items, .len = 3}});
    CHECK(r.err == PB_EVAL_TYPE);
    pb_value pub_items[3] = {
        {.kind = PB_SYMBOL, .text = {.ptr = "publish!", .len = 8}},
        {.kind = PB_STRING, .text = {.ptr = "out", .len = 3}},
        re,
    };
    published pub = {0};
    ctx.publish = publish_cb;
    ctx.publish_ctx = &pub;
    r = pb_eval(&ctx, (pb_value){.kind = PB_LIST, .seq = {.items = pub_items, .len = 3}});
    CHECK(r.err == PB_EVAL_TYPE);
    CHECK(pub.count == 0);
    pb_arena_free(&arena);
}

TEST_MAIN(eval,
          test_payload_bool,
          test_empty_payload_is_not_a_number,
          test_bound_symbols_and_math,
          test_replaying_symbol,
          test_contains_string_and_vector,
          test_if_when_do,
          test_subject_helpers,
          test_publish,
          test_thread_and_numeric_helpers,
          test_stateful_helpers,
          test_edge_and_holdoff_helpers,
          test_windowed_analytics,
          test_rate_throttle_and_clock_forms,
          test_transition_helper,
          test_now_and_bar,
          test_json_get,
          test_json_demux,
          test_call_site_state_identity,
          test_threaded_steps_keep_call_site,
          test_resolve_rejects_unknown_forms_in_any_branch,
          test_resolve_rejects_threaded_on_silence,
          test_resolve_allows_user_hooks_to_own_unknowns,
          test_hysteresis,
          test_ewma,
          test_keyed_is_unknown,
          test_clock_tick_binds_slot_subject,
          test_last,
          test_reply_to_and_reply,
          test_slot_under_every_has_empty_subject,
          test_regex_matches,
          test_regex_capture,
          test_regex_replace,
          test_regex_arity_and_types,
          test_regex_budget,
          test_regex_resolve_errors,
          test_regex_value_is_not_a_string)
