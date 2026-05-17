#include "array.h"
#include "fs.h"
#include "proto.h"
#include "router.h"
#include "test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mb_slice lit(const char *s) {
    return (mb_slice){.ptr = (const uint8_t *)s, .len = strlen(s)};
}

static void check_buf_eq(const mb_buf *buf, const char *want) {
    CHECK(buf->len == strlen(want));
    CHECK(memcmp(buf->ptr, want, buf->len) == 0);
}

static bool buf_contains(const mb_buf *buf, const char *needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= buf->len; i += 1) {
        if (memcmp(buf->ptr + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static size_t buf_count(const mb_buf *buf, const char *needle) {
    const size_t n = strlen(needle);
    size_t count = 0;
    for (size_t i = 0; i + n <= buf->len; i += 1) {
        if (memcmp(buf->ptr + i, needle, n) == 0) {
            count += 1;
        }
    }
    return count;
}

static void test_parse_ping(void) {
    const char *src = "PING\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_OK);
    CHECK(r.op.kind == MB_OP_PING);
    CHECK(r.consumed == strlen(src));
}

static void test_parse_sub(void) {
    const char *src = "SUB foo 1\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_OK);
    CHECK(r.op.kind == MB_OP_SUB);
    CHECK(r.op.subject.len == 3);
    CHECK(memcmp(r.op.subject.ptr, "foo", 3) == 0);
    CHECK(r.op.queue.len == 0);
    CHECK(r.op.sid.len == 1);
    CHECK(memcmp(r.op.sid.ptr, "1", 1) == 0);
}

static void test_parse_queue_sub(void) {
    const char *src = "SUB foo workers 1\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_OK);
    CHECK(r.op.kind == MB_OP_SUB);
    CHECK(r.op.subject.len == 3);
    CHECK(memcmp(r.op.subject.ptr, "foo", 3) == 0);
    CHECK(r.op.queue.len == 7);
    CHECK(memcmp(r.op.queue.ptr, "workers", 7) == 0);
    CHECK(r.op.sid.len == 1);
    CHECK(memcmp(r.op.sid.ptr, "1", 1) == 0);
}

static void test_parse_unsub(void) {
    const char *src = "UNSUB 1 2\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_OK);
    CHECK(r.op.kind == MB_OP_UNSUB);
    CHECK(r.op.sid.len == 1);
    CHECK(memcmp(r.op.sid.ptr, "1", 1) == 0);
    CHECK(r.op.has_max_msgs);
    CHECK(r.op.max_msgs == 2);
}

static void test_parse_connect(void) {
    const char *src = "CONNECT {\"verbose\":false}\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_OK);
    CHECK(r.op.kind == MB_OP_CONNECT);
    CHECK(r.consumed == strlen(src));
}

static void test_parse_pub(void) {
    const char *src = "PUB foo 2\r\nhi\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_OK);
    CHECK(r.op.kind == MB_OP_PUB);
    CHECK(r.op.subject.len == 3);
    CHECK(memcmp(r.op.subject.ptr, "foo", 3) == 0);
    CHECK(r.op.payload.len == 2);
    CHECK(memcmp(r.op.payload.ptr, "hi", 2) == 0);
    CHECK(r.consumed == strlen(src));
}

static void test_parse_fragmented_pub(void) {
    const char *src = "PUB foo 5\r\nhi";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_NEED_MORE);
}

static void test_parse_bad_command(void) {
    const char *src = "WAT\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_UNKNOWN);
}

static void test_parse_payload_too_large(void) {
    const char *src = "PUB foo 1048577\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_PAYLOAD_TOO_LARGE);
}

static void test_parse_control_line_too_long(void) {
    uint8_t src[MB_MAX_CONTROL_LINE + 2];
    memset(src, 'A', sizeof src);
    const mb_parse_result r = mb_parse_client_op(src, sizeof src);
    CHECK(r.status == MB_PARSE_CONTROL_LINE_TOO_LONG);
}

static void test_parse_rejects_control_bytes_in_tokens(void) {
    const char *src = "SUB foo\rbar 1\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_INVALID_ARGS);
}

static void test_parse_rejects_pub_wildcards(void) {
    const char *src = "PUB foo.* 1\r\nx\r\n";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_INVALID_ARGS);
}

static void test_parse_malformed_pub_trailer(void) {
    const char *src = "PUB foo 1\r\nxz";
    const mb_parse_result r = mb_parse_client_op((const uint8_t *)src, strlen(src));
    CHECK(r.status == MB_PARSE_MALFORMED);
}

static void test_write_msg(void) {
    mb_buf buf = {0};
    CHECK(mb_write_msg(&buf, lit("foo"), lit("1"), lit("hi")));
    check_buf_eq(&buf, "MSG foo 1 2\r\nhi\r\n");
    mb_buf_free(&buf);
}

static void test_buf_consume_and_swap(void) {
    mb_buf buf = {0};
    CHECK(mb_buf_append(&buf, "abcdef", 6));
    mb_buf_consume(&buf, 2);
    check_buf_eq(&buf, "cdef");
    mb_buf_consume(&buf, 99);
    CHECK(buf.len == 0);
    mb_buf_free(&buf);

    mb_buf a = {0};
    mb_buf b = {0};
    CHECK(mb_buf_append(&a, "left", 4));
    CHECK(mb_buf_append(&b, "right", 5));
    mb_buf_swap(&a, &b);
    check_buf_eq(&a, "right");
    check_buf_eq(&b, "left");
    mb_buf_free(&a);
    mb_buf_free(&b);
}

static void test_fs_read_write_helpers(void) {
    mb_buf buf = {0};
    CHECK(!mb_read_file(NULL, &buf));

    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-fs-%ld.txt", (long)getpid());
    remove(path);
    CHECK(!mb_read_file(path, &buf));

    CHECK(!mb_write_file_atomic(NULL, lit("x")));
    CHECK(!mb_write_file_atomic(path, (mb_slice){.ptr = NULL, .len = 1}));
    CHECK(mb_write_file_atomic(path, lit("hello")));
    CHECK(mb_read_file(path, &buf));
    check_buf_eq(&buf, "hello");

    mb_buf_free(&buf);
    remove(path);
}

static void test_array_reserve_helpers(void) {
    size_t cap = 0;
    int *items = NULL;
    CHECK(!mb_array_reserve(NULL, &cap, 1, sizeof items[0], 1));
    CHECK(!mb_array_reserve((void **)&items, NULL, 1, sizeof items[0], 1));
    CHECK(!mb_array_reserve((void **)&items, &cap, 1, 0, 1));
    CHECK(mb_array_reserve((void **)&items, &cap, 1, sizeof items[0], 0));
    CHECK(cap >= 1);
    items[0] = 7;
    CHECK(mb_array_reserve((void **)&items, &cap, 9, sizeof items[0], 0));
    CHECK(cap >= 9);
    CHECK(items[0] == 7);
    free(items);
}

static void test_write_info(void) {
    mb_buf buf = {0};
    const mb_info info = {
        .server_id = "MC0123",
        .host = "127.0.0.1",
        .client_ip = "127.0.0.1",
        .port = 4222,
        .client_id = 7,
    };
    CHECK(mb_write_info(&buf, &info));
    CHECK(buf.len > 5);
    CHECK(memcmp(buf.ptr, "INFO ", 5) == 0);
    CHECK(buf_contains(&buf, "\"server_id\":\"MC0123\""));
    CHECK(buf_contains(&buf, "\"client_id\":7"));
    CHECK(!buf_contains(&buf, "\"go\""));
    CHECK(buf.ptr[buf.len - 2] == '\r');
    CHECK(buf.ptr[buf.len - 1] == '\n');
    mb_buf_free(&buf);
}

static void test_router_one_sub(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    check_buf_eq(&conn.out, "MSG foo 1 2\r\nhi\r\n");
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_two_subs(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn a = {0};
    mb_router_conn b = {0};
    CHECK(mb_router_subscribe(&router, &a, lit("foo"), lit("1")));
    CHECK(mb_router_subscribe(&router, &b, lit("foo"), lit("2")));
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    check_buf_eq(&a.out, "MSG foo 1 2\r\nhi\r\n");
    check_buf_eq(&b.out, "MSG foo 2 2\r\nhi\r\n");
    mb_buf_free(&a.out);
    mb_buf_free(&b.out);
    mb_router_free(&router);
}

static void test_router_queue_group(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn normal = {0};
    mb_router_conn a = {0};
    mb_router_conn b = {0};
    CHECK(mb_router_subscribe(&router, &normal, lit("foo"), lit("1")));
    CHECK(mb_router_subscribe_queue(&router, &a, lit("foo"), lit("workers"), lit("2")));
    CHECK(mb_router_subscribe_queue(&router, &b, lit("foo"), lit("workers"), lit("3")));
    for (size_t i = 0; i < 20; i += 1) {
        CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    }
    CHECK(buf_count(&normal.out, "MSG foo ") == 20);
    CHECK(buf_count(&a.out, "MSG foo ") + buf_count(&b.out, "MSG foo ") == 20);
    mb_buf_free(&normal.out);
    mb_buf_free(&a.out);
    mb_buf_free(&b.out);
    mb_router_free(&router);
}

static void test_router_queue_group_by_filter(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn a = {0};
    mb_router_conn b = {0};
    CHECK(mb_router_subscribe_queue(&router, &a, lit("foo"), lit("workers"), lit("1")));
    CHECK(mb_router_subscribe_queue(&router, &b, lit(">"), lit("workers"), lit("2")));
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    check_buf_eq(&a.out, "MSG foo 1 2\r\nhi\r\n");
    check_buf_eq(&b.out, "MSG foo 2 2\r\nhi\r\n");
    mb_buf_free(&a.out);
    mb_buf_free(&b.out);
    mb_router_free(&router);
}

static void test_router_queue_auto_unsubscribe(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn a = {0};
    mb_router_conn b = {0};
    CHECK(mb_router_subscribe_queue(&router, &a, lit("foo"), lit("workers"), lit("1")));
    CHECK(mb_router_subscribe_queue(&router, &b, lit("foo"), lit("workers"), lit("2")));
    mb_router_unsubscribe(&router, &a, lit("1"), 1, true);
    mb_router_unsubscribe(&router, &b, lit("2"), 1, true);
    CHECK(mb_router_publish(&router, lit("foo"), lit("a")));
    CHECK(mb_router_publish(&router, lit("foo"), lit("b")));
    CHECK(router.sub_count == 0);
    CHECK(buf_count(&a.out, "MSG foo ") + buf_count(&b.out, "MSG foo ") == 2);
    mb_buf_free(&a.out);
    mb_buf_free(&b.out);
    mb_router_free(&router);
}

static void test_router_non_match(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    CHECK(mb_router_publish(&router, lit("bar"), lit("hi")));
    CHECK(conn.out.len == 0);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_wildcards(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn a = {0};
    mb_router_conn b = {0};
    mb_router_conn c = {0};
    CHECK(mb_router_subscribe(&router, &a, lit("sensors.*"), lit("1")));
    CHECK(mb_router_subscribe(&router, &b, lit("sensors.>"), lit("2")));
    CHECK(mb_router_subscribe(&router, &c, lit("*.temp"), lit("3")));
    CHECK(mb_router_publish(&router, lit("sensors.temp"), lit("31")));
    check_buf_eq(&a.out, "MSG sensors.temp 1 2\r\n31\r\n");
    check_buf_eq(&b.out, "MSG sensors.temp 2 2\r\n31\r\n");
    check_buf_eq(&c.out, "MSG sensors.temp 3 2\r\n31\r\n");
    mb_buf_clear(&a.out);
    mb_buf_clear(&b.out);
    mb_buf_clear(&c.out);

    CHECK(mb_router_publish(&router, lit("sensors.room.temp"), lit("32")));
    CHECK(a.out.len == 0);
    check_buf_eq(&b.out, "MSG sensors.room.temp 2 2\r\n32\r\n");
    CHECK(c.out.len == 0);
    mb_buf_free(&a.out);
    mb_buf_free(&b.out);
    mb_buf_free(&c.out);
    mb_router_free(&router);
}

static void test_router_lvc_replay(void) {
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[] = {lit(">")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_publish(&router, lit("foo"), lit("cached")));
    CHECK(mb_router_subscribe(&router, &conn, lit("$LVC.foo"), lit("11")));
    check_buf_eq(&conn.out, "MSG $LVC.foo 11 6\r\ncached\r\n");
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_lvc_live_wildcard(void) {
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[] = {lit("sensors.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("$LVC.sensors.*"), lit("7")));
    CHECK(mb_router_publish(&router, lit("sensors.temp"), lit("31")));
    check_buf_eq(&conn.out, "MSG $LVC.sensors.temp 7 2\r\n31\r\n");
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_lvc_rejects_writes(void) {
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[] = {lit(">")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    CHECK(!mb_router_publish(&router, lit("$LVC.foo"), lit("bad")));
    mb_router_free(&router);
}

static void test_router_lvc_disabled(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_publish(&router, lit("foo"), lit("cached")));
    CHECK(!mb_router_subscribe(&router, &conn, lit("$LVC.foo"), lit("1")));
    CHECK(conn.out.len == 0);
    mb_router_free(&router);
}

static void test_router_lvc_filter_gates_cache(void) {
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[] = {lit("hot.>")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_publish(&router, lit("cold.one"), lit("old")));
    CHECK(mb_router_publish(&router, lit("hot.one"), lit("new")));
    CHECK(mb_router_subscribe(&router, &conn, lit("$LVC.>"), lit("1")));
    check_buf_eq(&conn.out, "MSG $LVC.hot.one 1 3\r\nnew\r\n");
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_lvc_stats_requires_explicit_filter(void) {
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[] = {lit(">")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    CHECK(mb_router_publish(&router, lit("$STATS.global.pubs"), lit("1")));
    CHECK(mb_router_lvc_count(&router) == 0);
    mb_router_free(&router);

    mb_router_init(&router);
    mb_slice stats_filters[] = {lit("$STATS.>")};
    CHECK(mb_router_configure_lvc(&router, stats_filters, 1));
    CHECK(mb_router_publish(&router, lit("$STATS.global.pubs"), lit("1")));
    CHECK(mb_router_lvc_count(&router) == 1);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("$LVC.$STATS.>"), lit("1")));
    check_buf_eq(&conn.out, "MSG $LVC.$STATS.global.pubs 1 1\r\n1\r\n");
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_lvc_payload_cap(void) {
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[] = {lit(">")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    router.lvc_payload_bytes = MB_MAX_LVC_BYTES;
    CHECK(!mb_router_publish(&router, lit("hot.one"), lit("x")));
    mb_router_free(&router);
}

static void test_router_subscription_caps(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    conn.sub_count = MB_MAX_SUBS_PER_CONN;
    CHECK(!mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    conn.sub_count = 0;
    router.sub_count = MB_MAX_SUBS_TOTAL;
    CHECK(!mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    mb_router_free(&router);
}

static void test_router_unsubscribe(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    mb_router_unsubscribe(&router, &conn, lit("1"), 0, false);
    CHECK(router.sub_count == 0);
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    CHECK(conn.out.len == 0);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_auto_unsubscribe(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    mb_router_unsubscribe(&router, &conn, lit("1"), 2, true);
    CHECK(mb_router_publish(&router, lit("foo"), lit("a")));
    CHECK(mb_router_publish(&router, lit("foo"), lit("b")));
    CHECK(router.sub_count == 0);
    check_buf_eq(&conn.out, "MSG foo 1 1\r\na\r\nMSG foo 1 1\r\nb\r\n");
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

typedef struct kick_probe {
    mb_router_conn *conn;
    int kicks;
} kick_probe;

typedef struct close_probe {
    int closes;
} close_probe;

static void kick_after_full_fanout(void *ctx) {
    kick_probe *probe = ctx;
    probe->kicks += 1;
    check_buf_eq(&probe->conn->out, "MSG foo 1 2\r\nhi\r\nMSG foo 2 2\r\nhi\r\n");
    probe->conn->closed = true;
}

static void test_router_kicks_after_fanout(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    kick_probe probe = {.conn = &conn};
    conn.kick_fn = kick_after_full_fanout;
    conn.kick_ctx = &probe;
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("2")));
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    CHECK(probe.kicks == 1);
    CHECK(conn.closed);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void close_after_pending_cap(void *ctx) {
    close_probe *probe = ctx;
    probe->closes += 1;
}

static void test_router_closes_before_pending_append(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    close_probe probe = {0};
    conn.close_fn = close_after_pending_cap;
    conn.close_ctx = &probe;
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    conn.out.len = MB_MAX_PENDING;
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    CHECK(conn.closed);
    CHECK(probe.closes == 1);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static void test_router_remove_conn(void) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("foo"), lit("1")));
    mb_router_remove_all_for(&router, &conn);
    CHECK(router.sub_count == 0);
    CHECK(mb_router_publish(&router, lit("foo"), lit("hi")));
    CHECK(conn.out.len == 0);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

TEST_MAIN(unit,
          test_parse_ping,
          test_parse_connect,
          test_parse_sub,
          test_parse_queue_sub,
          test_parse_unsub,
          test_parse_pub,
          test_parse_fragmented_pub,
          test_parse_bad_command,
          test_parse_payload_too_large,
          test_parse_control_line_too_long,
          test_parse_rejects_control_bytes_in_tokens,
          test_parse_rejects_pub_wildcards,
          test_parse_malformed_pub_trailer,
          test_write_info,
          test_write_msg,
          test_buf_consume_and_swap,
          test_fs_read_write_helpers,
          test_array_reserve_helpers,
          test_router_one_sub,
          test_router_two_subs,
          test_router_queue_group,
          test_router_queue_group_by_filter,
          test_router_queue_auto_unsubscribe,
          test_router_non_match,
          test_router_wildcards,
          test_router_lvc_replay,
          test_router_lvc_live_wildcard,
          test_router_lvc_rejects_writes,
          test_router_lvc_disabled,
          test_router_lvc_filter_gates_cache,
          test_router_lvc_stats_requires_explicit_filter,
          test_router_lvc_payload_cap,
          test_router_subscription_caps,
          test_router_unsubscribe,
          test_router_auto_unsubscribe,
          test_router_kicks_after_fanout,
          test_router_closes_before_pending_append,
          test_router_remove_conn)
