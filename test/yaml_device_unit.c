// Built with PB_YAML_DEVICE_DECLS=1: the YAML device declarations tinyblok enables.

#include "pb_yaml.h"
#include "test_check.h"

#include <string.h>

static void check_text(pb_slice got, const char *want) {
    CHECK(got.len == strlen(want));
    CHECK(memcmp(got.ptr, want, got.len) == 0);
}

static void expect_error(const char *src, const char *needle) {
    pb_arena arena = {0};
    pb_parse_result r = pb_parse_yaml_patchbay(&arena, src, strlen(src));
    CHECK(r.err != PB_PARSE_OK);
    if (strstr(r.err_msg, needle) == NULL) {
        fprintf(stderr, "expected \"%s\" in error, got \"%s\" for:\n%s\n", needle, r.err_msg, src);
    }
    CHECK(strstr(r.err_msg, needle) != NULL);
    pb_arena_free(&arena);
}

// The declarations lower to the same forms the EDN spelling produces.
static void test_device_declarations(void) {
    const char *src =
        "pump:\n"
        "  - sub: board.temp\n"
        "    from: board_temp_c\n"
        "    type: f32\n"
        "    hz: 1\n"
        "fn:\n"
        "  - name: board-echo\n"
        "    from: board_echo\n"
        "    input: bytes\n"
        "    type: bytes\n"
        "on-req:\n"
        "  - sub: board.req.echo\n"
        "    do:\n"
        "      - [reply!, [board-echo, $payload]]\n"
        "on:\n"
        "  - sub: board.temp\n"
        "    pipeline:\n"
        "      - $payload-float\n"
        "      - [round, 1]\n"
        "      - [publish!, [subject-append, \"rounded\"]]\n";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_yaml_patchbay(&arena, src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 4);

    pb_values pump = r.forms.items[0].seq;
    CHECK(pump.len == 8);
    check_text(pump.items[0].text, "pump");
    CHECK(pump.items[1].kind == PB_STRING);
    check_text(pump.items[1].text, "board.temp");
    CHECK(pump.items[2].kind == PB_KEYWORD);
    check_text(pump.items[2].text, "from");
    CHECK(pump.items[3].kind == PB_SYMBOL);
    check_text(pump.items[3].text, "board_temp_c");
    CHECK(pump.items[5].kind == PB_SYMBOL);
    check_text(pump.items[5].text, "f32");
    check_text(pump.items[6].text, "hz");
    CHECK(pump.items[7].kind == PB_NUMBER);
    CHECK(pump.items[7].number == 1.0);

    pb_values fn = r.forms.items[1].seq;
    CHECK(fn.len == 8);
    check_text(fn.items[0].text, "fn");
    CHECK(fn.items[1].kind == PB_SYMBOL);
    check_text(fn.items[1].text, "board-echo");
    check_text(fn.items[4].text, "input");
    CHECK(fn.items[5].kind == PB_SYMBOL);
    check_text(fn.items[5].text, "bytes");

    pb_values req = r.forms.items[2].seq;
    CHECK(req.len == 3);
    check_text(req.items[0].text, "on-req");
    CHECK(req.items[1].kind == PB_STRING);
    check_text(req.items[1].text, "board.req.echo");
    CHECK(req.items[2].kind == PB_LIST);

    check_text(r.forms.items[3].seq.items[0].text, "on");
    pb_arena_free(&arena);
}

static void test_device_declaration_errors(void) {
    expect_error("pump:\n  - from: board_temp_c\n    type: f32\n    hz: 1\n", "pump item needs `sub:");
    expect_error("pump:\n  - sub: a\n    from: 3\n", "from must be a name");
    expect_error("pump:\n  - sub: a\n    hz: [1]\n", "hz must be a scalar");
    expect_error("pump:\n  sub: a\n", "pump must be a list");
    expect_error("fn:\n  - from: board_echo\n", "fn item needs `name:");
    expect_error("on-req:\n  - sub: a\n", "exactly one body key");
    expect_error("bogus:\n  - sub: a\n", "unknown top-level key");
}

TEST_MAIN(yaml_device,
          test_device_declarations,
          test_device_declaration_errors)
