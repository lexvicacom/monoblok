// Fuzzes HTTP/SSE request parsing (src/server/http_parse.c). The first byte
// picks a split point so each input is parsed whole and as a shorter prefix,
// the way http.c re-parses its receive buffer from the start as bytes arrive.
#include "../src/server/http_parse.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check(bool ok) {
    if (!ok) {
        __builtin_trap();
    }
}

static bool inside(mb_slice s, const uint8_t *buf, size_t len) {
    if (s.len == 0) {
        return true;
    }
    return s.ptr >= buf && s.ptr <= buf + len && s.len <= len - (size_t)(s.ptr - buf);
}

// Checks one parse of buf[0..len) and returns its status.
static mb_http_parse_status parse_and_check(const uint8_t *buf, size_t len, mb_http_request *req) {
    mb_http_error err = {0};
    memset(req, 0, sizeof *req);
    const mb_http_parse_status st = mb_http_parse_request(buf, len, req, &err);
    if (st == MB_HTTP_PARSE_ERROR) {
        check(err.status >= 400 && err.status < 500);
        check(err.reason != NULL && err.message != NULL);
        return st;
    }
    if (st == MB_HTTP_PARSE_NEED_MORE) {
        return st;
    }
    check(req->header_len <= len);
    check(req->header_len <= MB_HTTP_MAX_HEADER);
    check(inside(req->target, buf, req->header_len));
    check(inside(req->authorization, buf, req->header_len));
    check(inside(req->content_type, buf, req->header_len));
    check(req->target.len > 0 && req->target.len <= MB_HTTP_MAX_TARGET && req->target.ptr[0] == '/');
    check(memchr(req->target.ptr, '?', req->target.len) == NULL);
    check(memchr(req->target.ptr, '#', req->target.len) == NULL);
    check(!req->has_transfer_encoding);
    if (req->method == MB_HTTP_POST) {
        check(req->has_content_length);
        check(req->content_length <= MB_MAX_PAYLOAD);
        check(req->body.len == req->content_length);
        check(req->body.len == 0 || req->body.ptr == buf + req->header_len);
        check(req->header_len + req->body.len <= len);
        check(inside(req->body, buf, len));
    } else {
        check(req->body.len == 0);
    }
    if (req->has_content_type) {
        (void)mb_http_content_type_allowed(req->content_type);
    }
    static const struct {
        const char *prefix;
        bool wildcards;
    } routes[] = {{"/latest/", false}, {"/sub/", true}, {"/pub/", false}};
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i += 1) {
        mb_buf subject = {0};
        if (mb_http_decode_subject_path(req->target, routes[i].prefix, routes[i].wildcards, &subject)) {
            check(subject.len > 0 && subject.len <= MB_MAX_CONTROL_LINE);
            check(mb_proto_subject_valid((mb_slice){.ptr = subject.ptr, .len = subject.len}, routes[i].wildcards));
        }
        mb_buf_free(&subject);
    }
    return st;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    const size_t split_seed = data[0];
    data += 1;
    size -= 1;
    // Exact-size heap copy so ASan sees any read past the end of the input.
    uint8_t *buf = malloc(size == 0 ? 1 : size);
    if (buf == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(buf, data, size);
    }
    const size_t split = size == 0 ? 0 : (split_seed * (size + 1)) / 256;

    mb_http_request whole = {0};
    const mb_http_parse_status whole_st = parse_and_check(buf, size, &whole);

    uint8_t *head = malloc(split == 0 ? 1 : split);
    if (head != NULL) {
        if (split > 0) {
            memcpy(head, buf, split);
        }
        mb_http_request prefix = {0};
        const mb_http_parse_status prefix_st = parse_and_check(head, split, &prefix);
        // A prefix that already framed a request with the same header block
        // must agree with the whole input on what was framed.
        if (prefix_st == MB_HTTP_PARSE_OK && whole_st == MB_HTTP_PARSE_OK &&
            prefix.header_len == whole.header_len) {
            check(prefix.method == whole.method);
            check(prefix.content_length == whole.content_length);
            check(prefix.target.len == whole.target.len);
        }
        free(head);
    }
    free(buf);
    return 0;
}
