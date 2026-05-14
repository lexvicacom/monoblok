#include "pb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) fail(__FILE__, __LINE__, #expr); } while (0)

static void fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    exit(1);
}

static void check_text(pb_slice s, const char *want) {
    CHECK(s.len == strlen(want));
    CHECK(memcmp(s.ptr, want, s.len) == 0);
}

static void test_json_patchbay_form(void) {
    const char *src =
        "[[\"on\", \"sensors.*\", [\"publish!\", [\"subject-append\", \"seen\"], \"payload\"]],"
        " [\"bridge\", {\"servers\": [\"nats://127.0.0.1:4223\"], \"export\": [\"sensors.>\"]}]]";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "patchbay.json", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 2);
    CHECK(r.forms.items[0].kind == PB_LIST);
    CHECK(r.forms.items[0].seq.items[0].kind == PB_SYMBOL);
    check_text(r.forms.items[0].seq.items[0].text, "on");
    CHECK(r.forms.items[0].seq.items[2].kind == PB_LIST);
    CHECK(r.forms.items[0].seq.items[2].seq.items[2].kind == PB_SYMBOL);
    check_text(r.forms.items[0].seq.items[2].seq.items[2].text, "payload");

    CHECK(r.forms.items[1].seq.len == 5);
    CHECK(r.forms.items[1].seq.items[1].kind == PB_KEYWORD);
    check_text(r.forms.items[1].seq.items[1].text, "servers");
    CHECK(r.forms.items[1].seq.items[2].kind == PB_VECTOR);
    pb_arena_free(&arena);
}

int main(void) {
    test_json_patchbay_form();
    puts("json tests passed");
    return 0;
}
