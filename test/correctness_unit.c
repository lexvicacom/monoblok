#include "pb_program.h"
#include "test_check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HOT_SUBJECTS = 3,
    HOT_LITERAL_SUBS = 96,
    HOT_WILDCARD_SUBS = 32,
    HOT_PUBS = 3072,
    PARSER_HAMMER_SUBJECTS = 16,
    PARSER_HAMMER_PUBS = 65536,
    NUM_SUBJECTS = 3,
    NUM_METRIC_SUBS = 8,
    NUM_PUBS_PER_SUBJECT = 256,
};

// Decoded server MSG frame; all slices borrow from the parsed output buffer.
typedef struct msg_frame {
    mb_slice subject;
    mb_slice sid;
    mb_slice payload;
} msg_frame;

// Expected fan-out stream shape for a single subscriber.
typedef struct fanout_expect {
    size_t subject_idx;
    bool wildcard;
} fanout_expect;

typedef enum metric_kind {
    METRIC_AVG4,
    METRIC_SUM8,
    METRIC_COUNT,
} metric_kind;

// Expected derived numeric stream shape for one output subject.
typedef struct metric_expect {
    size_t subject_idx;
    metric_kind kind;
    const char *subject;
} metric_expect;

typedef void (*frame_check_fn)(const msg_frame *frame, size_t index, void *ctx);

static mb_slice lit(const char *s) {
    return (mb_slice){.ptr = (const uint8_t *)s, .len = strlen(s)};
}

static void check_slice_lit(mb_slice s, const char *want) {
    CHECK(s.len == strlen(want));
    CHECK(memcmp(s.ptr, want, s.len) == 0);
}

static void expect_bytes(const mb_buf *buf, size_t *pos, const char *want) {
    const size_t len = strlen(want);
    CHECK(*pos <= buf->len);
    CHECK(len <= buf->len - *pos);
    CHECK(memcmp(buf->ptr + *pos, want, len) == 0);
    *pos += len;
}

static mb_slice read_until_byte(const mb_buf *buf, size_t *pos, uint8_t delim) {
    const size_t start = *pos;
    while (*pos < buf->len && buf->ptr[*pos] != delim) {
        *pos += 1;
    }
    CHECK(*pos < buf->len);
    const mb_slice out = {.ptr = buf->ptr + start, .len = *pos - start};
    *pos += 1;
    return out;
}

static size_t parse_size_slice(mb_slice s) {
    CHECK(s.len > 0);
    size_t out = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        const uint8_t ch = s.ptr[i];
        CHECK(ch >= '0' && ch <= '9');
        const size_t digit = (size_t)(ch - '0');
        CHECK(out <= (SIZE_MAX - digit) / 10);
        out = out * 10 + digit;
    }
    return out;
}

static uint64_t parse_u64_slice(mb_slice s) {
    CHECK(s.len > 0);
    uint64_t out = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        const uint8_t ch = s.ptr[i];
        CHECK(ch >= '0' && ch <= '9');
        const uint64_t digit = (uint64_t)(ch - '0');
        CHECK(out <= (UINT64_MAX - digit) / 10);
        out = out * 10 + digit;
    }
    return out;
}

static double parse_double_slice(mb_slice s) {
    CHECK(s.len > 0 && s.len < 64);
    char tmp[64];
    memcpy(tmp, s.ptr, s.len);
    tmp[s.len] = '\0';
    char *end = NULL;
    const double out = strtod(tmp, &end);
    CHECK(end == tmp + s.len);
    return out;
}

static bool write_subject(char *buf, size_t cap, const char *prefix, size_t idx) {
    const int n = snprintf(buf, cap, "%s%zu", prefix, idx);
    return n >= 0 && (size_t)n < cap;
}

static bool write_metric_subject(char *buf, size_t cap, size_t idx, const char *suffix) {
    const int n = snprintf(buf, cap, "num.%zu.%s", idx, suffix);
    return n >= 0 && (size_t)n < cap;
}

static bool write_parser_hammer_payload(char *buf, size_t cap, size_t subject_idx, size_t pub_idx) {
    const int n = snprintf(buf, cap, "payload-%zu-%zu", subject_idx, pub_idx);
    return n >= 0 && (size_t)n < cap;
}

static void append_parser_hammer_pub(mb_buf *buf, size_t pub_idx) {
    const size_t subject_idx = pub_idx % PARSER_HAMMER_SUBJECTS;
    char subject[32];
    char payload[64];
    char frame[128];

    CHECK(write_subject(subject, sizeof subject, "load.", subject_idx));
    CHECK(write_parser_hammer_payload(payload, sizeof payload, subject_idx, pub_idx));

    const int n = snprintf(frame, sizeof frame, "PUB %s %zu\r\n%s\r\n", subject, strlen(payload), payload);
    CHECK(n >= 0 && (size_t)n < sizeof frame);
    CHECK(mb_buf_append(buf, frame, (size_t)n));
}

static msg_frame read_msg_frame(const mb_buf *buf, size_t *pos) {
    expect_bytes(buf, pos, "MSG ");
    const mb_slice subject = read_until_byte(buf, pos, ' ');
    const mb_slice sid = read_until_byte(buf, pos, ' ');
    const mb_slice len_token = read_until_byte(buf, pos, '\r');
    expect_bytes(buf, pos, "\n");

    const size_t payload_len = parse_size_slice(len_token);
    CHECK(payload_len <= buf->len - *pos);
    const mb_slice payload = {.ptr = buf->ptr + *pos, .len = payload_len};
    *pos += payload_len;
    expect_bytes(buf, pos, "\r\n");

    return (msg_frame){.subject = subject, .sid = sid, .payload = payload};
}

static size_t check_frames(const mb_buf *buf, frame_check_fn check, void *ctx) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < buf->len) {
        const msg_frame frame = read_msg_frame(buf, &pos);
        CHECK(frame.sid.len > 0);
        check(&frame, count, ctx);
        count += 1;
    }
    CHECK(pos == buf->len);
    return count;
}

static void check_fanout_frame(const msg_frame *frame, size_t index, void *ctx) {
    const fanout_expect *expect = ctx;
    const uint64_t payload = parse_u64_slice(frame->payload);
    const size_t subject_idx = expect->wildcard ? (size_t)(payload % HOT_SUBJECTS) : expect->subject_idx;
    const uint64_t expected_payload = expect->wildcard
                                          ? (uint64_t)index
                                          : (uint64_t)(expect->subject_idx + index * HOT_SUBJECTS);
    char subject[32];

    CHECK(write_subject(subject, sizeof subject, "hot.", subject_idx));
    check_slice_lit(frame->subject, subject);
    CHECK(payload == expected_payload);
}

static double expected_metric_value(metric_kind kind, size_t subject_idx, size_t index) {
    const double base = (double)(subject_idx * 1000);
    if (kind == METRIC_COUNT) {
        return (double)(index + 1);
    }

    const size_t window = kind == METRIC_AVG4 ? 4 : 8;
    const size_t len = index + 1 < window ? index + 1 : window;
    const size_t first = index + 1 - len;
    const double edge_sum = ((double)first + (double)index) * (double)len / 2.0;
    const double sum = base * (double)len + edge_sum;
    return kind == METRIC_AVG4 ? sum / (double)len : sum;
}

static void check_metric_frame(const msg_frame *frame, size_t index, void *ctx) {
    const metric_expect *expect = ctx;
    const double got = parse_double_slice(frame->payload);
    const double want = expected_metric_value(expect->kind, expect->subject_idx, index);

    check_slice_lit(frame->subject, expect->subject);
    CHECK(fabs(got - want) < 0.000000001);
}

static void test_router_hot_subject_fanout_exact_counts(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn literal_conns[HOT_SUBJECTS][HOT_LITERAL_SUBS] = {0};
    mb_router_conn wildcard_conns[HOT_WILDCARD_SUBS] = {0};

    for (size_t subject_idx = 0; subject_idx < HOT_SUBJECTS; subject_idx += 1) {
        char subject[32];
        CHECK(write_subject(subject, sizeof subject, "hot.", subject_idx));
        for (size_t sub_idx = 0; sub_idx < HOT_LITERAL_SUBS; sub_idx += 1) {
            char sid[32];
            const int n = snprintf(sid, sizeof sid, "l%zu_%zu", subject_idx, sub_idx);
            CHECK(n >= 0 && (size_t)n < sizeof sid);
            CHECK(mb_router_subscribe(&router, &literal_conns[subject_idx][sub_idx], lit(subject), lit(sid)));
        }
    }

    for (size_t sub_idx = 0; sub_idx < HOT_WILDCARD_SUBS; sub_idx += 1) {
        char sid[32];
        const int n = snprintf(sid, sizeof sid, "w%zu", sub_idx);
        CHECK(n >= 0 && (size_t)n < sizeof sid);
        CHECK(mb_router_subscribe(&router, &wildcard_conns[sub_idx], lit("hot.*"), lit(sid)));
    }

    for (size_t i = 0; i < HOT_PUBS; i += 1) {
        char subject[32];
        char payload[32];
        CHECK(write_subject(subject, sizeof subject, "hot.", i % HOT_SUBJECTS));
        const int n = snprintf(payload, sizeof payload, "%zu", i);
        CHECK(n >= 0 && (size_t)n < sizeof payload);
        CHECK(mb_router_publish(&router, lit(subject), lit(payload)));
    }

    for (size_t subject_idx = 0; subject_idx < HOT_SUBJECTS; subject_idx += 1) {
        for (size_t sub_idx = 0; sub_idx < HOT_LITERAL_SUBS; sub_idx += 1) {
            fanout_expect expect = {.subject_idx = subject_idx};
            CHECK(check_frames(&literal_conns[subject_idx][sub_idx].out, check_fanout_frame, &expect) == HOT_PUBS / HOT_SUBJECTS);
            mb_buf_free(&literal_conns[subject_idx][sub_idx].out);
        }
    }

    for (size_t sub_idx = 0; sub_idx < HOT_WILDCARD_SUBS; sub_idx += 1) {
        fanout_expect expect = {.wildcard = true};
        CHECK(check_frames(&wildcard_conns[sub_idx].out, check_fanout_frame, &expect) == HOT_PUBS);
        mb_buf_free(&wildcard_conns[sub_idx].out);
    }

    mb_router_free(&router);
}

static void check_metric_conns(mb_router_conn conns[NUM_SUBJECTS][NUM_METRIC_SUBS],
                               metric_kind kind, const char *suffix) {
    for (size_t subject_idx = 0; subject_idx < NUM_SUBJECTS; subject_idx += 1) {
        char subject[32];
        CHECK(write_metric_subject(subject, sizeof subject, subject_idx, suffix));
        for (size_t sub_idx = 0; sub_idx < NUM_METRIC_SUBS; sub_idx += 1) {
            metric_expect expect = {
                .subject_idx = subject_idx,
                .kind = kind,
                .subject = subject,
            };
            CHECK(check_frames(&conns[subject_idx][sub_idx].out, check_metric_frame, &expect) == NUM_PUBS_PER_SUBJECT);
            mb_buf_free(&conns[subject_idx][sub_idx].out);
        }
    }
}

static void test_patchbay_numeric_streams_exact_under_fanout(void) {
    static const char source[] =
        "(on \"num.*\"\n"
        "  (do\n"
        "    (publish! (subject-append \"avg4\") (moving-avg 4 payload-float))\n"
        "    (publish! (subject-append \"sum8\") (moving-sum 8 payload-float))\n"
        "    (count!)))\n";

    mb_router router;
    mb_router_init(&router);
    pb_program program = {0};
    mb_router_conn avg_conns[NUM_SUBJECTS][NUM_METRIC_SUBS] = {0};
    mb_router_conn sum_conns[NUM_SUBJECTS][NUM_METRIC_SUBS] = {0};
    mb_router_conn count_conns[NUM_SUBJECTS][NUM_METRIC_SUBS] = {0};

    CHECK(pb_program_load_source(&program, "<correctness>", source, strlen(source)));

    for (size_t subject_idx = 0; subject_idx < NUM_SUBJECTS; subject_idx += 1) {
        char avg_subject[32];
        char sum_subject[32];
        char count_subject[32];
        CHECK(write_metric_subject(avg_subject, sizeof avg_subject, subject_idx, "avg4"));
        CHECK(write_metric_subject(sum_subject, sizeof sum_subject, subject_idx, "sum8"));
        CHECK(write_metric_subject(count_subject, sizeof count_subject, subject_idx, "count"));
        for (size_t sub_idx = 0; sub_idx < NUM_METRIC_SUBS; sub_idx += 1) {
            char sid[32];
            int n = snprintf(sid, sizeof sid, "a%zu_%zu", subject_idx, sub_idx);
            CHECK(n >= 0 && (size_t)n < sizeof sid);
            CHECK(mb_router_subscribe(&router, &avg_conns[subject_idx][sub_idx], lit(avg_subject), lit(sid)));
            n = snprintf(sid, sizeof sid, "s%zu_%zu", subject_idx, sub_idx);
            CHECK(n >= 0 && (size_t)n < sizeof sid);
            CHECK(mb_router_subscribe(&router, &sum_conns[subject_idx][sub_idx], lit(sum_subject), lit(sid)));
            n = snprintf(sid, sizeof sid, "c%zu_%zu", subject_idx, sub_idx);
            CHECK(n >= 0 && (size_t)n < sizeof sid);
            CHECK(mb_router_subscribe(&router, &count_conns[subject_idx][sub_idx], lit(count_subject), lit(sid)));
        }
    }

    for (size_t i = 0; i < NUM_SUBJECTS * NUM_PUBS_PER_SUBJECT; i += 1) {
        const size_t subject_idx = i % NUM_SUBJECTS;
        const size_t local_idx = i / NUM_SUBJECTS;
        const size_t value = subject_idx * 1000 + local_idx;
        char subject[32];
        char payload[32];
        CHECK(write_subject(subject, sizeof subject, "num.", subject_idx));
        const int n = snprintf(payload, sizeof payload, "%zu", value);
        CHECK(n >= 0 && (size_t)n < sizeof payload);
        CHECK(mb_router_publish(&router, lit(subject), lit(payload)));
        CHECK(pb_program_eval_publish(&program, &router, lit(subject), lit(payload), (uint64_t)i, 0));
    }

    check_metric_conns(avg_conns, METRIC_AVG4, "avg4");
    check_metric_conns(sum_conns, METRIC_SUM8, "sum8");
    check_metric_conns(count_conns, METRIC_COUNT, "count");

    pb_program_free(&program);
    mb_router_free(&router);
}

static void test_parser_hammer_large_pub_stream_in_memory(void) {
    mb_buf stream = {0};

    for (size_t i = 0; i < PARSER_HAMMER_PUBS; i += 1) {
        append_parser_hammer_pub(&stream, i);
    }

    size_t cursor = 0;
    for (size_t i = 0; i < PARSER_HAMMER_PUBS; i += 1) {
        const size_t subject_idx = i % PARSER_HAMMER_SUBJECTS;
        char subject[32];
        char payload[64];
        char frame[128];

        CHECK(write_subject(subject, sizeof subject, "load.", subject_idx));
        CHECK(write_parser_hammer_payload(payload, sizeof payload, subject_idx, i));
        const int n = snprintf(frame, sizeof frame, "PUB %s %zu\r\n%s\r\n", subject, strlen(payload), payload);
        CHECK(n >= 0 && (size_t)n < sizeof frame);

        const mb_parse_result r = mb_parse_client_op(stream.ptr + cursor, stream.len - cursor);
        CHECK(r.status == MB_PARSE_OK);
        CHECK(r.op.kind == MB_OP_PUB);
        CHECK(r.consumed == (size_t)n);
        check_slice_lit(r.op.subject, subject);
        check_slice_lit(r.op.payload, payload);
        cursor += r.consumed;
    }

    CHECK(cursor == stream.len);
    mb_buf_free(&stream);
}

TEST_MAIN(correctness,
          test_router_hot_subject_fanout_exact_counts,
          test_patchbay_numeric_streams_exact_under_fanout,
          test_parser_hammer_large_pub_stream_in_memory)
