#include "conn.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <yyjson.h>

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void write_cb(uv_write_t *req, int status);

// libuv read allocator: reuse the per-connection buffer and cap read chunks.
static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    mb_conn *conn = handle->data;
    const size_t n = suggested_size < MB_READ_CHUNK ? suggested_size : MB_READ_CHUNK;
    buf->base = (char *)conn->read_buf;
    buf->len = n;
}

static void unlink_conn(mb_conn *conn) {
    if (conn->prev == NULL && conn->next == NULL && conn->server->conns != conn) {
        return;
    }
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        conn->server->conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    conn->prev = NULL;
    conn->next = NULL;
}

static void close_cb(uv_handle_t *handle) {
    mb_conn *conn = handle->data;
    if (conn->counted && conn->server->conn_count > 0) {
        conn->server->conn_count -= 1;
    }
    if (conn->tls != NULL) {
        SSL_free(conn->tls);
    }
    mb_buf_free(&conn->rx);
    mb_buf_free(&conn->router_conn.out);
    mb_buf_free(&conn->in_flight);
    mb_buf_free(&conn->tls_plain);
    mb_buf_free(&conn->tls_out);
    free(conn);
}

void mb_conn_begin_close(mb_conn *conn) {
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

static void close_conn_from_router(void *ctx) {
    mb_conn_begin_close(ctx);
}

static size_t add_pending(size_t a, size_t b) {
    if (a > SIZE_MAX - b) {
        return SIZE_MAX;
    }
    return a + b;
}

static size_t pending_bytes(const mb_conn *conn) {
    size_t pending = add_pending(conn->router_conn.out.len, conn->in_flight.len);
    pending = add_pending(pending, conn->tls_out.len);
    if (conn->tls_plain.len > conn->tls_plain_off) {
        pending = add_pending(pending, conn->tls_plain.len - conn->tls_plain_off);
    }
    return pending;
}

static bool close_if_slow_consumer(mb_conn *conn) {
    const size_t pending = pending_bytes(conn);
    if (pending <= MB_MAX_PENDING) {
        return false;
    }
    fprintf(stderr, "warn: conn %" PRIu64 " slow consumer, closing: pending=%zuB cap=%zuB\n",
            conn->client_id, pending, (size_t)MB_MAX_PENDING);
    mb_conn_begin_close(conn);
    return true;
}

static void trace_op(mb_conn *conn, mb_op op) {
    if (!conn->server->trace) {
        return;
    }
    switch (op.kind) {
    case MB_OP_CONNECT:
        fprintf(stderr, "trace: conn %" PRIu64 " CONNECT\n", conn->client_id);
        break;
    case MB_OP_PING:
        fprintf(stderr, "trace: conn %" PRIu64 " PING\n", conn->client_id);
        break;
    case MB_OP_SUB:
        if (op.queue.len != 0) {
            fprintf(stderr, "trace: conn %" PRIu64 " SUB %.*s %.*s %.*s\n", conn->client_id,
                    (int)op.subject.len, op.subject.ptr, (int)op.queue.len, op.queue.ptr,
                    (int)op.sid.len, op.sid.ptr);
        } else {
            fprintf(stderr, "trace: conn %" PRIu64 " SUB %.*s %.*s\n", conn->client_id,
                    (int)op.subject.len, op.subject.ptr, (int)op.sid.len, op.sid.ptr);
        }
        break;
    case MB_OP_UNSUB:
        fprintf(stderr, "trace: conn %" PRIu64 " UNSUB %.*s\n", conn->client_id,
                (int)op.sid.len, op.sid.ptr);
        break;
    case MB_OP_PUB:
        if (op.reply_to.len != 0) {
            fprintf(stderr, "trace: conn %" PRIu64 " PUB %.*s %.*s %zu\n", conn->client_id,
                    (int)op.subject.len, op.subject.ptr, (int)op.reply_to.len, op.reply_to.ptr, op.payload.len);
        } else {
            fprintf(stderr, "trace: conn %" PRIu64 " PUB %.*s %zu\n", conn->client_id,
                    (int)op.subject.len, op.subject.ptr, op.payload.len);
        }
        break;
    }
}

static bool op_mutates_server_state(mb_op op) {
    return op.kind == MB_OP_SUB || op.kind == MB_OP_UNSUB || op.kind == MB_OP_PUB;
}

static void write_err_or_close(mb_conn *conn, const char *msg) {
    if (!mb_write_err(&conn->router_conn.out, msg)) {
        mb_conn_begin_close(conn);
    }
}

static bool connect_auth_matches(mb_conn *conn, mb_slice json) {
    if (conn->server->auth.mode == MB_AUTH_NONE) {
        return true;
    }
    if (json.len == 0) {
        return false;
    }

    yyjson_read_err err = {0};
    yyjson_doc *doc = yyjson_read_opts((char *)(void *)json.ptr, json.len, YYJSON_READ_NOFLAG, NULL, &err);
    if (doc == NULL) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = false;
    if (yyjson_is_obj(root)) {
        if (conn->server->auth.mode == MB_AUTH_TOKEN) {
            yyjson_val *token = yyjson_obj_get(root, "auth_token");
            ok = yyjson_is_str(token) &&
                 mb_auth_token_matches(&conn->server->auth,
                                       (mb_slice){.ptr = (const uint8_t *)yyjson_get_str(token), .len = yyjson_get_len(token)});
        } else if (conn->server->auth.mode == MB_AUTH_USER_PASS) {
            yyjson_val *user = yyjson_obj_get(root, "user");
            yyjson_val *pass = yyjson_obj_get(root, "pass");
            ok = yyjson_is_str(user) && yyjson_is_str(pass) &&
                 mb_auth_user_pass_matches(&conn->server->auth,
                                           (mb_slice){.ptr = (const uint8_t *)yyjson_get_str(user), .len = yyjson_get_len(user)},
                                           (mb_slice){.ptr = (const uint8_t *)yyjson_get_str(pass), .len = yyjson_get_len(pass)});
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

static void auth_violation(mb_conn *conn) {
    write_err_or_close(conn, "Authorization Violation");
    mb_conn_begin_close(conn);
}

static void handle_op(mb_conn *conn, mb_op op) {
    trace_op(conn, op);
    if (!conn->authorized) {
        if (op.kind != MB_OP_CONNECT || !connect_auth_matches(conn, op.connect)) {
            auth_violation(conn);
            return;
        }
        conn->authorized = true;
        return;
    }
    if (conn->server->closing && op_mutates_server_state(op)) {
        write_err_or_close(conn, "Server Shutting Down");
        return;
    }
    switch (op.kind) {
    case MB_OP_CONNECT:
        break;
    case MB_OP_PING:
        if (!mb_write_pong(&conn->router_conn.out)) {
            mb_conn_begin_close(conn);
        }
        break;
    case MB_OP_SUB:
        if (mb_router_subject_has_lvc_prefix(op.subject) && !conn->server->lvc_enabled) {
            write_err_or_close(conn, "$LVC is disabled");
        } else if (!mb_router_subscribe_queue(&conn->server->router, &conn->router_conn, op.subject, op.queue, op.sid)) {
            write_err_or_close(conn, "Subscribe Failed");
        }
        break;
    case MB_OP_UNSUB:
        mb_router_unsubscribe(&conn->server->router, &conn->router_conn, op.sid,
                              op.max_msgs, op.has_max_msgs);
        break;
    case MB_OP_PUB: {
        const mb_client_publish_status status = mb_server_client_publish(conn->server, op.subject, op.payload, op.reply_to);
        if (status == MB_CLIENT_PUBLISH_DISABLED) {
            write_err_or_close(conn, "Client Publish Disabled");
        } else if (status == MB_CLIENT_PUBLISH_LVC_READ_ONLY) {
            write_err_or_close(conn, "$LVC is read-only");
        } else if (status == MB_CLIENT_PUBLISH_STATS_READ_ONLY) {
            write_err_or_close(conn, "$STATS is read-only");
        } else if (status == MB_CLIENT_PUBLISH_ROUTER_FAILED) {
            write_err_or_close(conn, "Publish Failed");
        } else if (status == MB_CLIENT_PUBLISH_PATCHBAY_FAILED) {
            write_err_or_close(conn, "Patchbay Failed");
        }
        break;
    }
    }
}

static void set_client_ip(mb_conn *conn) {
    struct sockaddr_storage addr = {0};
    int len = sizeof addr;
    if (uv_tcp_getpeername(&conn->tcp, (struct sockaddr *)&addr, &len) != 0) {
        conn->client_ip[0] = '\0';
        return;
    }
    if (addr.ss_family == AF_INET) {
        (void)uv_ip4_name((const struct sockaddr_in *)&addr, conn->client_ip, sizeof conn->client_ip);
    } else if (addr.ss_family == AF_INET6) {
        (void)uv_ip6_name((const struct sockaddr_in6 *)&addr, conn->client_ip, sizeof conn->client_ip);
    } else {
        conn->client_ip[0] = '\0';
    }
}

static void process_rx(mb_conn *conn) {
    size_t cursor = 0;
    while (cursor < conn->rx.len) {
        const mb_parse_result result = mb_parse_client_op(conn->rx.ptr + cursor, conn->rx.len - cursor);
        if (result.status == MB_PARSE_NEED_MORE) {
            break;
        }
        if (result.status != MB_PARSE_OK) {
            if (result.status == MB_PARSE_CONTROL_LINE_TOO_LONG ||
                result.status == MB_PARSE_MALFORMED ||
                result.status == MB_PARSE_PAYLOAD_TOO_LARGE) {
                write_err_or_close(conn, "Protocol Violation");
                mb_conn_begin_close(conn);
                return;
            }
            write_err_or_close(conn,
                               result.status == MB_PARSE_UNKNOWN ? "Unknown Protocol Operation" : "Invalid Operation");
            if (conn->closing) {
                return;
            }
            const void *nlp = memchr(conn->rx.ptr + cursor, '\n', conn->rx.len - cursor);
            if (nlp == NULL) {
                break;
            }
            const uint8_t *nl = nlp;
            cursor = (size_t)(nl - conn->rx.ptr) + 1;
            continue;
        }
        handle_op(conn, result.op);
        if (conn->closing) {
            return;
        }
        cursor += result.consumed;
    }
    if (cursor > 0) {
        mb_buf_consume(&conn->rx, cursor);
    }
    if (conn->rx.len > MB_MAX_RX) {
        mb_conn_begin_close(conn);
    }
}

static bool start_reading(mb_conn *conn) {
    const int rc = uv_read_start((uv_stream_t *)&conn->tcp, alloc_cb, read_cb);
    if (rc != 0) {
        mb_conn_begin_close(conn);
        return false;
    }
    return true;
}

static void tls_log_conn_error(mb_conn *conn, const char *what, int ssl_error) {
    char errbuf[256];
    const unsigned long code = ERR_get_error();
    if (code != 0) {
        ERR_error_string_n(code, errbuf, sizeof errbuf);
        fprintf(stderr, "warn: conn %" PRIu64 " tls %s failed: %s (ssl err=%d)\n",
                conn->client_id, what, errbuf, ssl_error);
    } else {
        fprintf(stderr, "warn: conn %" PRIu64 " tls %s failed (ssl err=%d)\n",
                conn->client_id, what, ssl_error);
    }
}

static bool tls_drain_wbio(mb_conn *conn) {
    BIO *wbio = SSL_get_wbio(conn->tls);
    uint8_t tmp[4096];
    for (;;) {
        const int n = BIO_read(wbio, tmp, (int)sizeof tmp);
        if (n > 0) {
            if (!mb_buf_append(&conn->tls_out, tmp, (size_t)n)) {
                mb_conn_begin_close(conn);
                return false;
            }
            continue;
        }
        if (n == 0 || BIO_should_retry(wbio)) {
            return true;
        }
        tls_log_conn_error(conn, "drain write bio", 0);
        mb_conn_begin_close(conn);
        return false;
    }
}

static bool tls_drain_plain(mb_conn *conn) {
    uint8_t plain[MB_READ_CHUNK];
    for (;;) {
        const int n = SSL_read(conn->tls, plain, (int)sizeof plain);
        const int ssl_error = n > 0 ? SSL_ERROR_NONE : SSL_get_error(conn->tls, n);
        if (!tls_drain_wbio(conn)) {
            return false;
        }
        if (n > 0) {
            if (!mb_buf_append(&conn->rx, plain, (size_t)n)) {
                mb_conn_begin_close(conn);
                return false;
            }
            process_rx(conn);
            if (conn->closing) {
                return false;
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            return true;
        }
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            mb_conn_begin_close(conn);
            return false;
        }
        tls_log_conn_error(conn, "read", ssl_error);
        mb_conn_begin_close(conn);
        return false;
    }
}

static bool tls_drive_handshake(mb_conn *conn) {
    if (conn->tls_state != MB_CONN_TLS_HANDSHAKE) {
        return true;
    }
    const int rc = SSL_do_handshake(conn->tls);
    const int ssl_error = rc == 1 ? SSL_ERROR_NONE : SSL_get_error(conn->tls, rc);
    if (!tls_drain_wbio(conn)) {
        return false;
    }
    if (rc == 1) {
        conn->tls_state = MB_CONN_TLS_READY;
        if (conn->server->trace) {
            fprintf(stderr, "trace: conn %" PRIu64 " TLS ready\n", conn->client_id);
        }
        return tls_drain_plain(conn);
    }
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return true;
    }
    tls_log_conn_error(conn, "handshake", ssl_error);
    mb_conn_begin_close(conn);
    return false;
}

static bool tls_feed_encrypted(mb_conn *conn, const uint8_t *data, size_t len) {
    BIO *rbio = SSL_get_rbio(conn->tls);
    while (len > 0) {
        const int chunk = len > (size_t)INT_MAX ? INT_MAX : (int)len;
        const int n = BIO_write(rbio, data, chunk);
        if (n <= 0) {
            tls_log_conn_error(conn, "feed read bio", 0);
            mb_conn_begin_close(conn);
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    if (conn->tls_state == MB_CONN_TLS_HANDSHAKE) {
        return tls_drive_handshake(conn);
    }
    if (conn->tls_state == MB_CONN_TLS_READY) {
        return tls_drain_plain(conn);
    }
    return true;
}

static bool tls_start(mb_conn *conn) {
    SSL *ssl = SSL_new(conn->server->tls_ctx);
    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    if (ssl == NULL || rbio == NULL || wbio == NULL) {
        BIO_free(rbio);
        BIO_free(wbio);
        SSL_free(ssl);
        tls_log_conn_error(conn, "allocate", 0);
        mb_conn_begin_close(conn);
        return false;
    }
    BIO_set_mem_eof_return(rbio, -1);
    SSL_set_bio(ssl, rbio, wbio);
    SSL_set_accept_state(ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    conn->tls = ssl;
    conn->tls_state = MB_CONN_TLS_HANDSHAKE;
    if (conn->server->trace) {
        fprintf(stderr, "trace: conn %" PRIu64 " TLS handshake\n", conn->client_id);
    }
    if (!start_reading(conn)) {
        return false;
    }
    if (!tls_drive_handshake(conn)) {
        return false;
    }
    mb_conn_kick_write(conn);
    return true;
}

static bool tls_encrypt_pending(mb_conn *conn) {
    if (conn->tls_state != MB_CONN_TLS_READY) {
        return true;
    }
    for (;;) {
        if (conn->tls_plain_off == conn->tls_plain.len) {
            mb_buf_clear(&conn->tls_plain);
            conn->tls_plain_off = 0;
            if (conn->router_conn.out.len == 0) {
                return true;
            }
            mb_buf_swap(&conn->router_conn.out, &conn->tls_plain);
        }

        const size_t pending = conn->tls_plain.len - conn->tls_plain_off;
        const int chunk = pending > (size_t)INT_MAX ? INT_MAX : (int)pending;
        const int n = SSL_write(conn->tls, conn->tls_plain.ptr + conn->tls_plain_off, chunk);
        const int ssl_error = n > 0 ? SSL_ERROR_NONE : SSL_get_error(conn->tls, n);
        if (!tls_drain_wbio(conn)) {
            return false;
        }
        if (n > 0) {
            conn->tls_plain_off += (size_t)n;
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            return true;
        }
        tls_log_conn_error(conn, "write", ssl_error);
        mb_conn_begin_close(conn);
        return false;
    }
}

static bool write_socket_from(mb_conn *conn, mb_buf *src) {
    if (conn->closing || conn->write_pending || src->len == 0) {
        return true;
    }

    mb_buf_swap(src, &conn->in_flight);
    if (conn->in_flight.len > UINT_MAX) {
        mb_conn_begin_close(conn);
        return false;
    }

    uv_buf_t uvb = uv_buf_init((char *)conn->in_flight.ptr, (unsigned int)conn->in_flight.len);
    conn->write_req.data = conn;
    conn->write_pending = true;
    const int rc = uv_write(&conn->write_req, (uv_stream_t *)&conn->tcp, &uvb, 1, write_cb);
    if (rc != 0) {
        conn->write_pending = false;
        mb_conn_begin_close(conn);
        return false;
    }
    return true;
}

static void write_cb(uv_write_t *req, int status) {
    mb_conn *conn = req->data;
    if (status < 0 || conn->closing) {
        conn->write_pending = false;
        mb_conn_begin_close(conn);
        return;
    }

    mb_buf_clear(&conn->in_flight);
    conn->write_pending = false;
    if (conn->tls_state == MB_CONN_TLS_INFO) {
        (void)tls_start(conn);
        return;
    }
    if (conn->tls_state == MB_CONN_TLS_HANDSHAKE) {
        if (tls_drive_handshake(conn) && !conn->closing) {
            mb_conn_kick_write(conn);
        }
        return;
    }
    mb_conn_kick_write(conn);
}

void mb_conn_kick_write(void *ctx) {
    mb_conn *conn = ctx;
    if (conn->closing) {
        return;
    }
    if (close_if_slow_consumer(conn) || conn->write_pending) {
        return;
    }

    if (conn->tls_state == MB_CONN_TLS_OFF) {
        (void)write_socket_from(conn, &conn->router_conn.out);
        return;
    }
    if (conn->tls_state == MB_CONN_TLS_INFO) {
        (void)write_socket_from(conn, &conn->router_conn.out);
        return;
    }
    if (conn->tls_state == MB_CONN_TLS_HANDSHAKE) {
        (void)write_socket_from(conn, &conn->tls_out);
        return;
    }
    if (!tls_encrypt_pending(conn)) {
        return;
    }
    (void)write_socket_from(conn, &conn->tls_out);
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    mb_conn *conn = stream->data;
    if (nread < 0) {
        (void)buf;
        mb_conn_begin_close(conn);
        return;
    }
    if (nread == 0) {
        (void)buf;
        return;
    }
    if (conn->tls_state == MB_CONN_TLS_HANDSHAKE || conn->tls_state == MB_CONN_TLS_READY) {
        if (tls_feed_encrypted(conn, (const uint8_t *)buf->base, (size_t)nread) && !conn->closing) {
            mb_conn_kick_write(conn);
        }
        return;
    }
    if (conn->tls_state == MB_CONN_TLS_INFO) {
        mb_conn_begin_close(conn);
        return;
    }
    if (!mb_buf_append(&conn->rx, buf->base, (size_t)nread)) {
        mb_conn_begin_close(conn);
        return;
    }
    process_rx(conn);
    if (!conn->closing) {
        mb_conn_kick_write(conn);
    }
}

bool mb_conn_create(mb_server *server, uv_stream_t *listener) {
    mb_conn *conn = calloc(1, sizeof *conn);
    if (conn == NULL) {
        return false;
    }
    conn->server = server;
    conn->client_id = server->next_client_id;
    server->next_client_id += 1;
    conn->router_conn.kick_fn = mb_conn_kick_write;
    conn->router_conn.kick_ctx = conn;
    conn->router_conn.close_fn = close_conn_from_router;
    conn->router_conn.close_ctx = conn;
    conn->tls_state = server->tls_enabled ? MB_CONN_TLS_INFO : MB_CONN_TLS_OFF;
    conn->authorized = server->auth.mode == MB_AUTH_NONE;

    if (uv_tcp_init(&server->loop, &conn->tcp) != 0) {
        free(conn);
        return false;
    }
    conn->tcp.data = conn;
    if (uv_accept(listener, (uv_stream_t *)&conn->tcp) != 0) {
        uv_close((uv_handle_t *)&conn->tcp, close_cb);
        return false;
    }
    set_client_ip(conn);
    const mb_info info = {
        .server_id = server->server_id,
        .host = server->host,
        .client_ip = conn->client_ip,
        .port = server->port,
        .client_id = conn->client_id,
        .auth_required = server->auth.mode != MB_AUTH_NONE,
        .tls_required = server->tls_enabled,
    };
    if (!mb_write_info(&conn->router_conn.out, &info)) {
        mb_conn_begin_close(conn);
        return false;
    }

    conn->next = server->conns;
    if (server->conns != NULL) {
        server->conns->prev = conn;
    }
    server->conns = conn;
    conn->counted = true;
    server->conn_count += 1;

    if (!server->tls_enabled && !start_reading(conn)) {
        mb_conn_begin_close(conn);
        return false;
    }
    mb_conn_kick_write(conn);
    return true;
}
