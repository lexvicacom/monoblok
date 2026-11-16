// libFuzzer harness for the NATS client protocol parser (src/proto.c).
//
// Mimics process_rx() in src/server/conn.c: bytes accumulate in an rx buffer,
// mb_parse_client_op runs over the unconsumed tail, and the cursor advances by
// `consumed`. Non-fatal errors skip to the next newline like conn.c does; fatal
// errors and NEED_MORE stop the loop.
//
// The first input byte picks a chunk size (0 = whole stream at once). The same
// stream is parsed whole and chunked; both runs must yield the same op digest,
// which covers partial-frame handling.
#include "proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            __builtin_trap(); \
        } \
    } while (0)

// Running FNV-1a digest of everything the parser reported for a stream.
typedef struct digest {
    uint64_t h;
    size_t ops;
} digest;

static void digest_bytes(digest *d, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i += 1) {
        d->h = (d->h ^ b[i]) * 1099511628211ULL;
    }
}

static void digest_u64(digest *d, uint64_t v) {
    digest_bytes(d, &v, sizeof v);
}

static void digest_slice(digest *d, mb_slice s) {
    digest_u64(d, s.len);
    if (s.len != 0) {
        digest_bytes(d, s.ptr, s.len);
    }
}

// A slice must be empty or lie entirely inside [base, base + len).
static void check_slice_inside(mb_slice s, const uint8_t *base, size_t len) {
    if (s.len == 0) {
        return;
    }
    CHECK(s.ptr != NULL);
    CHECK(s.ptr >= base);
    CHECK((size_t)(s.ptr - base) <= len);
    CHECK(s.len <= len - (size_t)(s.ptr - base));
}

static void check_pub_roundtrip(const mb_op *op) {
    const mb_slice sid = {.ptr = (const uint8_t *)"7", .len = 1};
    size_t expect = 0;
    mb_buf out = {0};
    if (op->reply_to.len != 0) {
        CHECK(mb_msg_frame_len_prefixed_with_reply(&expect, 0, op->subject, sid, op->reply_to, op->payload));
        CHECK(mb_write_msg_with_reply(&out, op->subject, sid, op->reply_to, op->payload));
    } else {
        CHECK(mb_msg_frame_len_prefixed(&expect, 0, op->subject, sid, op->payload));
        CHECK(mb_write_msg(&out, op->subject, sid, op->payload));
    }
    CHECK(out.len == expect);
    CHECK(out.len >= 4 && memcmp(out.ptr, "MSG ", 4) == 0);
    CHECK(out.ptr[out.len - 2] == '\r' && out.ptr[out.len - 1] == '\n');
    CHECK(out.len >= op->payload.len + 2);
    if (op->payload.len != 0) {
        CHECK(memcmp(out.ptr + out.len - 2 - op->payload.len, op->payload.ptr, op->payload.len) == 0);
    }
    mb_buf_free(&out);
}

static void check_op(const mb_op *op, const uint8_t *base, size_t len) {
    check_slice_inside(op->connect, base, len);
    check_slice_inside(op->subject, base, len);
    check_slice_inside(op->reply_to, base, len);
    check_slice_inside(op->queue, base, len);
    check_slice_inside(op->sid, base, len);
    check_slice_inside(op->payload, base, len);
    switch (op->kind) {
    case MB_OP_SUB:
        CHECK(mb_proto_subject_valid(op->subject, true));
        CHECK(mb_proto_token_valid(op->sid));
        CHECK(op->queue.len == 0 || mb_proto_token_valid(op->queue));
        break;
    case MB_OP_UNSUB:
        CHECK(mb_proto_token_valid(op->sid));
        break;
    case MB_OP_PUB:
        CHECK(mb_proto_subject_valid(op->subject, false));
        CHECK(op->reply_to.len == 0 || mb_proto_subject_valid(op->reply_to, false));
        CHECK(op->payload.len <= MB_MAX_PAYLOAD);
        check_pub_roundtrip(op);
        break;
    case MB_OP_CONNECT:
    case MB_OP_PING:
        break;
    }
}

// Feeds `stream` to the parser in `chunk`-sized pieces, as conn.c would see it.
static void run_stream(const uint8_t *stream, size_t size, size_t chunk, digest *d) {
    mb_buf rx = {0};
    size_t fed = 0;
    bool stop = false;
    d->h = 14695981039346656037ULL;
    d->ops = 0;

    while (!stop && (fed < size || rx.len > 0)) {
        if (fed < size) {
            size_t n = size - fed;
            if (n > chunk) {
                n = chunk;
            }
            CHECK(mb_buf_append(&rx, stream + fed, n));
            fed += n;
        }

        size_t cursor = 0;
        while (cursor < rx.len) {
            const size_t avail = rx.len - cursor;
            const uint8_t *at = rx.ptr + cursor;
            const mb_parse_result r = mb_parse_client_op(at, avail);
            if (r.status == MB_PARSE_NEED_MORE) {
                break;
            }
            digest_u64(d, (uint64_t)r.status);
            if (r.status != MB_PARSE_OK) {
                if (r.status == MB_PARSE_CONTROL_LINE_TOO_LONG ||
                    r.status == MB_PARSE_MALFORMED ||
                    r.status == MB_PARSE_PAYLOAD_TOO_LARGE) {
                    stop = true;
                    break;
                }
                const void *nlp = memchr(at, '\n', avail);
                if (nlp == NULL) {
                    break;
                }
                cursor = (size_t)((const uint8_t *)nlp - rx.ptr) + 1;
                continue;
            }
            CHECK(r.consumed > 0);
            CHECK(r.consumed <= avail);
            check_op(&r.op, at, avail);
            digest_u64(d, (uint64_t)r.op.kind);
            digest_u64(d, r.consumed);
            digest_slice(d, r.op.connect);
            digest_slice(d, r.op.subject);
            digest_slice(d, r.op.reply_to);
            digest_slice(d, r.op.queue);
            digest_slice(d, r.op.sid);
            digest_slice(d, r.op.payload);
            digest_u64(d, r.op.has_max_msgs ? r.op.max_msgs + 1 : 0);
            d->ops += 1;
            cursor += r.consumed;
        }
        CHECK(cursor <= rx.len);
        mb_buf_consume(&rx, cursor);

        // Out of input and the tail is an incomplete frame: done.
        if (fed == size) {
            break;
        }
    }
    mb_buf_free(&rx);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    const size_t chunk = data[0] == 0 ? size : data[0];
    const uint8_t *stream = data + 1;
    const size_t stream_len = size - 1;

    digest whole = {0};
    digest split = {0};
    run_stream(stream, stream_len, stream_len == 0 ? 1 : stream_len, &whole);
    run_stream(stream, stream_len, chunk, &split);
    CHECK(whole.h == split.h);
    CHECK(whole.ops == split.ops);
    return 0;
}
