#include "pb_program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) fail(__FILE__, __LINE__, #expr); } while (0)

static void fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    exit(1);
}

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

static void load_program(pb_program *program, const char *src) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-c-program-%ld.edn", (long)getpid());
    CHECK(write_file(path, src));
    CHECK(pb_program_load_file(program, path));
    remove(path);
}

static void test_reentry_is_opt_in(void) {
    pb_program program = {0};
    load_program(&program,
        "(on \"a\" (publish! \"b\" payload))\n"
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
    load_program(&program,
        "(on \"a\" :reentrant true (publish! \"b\" payload))\n"
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

static void test_reentry_depth_cap(void) {
    pb_program program = {0};
    load_program(&program,
        "(on \"loop.>\" :reentrant true\n"
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

int main(void) {
    test_reentry_is_opt_in();
    test_reentry_runs_downstream_rules();
    test_reentry_depth_cap();
    puts("program tests passed");
    return 0;
}
