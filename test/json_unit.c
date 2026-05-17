#include "pb_json.h"
#include "test_check.h"
#include "test_pb_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_json_patchbay_form(void) {
    const char *src =
        "[[\"on\", \"sensors.*\", [\"publish!\", [\"subject-append\", \"seen\"], \"payload\"]],"
        " [\"export\", {\"servers\": [\"nats://127.0.0.1:4223\"], \"export\": [\"sensors.>\"]}]]";

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

static void test_json_single_form_and_special_objects(void) {
    const char *src =
        "[\"do\", null, true, 7, {\"kw\":\"mode\"},"
        " {\"vec\":[1, \"subject\", {\"str\":\"subject\"}]},"
        " [\"publish!\", {\"str\":\"out\"}, {\"sym\":\"payload\"}]]";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "patchbay.json", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);

    pb_values items = r.forms.items[0].seq;
    CHECK(items.len == 7);
    CHECK(items.items[0].kind == PB_SYMBOL);
    check_text(items.items[0].text, "do");
    CHECK(items.items[1].kind == PB_NIL);
    CHECK(items.items[2].kind == PB_BOOL && items.items[2].boolean);
    CHECK(items.items[3].kind == PB_NUMBER && items.items[3].number == 7.0);
    CHECK(items.items[4].kind == PB_KEYWORD);
    check_text(items.items[4].text, "mode");
    CHECK(items.items[5].kind == PB_VECTOR);
    CHECK(items.items[5].seq.items[1].kind == PB_SYMBOL);
    check_text(items.items[5].seq.items[1].text, "subject");
    CHECK(items.items[5].seq.items[2].kind == PB_STRING);
    check_text(items.items[5].seq.items[2].text, "subject");
    CHECK(items.items[6].kind == PB_LIST);
    CHECK(items.items[6].seq.items[1].kind == PB_STRING);
    check_text(items.items[6].seq.items[1].text, "out");
    CHECK(items.items[6].seq.items[2].kind == PB_SYMBOL);
    check_text(items.items[6].seq.items[2].text, "payload");
    pb_arena_free(&arena);
}

static void test_json_invalid_inputs(void) {
    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "patchbay.json", "{\"not\":\"a list\"}", strlen("{\"not\":\"a list\"}"));
    CHECK(r.err == PB_PARSE_INVALID_JSON);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = pb_parse_patchbay_source(&arena, "patchbay.json", "[[]]", strlen("[[]]"));
    CHECK(r.err == PB_PARSE_INVALID_LIST_HEAD);
    pb_arena_free(&arena);

    arena = (pb_arena){0};
    r = pb_parse_patchbay_source(&arena, "patchbay.json", "[", strlen("["));
    CHECK(r.err == PB_PARSE_INVALID_JSON);
    pb_arena_free(&arena);
}

TEST_MAIN(json,
          test_json_patchbay_form,
          test_json_single_form_and_special_objects,
          test_json_invalid_inputs)
