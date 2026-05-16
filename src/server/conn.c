#include "conn.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    mb_buf_free(&conn->rx);
    mb_buf_free(&conn->router_conn.out);
    mb_buf_free(&conn->in_flight);
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

static size_t pending_bytes(const mb_conn *conn) {
    if (conn->router_conn.out.len > SIZE_MAX - conn->in_flight.len) {
        return SIZE_MAX;
    }
    return conn->router_conn.out.len + conn->in_flight.len;
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
        fprintf(stderr, "trace: conn %" PRIu64 " PUB %.*s %zu\n", conn->client_id,
                (int)op.subject.len, op.subject.ptr, op.payload.len);
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

static void handle_op(mb_conn *conn, mb_op op) {
    trace_op(conn, op);
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
        bool published = false;
        if (mb_router_subject_has_lvc_prefix(op.subject)) {
            write_err_or_close(conn, "$LVC is read-only");
        } else if (!mb_router_publish(&conn->server->router, op.subject, op.payload)) {
            write_err_or_close(conn, "Publish Failed");
        } else {
            published = true;
        }
        if (!published) {
            break;
        }
        uv_update_time(&conn->server->loop);
        if (!pb_program_eval_publish(conn->server->program, &conn->server->router, op.subject, op.payload,
                                     uv_now(&conn->server->loop), mb_wall_clock_ms())) {
            write_err_or_close(conn, "Patchbay Failed");
        }
        mb_server_reschedule_patchbay_clock(conn->server);
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

static void write_cb(uv_write_t *req, int status) {
    mb_conn *conn = req->data;
    if (status < 0 || conn->closing) {
        conn->write_pending = false;
        mb_conn_begin_close(conn);
        return;
    }

    mb_buf_clear(&conn->in_flight);
    conn->write_pending = false;
    mb_conn_kick_write(conn);
}

void mb_conn_kick_write(void *ctx) {
    mb_conn *conn = ctx;
    if (conn->closing) {
        return;
    }
    if (close_if_slow_consumer(conn) || conn->write_pending || conn->router_conn.out.len == 0) {
        return;
    }

    mb_buf_swap(&conn->router_conn.out, &conn->in_flight);
    if (conn->in_flight.len > UINT_MAX) {
        mb_conn_begin_close(conn);
        return;
    }

    uv_buf_t uvb = uv_buf_init((char *)conn->in_flight.ptr, (unsigned int)conn->in_flight.len);
    conn->write_req.data = conn;
    conn->write_pending = true;
    const int rc = uv_write(&conn->write_req, (uv_stream_t *)&conn->tcp, &uvb, 1, write_cb);
    if (rc != 0) {
        conn->write_pending = false;
        mb_conn_begin_close(conn);
    }
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

    if (uv_read_start((uv_stream_t *)&conn->tcp, alloc_cb, read_cb) != 0) {
        mb_conn_begin_close(conn);
        return false;
    }
    mb_conn_kick_write(conn);
    return true;
}
