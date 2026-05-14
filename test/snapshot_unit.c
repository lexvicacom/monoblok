#include "snapshot.h"

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

static bool buf_contains(const mb_buf *buf, const char *needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= buf->len; i += 1) {
        if (memcmp(buf->ptr + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void configure_router_lvc(mb_router *router, const pb_program *program) {
    mb_slice filters[8];
    CHECK(program->lvc.len <= sizeof filters / sizeof filters[0]);
    for (size_t i = 0; i < program->lvc.len; i += 1) {
        filters[i] = (mb_slice){.ptr = (const uint8_t *)program->lvc.filters[i].ptr,
                                .len = program->lvc.filters[i].len};
    }
    CHECK(mb_router_configure_lvc(router, filters, program->lvc.len));
}

static void test_snapshot_roundtrip_lvc_and_state(void) {
    const char *src =
        "(lvc [\">\"])\n"
        "(on \"foo\" (when (changed? payload) (publish! \"out\" payload)))\n";
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-snapshot-%ld.bin", (long)getpid());
    remove(path);

    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    mb_router router;
    mb_router_init(&router);
    configure_router_lvc(&router, &program);
    CHECK(mb_router_publish(&router, lit("foo"), lit("cached")));
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("same"), 0, 0));
    CHECK(mb_snapshot_write(path, &router, &program));
    mb_router_free(&router);
    pb_program_free(&program);

    pb_program restored_program = {0};
    CHECK(pb_program_load_source(&restored_program, "<test>", src, strlen(src)));
    mb_router restored_router;
    mb_router_init(&restored_router);
    configure_router_lvc(&restored_router, &restored_program);
    mb_snapshot_counts counts = {0};
    CHECK(mb_snapshot_load(path, &restored_router, &restored_program, &counts));
    CHECK(counts.lvc == 2);
    CHECK(counts.rule_state == 1);

    mb_router_conn lvc_conn = {0};
    CHECK(mb_router_subscribe(&restored_router, &lvc_conn, lit("$LVC.foo"), lit("1")));
    CHECK(buf_contains(&lvc_conn.out, "MSG $LVC.foo 1 6\r\ncached\r\n"));

    mb_router_conn out_conn = {0};
    CHECK(mb_router_subscribe(&restored_router, &out_conn, lit("out"), lit("2")));
    CHECK(pb_program_eval_publish(&restored_program, &restored_router, lit("foo"), lit("same"), 0, 0));
    CHECK(out_conn.out.len == 0);
    CHECK(pb_program_eval_publish(&restored_program, &restored_router, lit("foo"), lit("next"), 0, 0));
    CHECK(buf_contains(&out_conn.out, "MSG out 2 4\r\nnext\r\n"));

    mb_buf_free(&lvc_conn.out);
    mb_buf_free(&out_conn.out);
    mb_router_free(&restored_router);
    pb_program_free(&restored_program);
    remove(path);
}

int main(void) {
    test_snapshot_roundtrip_lvc_and_state();
    puts("snapshot tests passed");
    return 0;
}
