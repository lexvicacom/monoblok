#include "proto.h"
#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) fail(__FILE__, __LINE__, #expr); } while (0)

static void fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    exit(1);
}

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

static void test_write_msg(void) {
    mb_buf buf = {0};
    CHECK(mb_write_msg(&buf, lit("foo"), lit("1"), lit("hi")));
    check_buf_eq(&buf, "MSG foo 1 2\r\nhi\r\n");
    mb_buf_free(&buf);
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

int main(void) {
    test_parse_ping();
    test_parse_connect();
    test_parse_sub();
    test_parse_unsub();
    test_parse_pub();
    test_parse_fragmented_pub();
    test_parse_bad_command();
    test_parse_payload_too_large();
    test_parse_control_line_too_long();
    test_write_info();
    test_write_msg();
    test_router_one_sub();
    test_router_two_subs();
    test_router_non_match();
    test_router_wildcards();
    test_router_lvc_replay();
    test_router_lvc_live_wildcard();
    test_router_lvc_rejects_writes();
    test_router_lvc_disabled();
    test_router_lvc_filter_gates_cache();
    test_router_unsubscribe();
    test_router_auto_unsubscribe();
    test_router_kicks_after_fanout();
    test_router_remove_conn();
    puts("unit tests passed");
    return 0;
}
