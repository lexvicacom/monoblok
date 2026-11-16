#include "http_parse.h"

#include "proto.h"
#include "slice.h"

#include <string.h>

mb_slice mb_http_trim_ows(mb_slice s) {
    while (s.len > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) {
        s.ptr += 1;
        s.len -= 1;
    }
    while (s.len > 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t' || s.ptr[s.len - 1] == '\r')) {
        s.len -= 1;
    }
    return s;
}


static bool slice_eq_lit(mb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}


static bool media_type_is(mb_slice value, const char *media_type) {
    value = mb_http_trim_ows(value);
    const size_t n = strlen(media_type);
    if (!mb_slice_has_prefix_ci_lit(value, media_type)) {
        return false;
    }
    if (value.len == n) {
        return true;
    }
    return value.ptr[n] == ';' || value.ptr[n] == ' ' || value.ptr[n] == '\t';
}

bool mb_http_content_type_allowed(mb_slice value) {
    return media_type_is(value, "text/plain") || media_type_is(value, "application/json");
}


static bool parse_usize_slice(mb_slice s, size_t *out) {
    s = mb_http_trim_ows(s);
    if (s.len == 0) {
        return false;
    }
    size_t v = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') {
            return false;
        }
        const size_t digit = (size_t)(s.ptr[i] - '0');
        if (v > (SIZE_MAX - digit) / 10) {
            return false;
        }
        v = v * 10 + digit;
    }
    *out = v;
    return true;
}


static bool find_header_end(const uint8_t *buf, size_t len, size_t *out) {
    for (size_t i = 0; i + 3 < len; i += 1) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *out = i + 4;
            return true;
        }
    }
    for (size_t i = 0; i + 1 < len; i += 1) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            *out = i + 2;
            return true;
        }
    }
    return false;
}

static bool next_line(mb_slice *rest, mb_slice *line) {
    if (rest->len == 0) {
        return false;
    }
    size_t i = 0;
    while (i < rest->len && rest->ptr[i] != '\n') {
        i += 1;
    }
    if (i == rest->len) {
        *line = *rest;
        rest->ptr += rest->len;
        rest->len = 0;
    } else {
        *line = (mb_slice){.ptr = rest->ptr, .len = i};
        rest->ptr += i + 1;
        rest->len -= i + 1;
    }
    if (line->len > 0 && line->ptr[line->len - 1] == '\r') {
        line->len -= 1;
    }
    return true;
}

static bool next_token(mb_slice *rest, mb_slice *tok) {
    *rest = mb_http_trim_ows(*rest);
    if (rest->len == 0) {
        return false;
    }
    size_t i = 0;
    while (i < rest->len && rest->ptr[i] != ' ' && rest->ptr[i] != '\t') {
        i += 1;
    }
    *tok = (mb_slice){.ptr = rest->ptr, .len = i};
    rest->ptr += i;
    rest->len -= i;
    return true;
}

static bool parse_head(mb_slice header, mb_http_request *req) {
    *req = (mb_http_request){0};
    mb_slice line = {0};
    if (!next_line(&header, &line)) {
        return false;
    }
    mb_slice rest = line;
    mb_slice method = {0};
    mb_slice target = {0};
    mb_slice version = {0};
    mb_slice extra = {0};
    if (!next_token(&rest, &method) || !next_token(&rest, &target) || !next_token(&rest, &version) || next_token(&rest, &extra)) {
        return false;
    }
    if (slice_eq_lit(method, "GET")) {
        req->method = MB_HTTP_GET;
    } else if (slice_eq_lit(method, "POST")) {
        req->method = MB_HTTP_POST;
    } else {
        return false;
    }
    if ((!slice_eq_lit(version, "HTTP/1.1") && !slice_eq_lit(version, "HTTP/1.0")) ||
        target.len == 0 || target.len > MB_HTTP_MAX_TARGET || target.ptr[0] != '/') {
        return false;
    }
    for (size_t i = 0; i < target.len; i += 1) {
        if (target.ptr[i] == '?' || target.ptr[i] == '#') {
            return false;
        }
    }
    req->target = target;

    while (next_line(&header, &line)) {
        if (line.len == 0) {
            continue;
        }
        const void *colonp = memchr(line.ptr, ':', line.len);
        if (colonp == NULL) {
            return false;
        }
        const uint8_t *colon = colonp;
        mb_slice name = {.ptr = line.ptr, .len = (size_t)(colon - line.ptr)};
        mb_slice value = {.ptr = colon + 1, .len = line.len - name.len - 1};
        name = mb_http_trim_ows(name);
        value = mb_http_trim_ows(value);
        if (name.len == 0) {
            return false;
        }
        if (mb_slice_eq_ci_lit(name, "authorization")) {
            req->authorization = value;
            req->has_authorization = true;
        } else if (mb_slice_eq_ci_lit(name, "content-type")) {
            req->content_type = value;
            req->has_content_type = true;
        } else if (mb_slice_eq_ci_lit(name, "content-length")) {
            if (req->has_content_length || !parse_usize_slice(value, &req->content_length)) {
                return false;
            }
            req->has_content_length = true;
        } else if (mb_slice_eq_ci_lit(name, "transfer-encoding")) {
            req->has_transfer_encoding = true;
        }
    }
    return true;
}

mb_http_parse_status mb_http_parse_request(const uint8_t *buf, size_t len,
                                           mb_http_request *req, mb_http_error *err) {
    size_t header_end = 0;
    if (!find_header_end(buf, len, &header_end)) {
        if (len > MB_HTTP_MAX_HEADER) {
            *err = (mb_http_error){431, "Request Header Fields Too Large", "headers too large\n"};
            return MB_HTTP_PARSE_ERROR;
        }
        return MB_HTTP_PARSE_NEED_MORE;
    }
    if (header_end > MB_HTTP_MAX_HEADER) {
        *err = (mb_http_error){431, "Request Header Fields Too Large", "headers too large\n"};
        return MB_HTTP_PARSE_ERROR;
    }
    if (!parse_head((mb_slice){.ptr = buf, .len = header_end}, req)) {
        *err = (mb_http_error){400, "Bad Request", "bad request\n"};
        return MB_HTTP_PARSE_ERROR;
    }
    req->header_len = header_end;
    if (req->has_transfer_encoding) {
        *err = (mb_http_error){400, "Bad Request", "transfer encoding is not supported\n"};
        return MB_HTTP_PARSE_ERROR;
    }
    if (req->method == MB_HTTP_GET) {
        return MB_HTTP_PARSE_OK;
    }
    if (!req->has_content_length) {
        *err = (mb_http_error){411, "Length Required", "Content-Length is required\n"};
        return MB_HTTP_PARSE_ERROR;
    }
    if (req->content_length > MB_MAX_PAYLOAD) {
        *err = (mb_http_error){413, "Payload Too Large", "payload too large\n"};
        return MB_HTTP_PARSE_ERROR;
    }
    if (len - header_end < req->content_length) {
        return MB_HTTP_PARSE_NEED_MORE;
    }
    req->body = (mb_slice){.ptr = buf + header_end, .len = req->content_length};
    return MB_HTTP_PARSE_OK;
}

static int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static bool append_decoded_path_byte(mb_buf *out, uint8_t c) {
    if (c == '\0' || c == '/' || c == '.' || c <= ' ' || c == 0x7f) {
        return false;
    }
    if (out->len >= MB_MAX_CONTROL_LINE) {
        return false;
    }
    return mb_buf_append_byte(out, c);
}

bool mb_http_decode_subject_path(mb_slice target, const char *prefix, bool allow_wildcards, mb_buf *out) {
    const size_t prefix_len = strlen(prefix);
    if (target.len <= prefix_len || memcmp(target.ptr, prefix, prefix_len) != 0) {
        return false;
    }
    mb_buf_clear(out);
    mb_slice path = {.ptr = target.ptr + prefix_len, .len = target.len - prefix_len};
    bool token_has_bytes = false;
    for (size_t i = 0; i < path.len; i += 1) {
        uint8_t c = path.ptr[i];
        if (c == '/') {
            if (!token_has_bytes || out->len >= MB_MAX_CONTROL_LINE) {
                return false;
            }
            if (!mb_buf_append_byte(out, '.')) {
                return false;
            }
            token_has_bytes = false;
            continue;
        }
        if (c == '%') {
            if (i + 2 >= path.len) {
                return false;
            }
            const int hi = hex_value(path.ptr[i + 1]);
            const int lo = hex_value(path.ptr[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            c = (uint8_t)((hi << 4) | lo);
            i += 2;
        }
        if (!append_decoded_path_byte(out, c)) {
            return false;
        }
        token_has_bytes = true;
    }
    if (!token_has_bytes) {
        return false;
    }
    const mb_slice subject = {.ptr = out->ptr, .len = out->len};
    return mb_proto_subject_valid(subject, allow_wildcards);
}

