#include "http.h"

#if __has_include("mb_version.h")
#include "mb_version.h"
#endif

#ifndef MB_VERSION
#define MB_VERSION "0.0.0"
#endif

// Sent on every HTTP response, for example "Server: monoblok/v0.3.15".
#define MB_HTTP_SERVER_HEADER "Server: monoblok/v" MB_VERSION "\r\n"

#include "http_parse.h"
#include "slice.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MB_HTTP_READ_CHUNK = 16 * 1024,
};

// HTTP connection state; SSE streams also own one normal router connection.
struct mb_http_conn {
    uv_tcp_t tcp;
    uv_write_t write_req;
    mb_server *server;
    mb_router_conn router_conn;
    mb_buf rx;
    mb_buf in_flight;
    mb_buf subject;
    uint8_t read_buf[MB_HTTP_READ_CHUNK];
    uint64_t client_id;
    bool write_pending;
    bool close_after_write;
    bool closing;
    bool counted;
    bool sse;
    mb_http_conn *prev;
    mb_http_conn *next;
};

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void write_cb(uv_write_t *req, int status);

static bool add_size(size_t *total, size_t add) {
    if (add > SIZE_MAX - *total) {
        return false;
    }
    *total += add;
    return true;
}

static bool append_lit(mb_buf *out, const char *lit) {
    return mb_buf_append(out, lit, strlen(lit));
}

static bool append_usize(mb_buf *out, size_t n) {
    char tmp[32];
    const int written = snprintf(tmp, sizeof tmp, "%zu", n);
    return written >= 0 && (size_t)written < sizeof tmp &&
           mb_buf_append(out, tmp, (size_t)written);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    mb_http_conn *conn = handle->data;
    const size_t n = suggested_size < MB_HTTP_READ_CHUNK ? suggested_size : MB_HTTP_READ_CHUNK;
    buf->base = (char *)conn->read_buf;
    buf->len = n;
}

static void unlink_conn(mb_http_conn *conn) {
    if (conn->prev == NULL && conn->next == NULL && conn->server->http_conns != conn) {
        return;
    }
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        conn->server->http_conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    conn->prev = NULL;
    conn->next = NULL;
}

static void close_cb(uv_handle_t *handle) {
    mb_http_conn *conn = handle->data;
    if (conn->counted && conn->server->conn_count > 0) {
        conn->server->conn_count -= 1;
    }
    mb_buf_free(&conn->rx);
    mb_buf_free(&conn->router_conn.out);
    mb_buf_free(&conn->in_flight);
    mb_buf_free(&conn->subject);
    free(conn);
}

static void http_conn_begin_close(mb_http_conn *conn) {
    if (conn->closing) {
        return;
    }
    conn->closing = true;
    conn->router_conn.closed = true;
    mb_router_remove_all_for(&conn->server->router, &conn->router_conn);
    unlink_conn(conn);
    uv_read_stop((uv_stream_t *)&conn->tcp);
    if (!uv_is_closing((uv_handle_t *)&conn->tcp)) {
        uv_close((uv_handle_t *)&conn->tcp, close_cb);
    }
}

static void close_from_router(void *ctx) {
    http_conn_begin_close(ctx);
}

static size_t pending_bytes(const mb_http_conn *conn) {
    if (conn->router_conn.out.len > SIZE_MAX - conn->in_flight.len) {
        return SIZE_MAX;
    }
    return conn->router_conn.out.len + conn->in_flight.len;
}

static bool close_if_slow_consumer(mb_http_conn *conn) {
    const size_t pending = pending_bytes(conn);
    if (pending <= MB_MAX_PENDING) {
        return false;
    }
    fprintf(stderr, "warn: http conn %" PRIu64 " slow consumer, closing: pending=%zuB cap=%zuB\n",
            conn->client_id, pending, (size_t)MB_MAX_PENDING);
    http_conn_begin_close(conn);
    return true;
}

static bool write_socket(mb_http_conn *conn) {
    if (conn->closing || conn->write_pending || conn->router_conn.out.len == 0) {
        return true;
    }
    mb_buf_swap(&conn->router_conn.out, &conn->in_flight);
    if (conn->in_flight.len > UINT_MAX) {
        http_conn_begin_close(conn);
        return false;
    }
    uv_buf_t uvb = uv_buf_init((char *)conn->in_flight.ptr, (unsigned int)conn->in_flight.len);
    conn->write_req.data = conn;
    conn->write_pending = true;
    const int rc = uv_write(&conn->write_req, (uv_stream_t *)&conn->tcp, &uvb, 1, write_cb);
    if (rc != 0) {
        conn->write_pending = false;
        http_conn_begin_close(conn);
        return false;
    }
    return true;
}

static void kick_write(void *ctx) {
    mb_http_conn *conn = ctx;
    if (conn->closing || close_if_slow_consumer(conn) || conn->write_pending) {
        return;
    }
    (void)write_socket(conn);
}

static void write_cb(uv_write_t *req, int status) {
    mb_http_conn *conn = req->data;
    conn->write_pending = false;
    if (status < 0 || conn->closing) {
        http_conn_begin_close(conn);
        return;
    }
    mb_buf_clear(&conn->in_flight);
    if (conn->close_after_write && conn->router_conn.out.len == 0) {
        http_conn_begin_close(conn);
        return;
    }
    kick_write(conn);
}

static bool write_response_bytes(mb_http_conn *conn, int code, const char *reason,
                                 const char *extra_header, const char *content_type, mb_slice body) {
    const char *type = content_type == NULL ? "text/plain; charset=utf-8" : content_type;
    if (!append_lit(&conn->router_conn.out, "HTTP/1.1 ") ||
        !append_usize(&conn->router_conn.out, (size_t)code) ||
        !mb_buf_append_byte(&conn->router_conn.out, ' ') ||
        !append_lit(&conn->router_conn.out, reason) ||
        !append_lit(&conn->router_conn.out, "\r\n" MB_HTTP_SERVER_HEADER "Connection: close\r\nContent-Length: ") ||
        !append_usize(&conn->router_conn.out, body.len) ||
        !append_lit(&conn->router_conn.out, "\r\nContent-Type: ") ||
        !append_lit(&conn->router_conn.out, type) ||
        !append_lit(&conn->router_conn.out, "\r\n")) {
        return false;
    }
    if (extra_header != NULL && !append_lit(&conn->router_conn.out, extra_header)) {
        return false;
    }
    return append_lit(&conn->router_conn.out, "\r\n") &&
           mb_buf_append(&conn->router_conn.out, body.ptr, body.len);
}

static bool write_response(mb_http_conn *conn, int code, const char *reason,
                           const char *extra_header, const char *body) {
    const char *payload = body == NULL ? "" : body;
    return write_response_bytes(conn, code, reason, extra_header, NULL,
                                (mb_slice){.ptr = (const uint8_t *)payload, .len = strlen(payload)});
}

static void respond_and_close(mb_http_conn *conn, int code, const char *reason,
                              const char *extra_header, const char *body) {
    if (!write_response(conn, code, reason, extra_header, body)) {
        http_conn_begin_close(conn);
        return;
    }
    conn->close_after_write = true;
    kick_write(conn);
}

static void respond_bytes_and_close(mb_http_conn *conn, int code, const char *reason,
                                    const char *extra_header, const char *content_type, mb_slice body) {
    if (!write_response_bytes(conn, code, reason, extra_header, content_type, body)) {
        http_conn_begin_close(conn);
        return;
    }
    conn->close_after_write = true;
    kick_write(conn);
}

static bool slice_has_nul(mb_slice s) {
    return s.len != 0 && memchr(s.ptr, '\0', s.len) != NULL;
}

static bool json_string_len(size_t *total, mb_slice s) {
    for (size_t i = 0; i < s.len; i += 1) {
        const uint8_t c = s.ptr[i];
        if (c == '\"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') {
            if (!add_size(total, 2)) return false;
        } else if (c < 0x20) {
            if (!add_size(total, 6)) return false;
        } else if (!add_size(total, 1)) {
            return false;
        }
    }
    return true;
}

static bool append_json_string(mb_buf *out, mb_slice s) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < s.len; i += 1) {
        const uint8_t c = s.ptr[i];
        if (c == '\"' || c == '\\') {
            if (!mb_buf_append_byte(out, '\\') || !mb_buf_append_byte(out, c)) return false;
        } else if (c == '\n') {
            if (!append_lit(out, "\\n")) return false;
        } else if (c == '\r') {
            if (!append_lit(out, "\\r")) return false;
        } else if (c == '\t') {
            if (!append_lit(out, "\\t")) return false;
        } else if (c == '\b') {
            if (!append_lit(out, "\\b")) return false;
        } else if (c == '\f') {
            if (!append_lit(out, "\\f")) return false;
        } else if (c < 0x20) {
            char esc[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xf]};
            if (!mb_buf_append(out, esc, sizeof esc)) return false;
        } else if (!mb_buf_append_byte(out, c)) {
            return false;
        }
    }
    return true;
}

static bool sse_msg_len(void *ctx, size_t *out,
                        const char *prefix, size_t prefix_len,
                        mb_slice subject, mb_slice sid, mb_slice reply_to, mb_slice payload) {
    (void)ctx;
    (void)sid;
    (void)reply_to;
    if (slice_has_nul(payload)) {
        *out = 0;
        return true;
    }
    size_t n = strlen("event: msg\ndata: {\"subject\":\"") + strlen("\",\"payload\":\"") + strlen("\"}\n\n");
    if (!add_size(&n, prefix_len) ||
        !json_string_len(&n, subject) ||
        !json_string_len(&n, payload)) {
        return false;
    }
    *out = n;
    (void)prefix;
    return true;
}

static bool sse_write_msg(void *ctx, mb_buf *out,
                          const char *prefix, size_t prefix_len,
                          mb_slice subject, mb_slice sid, mb_slice reply_to, mb_slice payload) {
    (void)ctx;
    (void)sid;
    (void)reply_to;
    if (slice_has_nul(payload)) {
        return true;
    }
    return append_lit(out, "event: msg\ndata: {\"subject\":\"") &&
           mb_buf_append(out, prefix, prefix_len) &&
           append_json_string(out, subject) &&
           append_lit(out, "\",\"payload\":\"") &&
           append_json_string(out, payload) &&
           append_lit(out, "\"}\n\n");
}

static int base64_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_decode(mb_slice in, mb_buf *out) {
    mb_buf_clear(out);
    uint32_t acc = 0;
    int bits = 0;
    bool padded = false;
    for (size_t i = 0; i < in.len; i += 1) {
        const uint8_t c = in.ptr[i];
        if (c == '=') {
            padded = true;
            continue;
        }
        const int v = base64_value(c);
        if (v < 0 || padded) {
            return false;
        }
        acc = ((acc << 6) | (uint32_t)v) & UINT32_C(0xffffff);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (!mb_buf_append_byte(out, (uint8_t)(acc >> bits))) {
                return false;
            }
            if (bits > 0) {
                acc &= (UINT32_C(1) << bits) - 1;
            } else {
                acc = 0;
            }
        }
    }
    return true;
}

static bool auth_bearer(mb_http_conn *conn, mb_slice value) {
    if (!mb_slice_has_prefix_ci_lit(value, "Bearer")) {
        return false;
    }
    value.ptr += strlen("Bearer");
    value.len -= strlen("Bearer");
    value = mb_http_trim_ows(value);
    return value.len != 0 && mb_auth_token_matches(&conn->server->auth, value);
}

static bool auth_basic(mb_http_conn *conn, mb_slice value) {
    if (!mb_slice_has_prefix_ci_lit(value, "Basic")) {
        return false;
    }
    value.ptr += strlen("Basic");
    value.len -= strlen("Basic");
    value = mb_http_trim_ows(value);
    mb_buf decoded = {0};
    bool ok = false;
    if (value.len != 0 && base64_decode(value, &decoded)) {
        const void *colonp = memchr(decoded.ptr, ':', decoded.len);
        if (colonp != NULL) {
            const uint8_t *colon = colonp;
            const mb_slice user = {.ptr = decoded.ptr, .len = (size_t)(colon - decoded.ptr)};
            const mb_slice pass = {.ptr = colon + 1, .len = decoded.len - user.len - 1};
            ok = mb_auth_user_pass_matches(&conn->server->auth, user, pass);
        }
    }
    mb_buf_free(&decoded);
    return ok;
}

static bool request_authorized(mb_http_conn *conn, const mb_http_request *req) {
    if (conn->server->auth.mode == MB_AUTH_NONE) {
        return true;
    }
    if (!req->has_authorization) {
        return false;
    }
    if (conn->server->auth.mode == MB_AUTH_TOKEN) {
        return auth_bearer(conn, req->authorization);
    }
    if (conn->server->auth.mode == MB_AUTH_USER_PASS) {
        return auth_basic(conn, req->authorization);
    }
    return false;
}

static const char *auth_header(const mb_server *server) {
    if (server->auth.mode == MB_AUTH_USER_PASS) {
        return "WWW-Authenticate: Basic realm=\"monoblok\"\r\n";
    }
    if (server->auth.mode == MB_AUTH_TOKEN) {
        return "WWW-Authenticate: Bearer\r\n";
    }
    return NULL;
}

static void handle_latest(mb_http_conn *conn, const mb_http_request *req) {
    if (!mb_http_decode_subject_path(req->target, "/latest/", false, &conn->subject)) {
        respond_and_close(conn, 400, "Bad Request", NULL, "invalid latest path\n");
        return;
    }
    const mb_slice subject = {.ptr = conn->subject.ptr, .len = conn->subject.len};
    if (mb_router_subject_has_lvc_prefix(subject)) {
        respond_and_close(conn, 400, "Bad Request", NULL, "use the unprefixed subject\n");
        return;
    }
    if (!conn->server->lvc_enabled) {
        respond_and_close(conn, 409, "Conflict", NULL, "$LVC is disabled\n");
        return;
    }
    mb_slice payload = {0};
    if (!mb_router_lvc_latest(&conn->server->router, subject, &payload)) {
        respond_and_close(conn, 404, "Not Found", NULL, "not cached\n");
        return;
    }
    respond_bytes_and_close(conn, 200, "OK", "Cache-Control: no-cache\r\n", "text/plain; charset=utf-8", payload);
}

static void handle_get(mb_http_conn *conn, const mb_http_request *req) {
    if (req->has_content_length && req->content_length != 0) {
        respond_and_close(conn, 400, "Bad Request", NULL, "GET body is not supported\n");
        return;
    }
    if (!request_authorized(conn, req)) {
        respond_and_close(conn, 401, "Unauthorized", auth_header(conn->server), "unauthorized\n");
        return;
    }
    if (mb_slice_has_prefix(req->target, "/latest/", strlen("/latest/"))) {
        handle_latest(conn, req);
        return;
    }
    if (!mb_slice_has_prefix(req->target, "/sub/", strlen("/sub/"))) {
        respond_and_close(conn, 400, "Bad Request", NULL, "invalid GET path\n");
        return;
    }
    if (!mb_http_decode_subject_path(req->target, "/sub/", true, &conn->subject)) {
        respond_and_close(conn, 400, "Bad Request", NULL, "invalid subscription path\n");
        return;
    }
    const mb_slice subject = {.ptr = conn->subject.ptr, .len = conn->subject.len};
    if (mb_router_subject_has_lvc_prefix(subject) && !conn->server->lvc_enabled) {
        respond_and_close(conn, 409, "Conflict", NULL, "$LVC is disabled\n");
        return;
    }
    conn->sse = true;
    conn->router_conn.msg_len_fn = sse_msg_len;
    conn->router_conn.write_msg_fn = sse_write_msg;
    conn->router_conn.write_msg_ctx = conn;
    if (!append_lit(&conn->router_conn.out,
                    "HTTP/1.1 200 OK\r\n"
                    MB_HTTP_SERVER_HEADER
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n")) {
        http_conn_begin_close(conn);
        return;
    }
    const mb_slice sid = {.ptr = (const uint8_t *)"sse", .len = 3};
    if (!mb_router_subscribe(&conn->server->router, &conn->router_conn, subject, sid)) {
        mb_buf_clear(&conn->router_conn.out);
        conn->sse = false;
        respond_and_close(conn, 500, "Internal Server Error", NULL, "subscribe failed\n");
        return;
    }
    mb_buf_clear(&conn->rx);
    kick_write(conn);
}

static void handle_post(mb_http_conn *conn, const mb_http_request *req, mb_slice body) {
    if (!request_authorized(conn, req)) {
        respond_and_close(conn, 401, "Unauthorized", auth_header(conn->server), "unauthorized\n");
        return;
    }
    if (!req->has_content_type || !mb_http_content_type_allowed(req->content_type)) {
        respond_and_close(conn, 415, "Unsupported Media Type", NULL, "use text/plain or application/json\n");
        return;
    }
    if (!mb_http_decode_subject_path(req->target, "/pub/", false, &conn->subject)) {
        respond_and_close(conn, 400, "Bad Request", NULL, "invalid publish path\n");
        return;
    }
    if (slice_has_nul(body)) {
        respond_and_close(conn, 400, "Bad Request", NULL, "binary payloads are not supported\n");
        return;
    }
    const mb_client_publish_status status = mb_server_client_publish(conn->server,
                                                                     (mb_slice){.ptr = conn->subject.ptr, .len = conn->subject.len},
                                                                     body, (mb_slice){0});
    switch (status) {
    case MB_CLIENT_PUBLISH_OK:
        respond_and_close(conn, 202, "Accepted", NULL, NULL);
        break;
    case MB_CLIENT_PUBLISH_DISABLED:
        respond_and_close(conn, 403, "Forbidden", NULL, "client publish disabled\n");
        break;
    case MB_CLIENT_PUBLISH_LVC_READ_ONLY:
        respond_and_close(conn, 409, "Conflict", NULL, "$LVC is read-only\n");
        break;
    case MB_CLIENT_PUBLISH_STATS_READ_ONLY:
        respond_and_close(conn, 409, "Conflict", NULL, "$STATS is read-only\n");
        break;
    case MB_CLIENT_PUBLISH_ROUTER_FAILED:
        respond_and_close(conn, 500, "Internal Server Error", NULL, "publish failed\n");
        break;
    case MB_CLIENT_PUBLISH_PATCHBAY_FAILED:
        respond_and_close(conn, 500, "Internal Server Error", NULL, "patchbay failed\n");
        break;
    }
}

static void process_request(mb_http_conn *conn) {
    mb_http_request req = {0};
    mb_http_error err = {0};
    switch (mb_http_parse_request(conn->rx.ptr, conn->rx.len, &req, &err)) {
    case MB_HTTP_PARSE_NEED_MORE:
        return;
    case MB_HTTP_PARSE_ERROR:
        respond_and_close(conn, err.status, err.reason, NULL, err.message);
        return;
    case MB_HTTP_PARSE_OK:
        break;
    }
    if (req.method == MB_HTTP_GET) {
        handle_get(conn, &req);
        return;
    }
    handle_post(conn, &req, req.body);
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    mb_http_conn *conn = stream->data;
    if (nread < 0) {
        (void)buf;
        http_conn_begin_close(conn);
        return;
    }
    if (nread == 0) {
        (void)buf;
        return;
    }
    if (conn->sse) {
        http_conn_begin_close(conn);
        return;
    }
    const size_t max_rx = (size_t)MB_HTTP_MAX_HEADER + (size_t)MB_MAX_PAYLOAD;
    if ((size_t)nread > max_rx || conn->rx.len > max_rx - (size_t)nread) {
        respond_and_close(conn, 413, "Payload Too Large", NULL, "request too large\n");
        return;
    }
    if (!mb_buf_append(&conn->rx, buf->base, (size_t)nread)) {
        http_conn_begin_close(conn);
        return;
    }
    process_request(conn);
}

static bool http_conn_create(mb_server *server, uv_stream_t *listener) {
    mb_http_conn *conn = calloc(1, sizeof *conn);
    if (conn == NULL) {
        return false;
    }
    conn->server = server;
    conn->client_id = server->next_client_id;
    server->next_client_id += 1;
    conn->router_conn.kick_fn = kick_write;
    conn->router_conn.kick_ctx = conn;
    conn->router_conn.close_fn = close_from_router;
    conn->router_conn.close_ctx = conn;

    if (uv_tcp_init(&server->loop, &conn->tcp) != 0) {
        free(conn);
        return false;
    }
    conn->tcp.data = conn;
    if (uv_accept(listener, (uv_stream_t *)&conn->tcp) != 0) {
        uv_close((uv_handle_t *)&conn->tcp, close_cb);
        return false;
    }
    conn->next = server->http_conns;
    if (server->http_conns != NULL) {
        server->http_conns->prev = conn;
    }
    server->http_conns = conn;
    conn->counted = true;
    server->conn_count += 1;

    const int rc = uv_read_start((uv_stream_t *)&conn->tcp, alloc_cb, read_cb);
    if (rc != 0) {
        http_conn_begin_close(conn);
        return false;
    }
    return true;
}

static void close_dropped_connection(uv_handle_t *handle) {
    free(handle);
}

static void drop_pending_connection(mb_server *server, uv_stream_t *listener) {
    uv_tcp_t *drop = calloc(1, sizeof *drop);
    if (drop == NULL) {
        return;
    }
    if (uv_tcp_init(&server->loop, drop) != 0) {
        free(drop);
        return;
    }
    if (uv_accept(listener, (uv_stream_t *)drop) == 0) {
        uv_close((uv_handle_t *)drop, close_dropped_connection);
        return;
    }
    uv_close((uv_handle_t *)drop, close_dropped_connection);
}

static void on_connection(uv_stream_t *listener, int status) {
    mb_server *server = listener->data;
    if (status < 0) {
        fprintf(stderr, "http accept error: %s\n", uv_strerror(status));
        return;
    }
    if (server->closing) {
        return;
    }
    if (server->conn_count >= MB_MAX_CONNECTIONS) {
        fprintf(stderr, "warn: connection cap reached (%zu), dropping http client\n", (size_t)MB_MAX_CONNECTIONS);
        drop_pending_connection(server, listener);
        return;
    }
    if (!http_conn_create(server, listener)) {
        fprintf(stderr, "failed to create http connection\n");
    }
}

bool mb_http_listen(mb_server *server, const char *host, unsigned int port) {
    if (port == 0) {
        return true;
    }
    if (server->http_listener_started) {
        return true;
    }
    if (!server->http_listener_initialized) {
        if (uv_tcp_init(&server->loop, &server->http_listener) != 0) {
            return false;
        }
        server->http_listener.data = server;
        server->http_listener_initialized = true;
    }
    struct sockaddr_in addr;
    const int ip_rc = uv_ip4_addr(host, (int)port, &addr);
    if (ip_rc != 0) {
        fprintf(stderr, "invalid http address %s:%u: %s\n", host, port, uv_strerror(ip_rc));
        return false;
    }
    const int bind_rc = uv_tcp_bind(&server->http_listener, (const struct sockaddr *)&addr, 0);
    if (bind_rc != 0) {
        fprintf(stderr, "http bind failed: %s\n", uv_strerror(bind_rc));
        return false;
    }
    const int listen_rc = uv_listen((uv_stream_t *)&server->http_listener, 128, on_connection);
    if (listen_rc != 0) {
        fprintf(stderr, "http listen failed: %s\n", uv_strerror(listen_rc));
        return false;
    }
    server->http_host = host;
    server->http_port = port;
    server->http_listener_started = true;
    fprintf(stderr, "info: http: listening on %s:%u\n", host, port);
    return true;
}

void mb_http_close(mb_server *server) {
    while (server->http_conns != NULL) {
        http_conn_begin_close(server->http_conns);
    }
    if (server->http_listener_initialized && !uv_is_closing((uv_handle_t *)&server->http_listener)) {
        uv_close((uv_handle_t *)&server->http_listener, NULL);
    }
}
