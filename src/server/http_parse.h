#ifndef MB_HTTP_PARSE_H
#define MB_HTTP_PARSE_H

#include "buf.h"
#include "proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MB_HTTP_MAX_HEADER = 8 * 1024,
    MB_HTTP_MAX_TARGET = 1024,
};

typedef enum mb_http_method {
    MB_HTTP_GET,
    MB_HTTP_POST,
} mb_http_method;

// Parsed request head plus POST body. Every slice borrows from the buffer
// given to mb_http_parse_request and is only valid while that buffer is.
typedef struct mb_http_request {
    mb_http_method method;
    mb_slice target;
    mb_slice authorization;
    mb_slice content_type;
    mb_slice body;           // POST only: exactly content_length bytes
    size_t content_length;
    size_t header_len;       // bytes of buf used by the request line and headers
    bool has_authorization;
    bool has_content_length;
    bool has_content_type;
    bool has_transfer_encoding;
} mb_http_request;

typedef enum mb_http_parse_status {
    MB_HTTP_PARSE_NEED_MORE, // buffer is a valid prefix; read more and re-parse from the start
    MB_HTTP_PARSE_OK,        // request is complete; see mb_http_request
    MB_HTTP_PARSE_ERROR,     // reply with error.status and close
} mb_http_parse_status;

// Rejection to send; reason and message are static strings.
typedef struct mb_http_error {
    int status;
    const char *reason;
    const char *message;
} mb_http_error;

// Parses one request from buf[0..len). Allocation-free and stateless: callers
// re-parse from the start as more bytes arrive. GET completes at the end of
// the headers; POST also needs Content-Length bytes of body (limit MB_MAX_PAYLOAD).
// On ERROR, *err is filled in; on OK, *req is filled in.
mb_http_parse_status mb_http_parse_request(const uint8_t *buf, size_t len,
                                           mb_http_request *req, mb_http_error *err);

// Strips leading spaces/tabs and trailing spaces/tabs/CR.
mb_slice mb_http_trim_ows(mb_slice s);

// True for text/plain or application/json, ignoring case and parameters.
bool mb_http_content_type_allowed(mb_slice value);

// Turns "<prefix>a/b%2Fc" into the subject "a.b/c" in out (replacing its
// contents). Rejects empty tokens, NUL, '.', control bytes and invalid
// subjects. out must outlive any slice taken from it.
bool mb_http_decode_subject_path(mb_slice target, const char *prefix, bool allow_wildcards, mb_buf *out);

#endif
