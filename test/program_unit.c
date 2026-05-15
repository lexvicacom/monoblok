#include "pb_program.h"
#include "test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mb_slice lit(const char *s) {
    return (mb_slice){.ptr = (const uint8_t *)s, .len = strlen(s)};
}

static bool write_file(const char *path, const char *src) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    const size_t len = strlen(src);
    const bool ok = fwrite(src, 1, len, f) == len;
    fclose(f);
    return ok;
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

static size_t count_msgs(const mb_buf *buf) {
    size_t count = 0;
    for (size_t i = 0; i + 4 <= buf->len; i += 1) {
        if (memcmp(buf->ptr + i, "MSG ", 4) == 0) {
            count += 1;
        }
    }
    return count;
}

static bool buf_eq(const mb_buf *buf, const char *want) {
    const size_t len = strlen(want);
    return buf->len == len && memcmp(buf->ptr, want, len) == 0;
}

static void load_program(pb_program *program, const char *src) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-program-%ld.edn", (long)getpid());
    CHECK(write_file(path, src));
    CHECK(pb_program_load_file(program, path));
    remove(path);
}

static void test_reentry_is_opt_in(void) {
    pb_program program = {0};
    load_program(&program, "(on \"a\" (publish! \"b\" payload))\n"
                           "(on \"b\" (publish! \"c\" payload))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0));
    CHECK(buf_contains(&conn.out, "MSG b 1 1\r\nx\r\n"));
    CHECK(!buf_contains(&conn.out, "MSG c 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_reentry_runs_downstream_rules(void) {
    pb_program program = {0};
    load_program(&program, "(on \"a\" :reentrant true (publish! \"b\" payload))\n"
                           "(on \"b\" (publish! \"c\" payload))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0));
    CHECK(buf_contains(&conn.out, "MSG b 1 1\r\nx\r\n"));
    CHECK(buf_contains(&conn.out, "MSG c 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_reentry_eval_failure_does_not_fail_publish(void) {
    pb_program program = {0};
    load_program(&program, "(on \"a\" :reentrant true (publish! \"b\" payload))\n"
                           "(on \"b\" (round payload))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0));
    CHECK(buf_contains(&conn.out, "MSG b 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_reentry_depth_cap(void) {
    pb_program program = {0};
    load_program(&program, "(on \"loop.>\" :reentrant true\n"
                           "  (publish! (subject-append \"x\") payload))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("loop.a"), lit("x"), 0, 0));
    CHECK(count_msgs(&conn.out) == 9);

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_rule_dispatch_preserves_source_order(void) {
    pb_program program = {0};
    load_program(&program, "(on \"foo\" (publish! \"one\" payload))\n"
                           "(on \">\" (publish! \"two\" payload))\n"
                           "(on \"foo\" (publish! \"three\" payload))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0));
    CHECK(buf_eq(&conn.out,
                 "MSG one 1 1\r\nx\r\n"
                 "MSG two 1 1\r\nx\r\n"
                 "MSG three 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_lvc_filters_load(void) {
    pb_program program = {0};
    load_program(&program, "(lvc [\"hot.>\" \"devices.*\"])\n"
                           "(on \"a\" (publish! \"b\" payload))\n");
    CHECK(program.lvc.len == 2);
    CHECK(program.lvc.filters[0].len == strlen("hot.>"));
    CHECK(memcmp(program.lvc.filters[0].ptr, "hot.>", strlen("hot.>")) == 0);
    CHECK(program.lvc.filters[1].len == strlen("devices.*"));
    CHECK(memcmp(program.lvc.filters[1].ptr, "devices.*", strlen("devices.*")) == 0);
    pb_program_free(&program);
}

static bool slice_is(pb_slice slice, const char *s) {
    const size_t len = strlen(s);
    return slice.len == len && memcmp(slice.ptr, s, len) == 0;
}

static void test_bridge_config_loads(void) {
    pb_program program = {0};
    load_program(&program, "(bridge :servers [\"nats://a:4222\" \"nats://b:4222\"]\n"
                           "        :name \"monoblok\"\n"
                           "        :export [\"foo.>\" \"bar\"]\n"
                           "        :tls true\n"
                           "        :max-reconnect 5\n"
                           "        :reconnect-wait-ms 250)\n"
                           "(on \"a\" (publish! \"b\" payload))\n");
    CHECK(program.bridge.present);
    CHECK(program.bridge.servers_len == 2);
    CHECK(program.bridge.exports_len == 2);
    CHECK(slice_is(program.bridge.servers[0], "nats://a:4222"));
    CHECK(slice_is(program.bridge.servers[1], "nats://b:4222"));
    CHECK(slice_is(program.bridge.exports[0], "foo.>"));
    CHECK(slice_is(program.bridge.exports[1], "bar"));
    CHECK(program.bridge.has_name);
    CHECK(slice_is(program.bridge.name, "monoblok"));
    CHECK(program.bridge.tls);
    CHECK(program.bridge.has_max_reconnect);
    CHECK(program.bridge.max_reconnect == 5);
    CHECK(program.bridge.has_reconnect_wait_ms);
    CHECK(program.bridge.reconnect_wait_ms == 250);
    pb_program_free(&program);
}

static void test_bridge_requires_servers(void) {
    pb_program program = {0};
    const char *src = "(bridge :export [\"foo.>\"])\n";
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

TEST_MAIN(program,
          test_reentry_is_opt_in,
          test_reentry_runs_downstream_rules,
          test_reentry_eval_failure_does_not_fail_publish,
          test_reentry_depth_cap,
          test_rule_dispatch_preserves_source_order,
          test_lvc_filters_load,
          test_bridge_config_loads,
          test_bridge_requires_servers)
