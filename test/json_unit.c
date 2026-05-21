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

static void test_json_replaying_symbol(void) {
    const char *src = "[[\"on\", \"foo\", [\"publish!\", \"out\", \"replaying?\"]]]";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "patchbay.json", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);
    pb_values body = r.forms.items[0].seq.items[2].seq;
    CHECK(body.items[2].kind == PB_SYMBOL);
    check_text(body.items[2].text, "replaying?");
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

static void test_yaml_patchbay_sugar(void) {
    const char *src =
        "lvc:\n"
        "  - car.>\n"
        "export:\n"
        "  servers: [\"nats://127.0.0.1:4223\"]\n"
        "  export: [\"car.>\"]\n"
        "on:\n"
        "  - sub: car.*.rpm\n"
        "    thread:\n"
        "      from: payload-float\n"
        "      steps:\n"
        "        - [quantize, 50]\n"
        "        - [squelch]\n"
        "        - [publish!, [subject-append, stable]]\n"
        "  - sub: car.*.rpm\n"
        "    when:\n"
        "      test: [>, [moving-avg, 20, payload-float], 7500.0]\n"
        "      then:\n"
        "        thread:\n"
        "          - payload\n"
        "          - [hold-off, 5000]\n"
        "          - [publish!, [subject-append, alert]]\n";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "rental-car.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 4);

    CHECK(r.forms.items[0].kind == PB_LIST);
    check_text(r.forms.items[0].seq.items[0].text, "lvc");
    CHECK(r.forms.items[0].seq.items[1].kind == PB_VECTOR);
    check_text(r.forms.items[0].seq.items[1].seq.items[0].text, "car.>");

    CHECK(r.forms.items[1].kind == PB_LIST);
    check_text(r.forms.items[1].seq.items[0].text, "export");
    CHECK(r.forms.items[1].seq.items[1].kind == PB_KEYWORD);
    check_text(r.forms.items[1].seq.items[1].text, "servers");
    CHECK(r.forms.items[1].seq.items[2].kind == PB_VECTOR);

    CHECK(r.forms.items[2].kind == PB_LIST);
    pb_values first = r.forms.items[2].seq;
    check_text(first.items[0].text, "on");
    check_text(first.items[1].text, "car.*.rpm");
    CHECK(first.items[2].kind == PB_LIST);
    check_text(first.items[2].seq.items[0].text, "->");
    CHECK(first.items[2].seq.items[1].kind == PB_SYMBOL);
    check_text(first.items[2].seq.items[1].text, "payload-float");
    check_text(first.items[2].seq.items[2].seq.items[0].text, "quantize");
    CHECK(first.items[2].seq.items[2].seq.items[1].kind == PB_NUMBER);
    check_text(first.items[2].seq.items[4].seq.items[1].seq.items[1].text, "stable");

    CHECK(r.forms.items[3].kind == PB_LIST);
    pb_values second = r.forms.items[3].seq;
    check_text(second.items[2].seq.items[0].text, "when");
    check_text(second.items[2].seq.items[1].seq.items[0].text, ">");
    check_text(second.items[2].seq.items[2].seq.items[0].text, "->");
    pb_arena_free(&arena);
}

static void test_yaml_config_env_sugar(void) {
    const char *src =
        "export:\n"
        "  servers:\n"
        "    env: MB_TEST_NATS_SERVERS\n"
        "  export:\n"
        "    - env: MB_TEST_EXPORT_FILTER\n";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "env.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);
    pb_values form = r.forms.items[0].seq;
    check_text(form.items[0].text, "export");
    check_text(form.items[1].text, "servers");
    CHECK(form.items[2].kind == PB_LIST);
    check_text(form.items[2].seq.items[0].text, "env");
    CHECK(form.items[2].seq.items[1].kind == PB_STRING);
    check_text(form.items[2].seq.items[1].text, "MB_TEST_NATS_SERVERS");
    check_text(form.items[3].text, "export");
    CHECK(form.items[4].kind == PB_VECTOR);
    CHECK(form.items[4].seq.items[0].kind == PB_LIST);
    check_text(form.items[4].seq.items[0].seq.items[0].text, "env");
    check_text(form.items[4].seq.items[0].seq.items[1].text, "MB_TEST_EXPORT_FILTER");
    pb_arena_free(&arena);
}

static void test_yaml_replaying_symbol(void) {
    const char *src =
        "on:\n"
        "  - sub: foo\n"
        "    form: [publish!, out, replaying?]\n";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "replay.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);
    pb_values body = r.forms.items[0].seq.items[2].seq;
    CHECK(body.items[2].kind == PB_SYMBOL);
    check_text(body.items[2].text, "replaying?");
    pb_arena_free(&arena);
}

static void test_yaml_nested_import_config_sugar(void) {
    const char *src =
        "import:\n"
        "  core:\n"
        "    - servers: [\"nats://core:4222\"]\n"
        "      subject: [raw.>]\n"
        "  streams:\n"
        "    - servers: [\"nats://js:4222\"]\n"
        "      subject: [sensors.>]\n"
        "      stream: SENSORS\n"
        "      consumer: monoblok-sensors\n"
        "      catch-up: true\n";

    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "nested-import.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);
    pb_values form = r.forms.items[0].seq;
    check_text(form.items[0].text, "import");
    check_text(form.items[1].text, "core");
    CHECK(form.items[2].kind == PB_VECTOR);
    CHECK(form.items[2].seq.items[0].kind == PB_VECTOR);
    check_text(form.items[2].seq.items[0].seq.items[0].text, "servers");
    check_text(form.items[3].text, "streams");
    CHECK(form.items[4].kind == PB_VECTOR);
    CHECK(form.items[4].seq.items[0].kind == PB_VECTOR);
    check_text(form.items[4].seq.items[0].seq.items[0].text, "servers");
    check_text(form.items[4].seq.items[0].seq.items[4].text, "stream");
    check_text(form.items[4].seq.items[0].seq.items[5].text, "SENSORS");
    pb_arena_free(&arena);
}

static void test_yaml_invalid_inputs(void) {
    const char *src = "on:\n  - sub: car.*.rpm\n    thread:\n      from: payload-float\n";
    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, "bad.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_INVALID_YAML);
    pb_arena_free(&arena);
}

TEST_MAIN(json,
          test_json_patchbay_form,
          test_json_single_form_and_special_objects,
          test_json_replaying_symbol,
          test_json_invalid_inputs,
          test_yaml_patchbay_sugar,
          test_yaml_config_env_sugar,
          test_yaml_replaying_symbol,
          test_yaml_nested_import_config_sugar,
          test_yaml_invalid_inputs)
