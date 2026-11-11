#include "pb_json.h"
#include "test_check.h"
#include "test_pb_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_json_patchbay_form(void) {
    const char *src =
        "[[\"on\", \"sensors.*\", [\"publish!\", [\"subject-append\", \"seen\"], \"$payload\"]],"
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
        " {\"vector\":[1, \"$subject\", {\"str\":\"subject\"}]},"
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
    const char *src = "[[\"on\", \"foo\", [\"publish!\", \"out\", \"$replaying?\"]]]";

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
        "  - sub: \"car.*.rpm\"\n"
        "    thread:\n"
        "      - $payload-float\n"
        "      - [quantize, 50]\n"
        "      - [squelch]\n"
        "      - [publish!, [subject-append, \"stable\"], dp, 1]\n"
        "  - sub: \"car.*.rpm\"\n"
        "    when: [\">\", [moving-avg, 20, $payload-float], 7500.0]\n"
        "    thread:\n"
        "      - $payload\n"
        "      - [hold-off, 5000]\n"
        "      - [publish!, [subject-append, \"alert\"]]\n";

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
    CHECK(first.items[2].seq.items[4].seq.items[2].kind == PB_KEYWORD);
    check_text(first.items[2].seq.items[4].seq.items[2].text, "dp");
    CHECK(first.items[2].seq.items[4].seq.items[3].kind == PB_NUMBER);

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
        "    env: \"MB_TEST_NATS_SERVERS\"\n"
        "  export:\n"
        "    - env: \"MB_TEST_EXPORT_FILTER\"\n";

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
        "  - sub: \"foo\"\n"
        "    form: [publish!, \"out\", $replaying?]\n";

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
        "      subject: [\"raw.>\"]\n"
        "  streams:\n"
        "    - servers: [\"nats://js:4222\"]\n"
        "      subject: [\"sensors.>\"]\n"
        "      stream: \"SENSORS\"\n"
        "      consumer: \"monoblok-sensors\"\n"
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

// Parses src as a YAML or JSON patchbay and checks it fails with `needle` in the detail message.
static void expect_load_error(const char *path, const char *src, const char *needle) {
    pb_arena arena = {0};
    pb_parse_result r = pb_parse_patchbay_source(&arena, path, src, strlen(src));
    if (r.err == PB_PARSE_OK) {
        fprintf(stderr, "expected load error \"%s\" but loaded:\n%s\n", needle, src);
    }
    CHECK(r.err != PB_PARSE_OK);
    if (strstr(r.err_msg, needle) == NULL) {
        fprintf(stderr, "expected \"%s\" in error, got \"%s\" for:\n%s\n", needle, r.err_msg, src);
    }
    CHECK(strstr(r.err_msg, needle) != NULL);
    pb_arena_free(&arena);
}

// Wraps one `form:` expression into a complete on-rule.
static void expect_form_error(const char *form, const char *needle) {
    char src[512];
    snprintf(src, sizeof src, "on:\n  - sub: \"a\"\n    form: %s\n", form);
    expect_load_error("t.yml", src, needle);
}

static void test_yaml_invalid_inputs(void) {
    // Old map-form thread: from/steps.
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n    thread:\n      from: $payload-float\n      steps:\n        - [round, 1]\n",
                      "from/steps map form was removed");
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n    body: [publish!, \"x\"]\n", "use form");
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n    when:\n      test: [\">\", 1, 2]\n      then: [publish!, \"x\"]\n",
                      "when: {test, then} map form was removed");
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n    form: [publish!, \"x\"]\n    thread: [$payload]\n", "exactly one of thread, do or form");
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n", "exactly one of thread, do or form");
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n    form: [publish!, \"x\"]\n    bogus: 1\n", "unknown key \"bogus\"");
    expect_load_error("bad.yml", "every:\n  - ms: 5\n    form: [publish!, \"x\"]\n    sub: \"a\"\n", "unknown key \"sub\"");
    expect_load_error("bad.yml", "on:\n  - form: [publish!, \"x\"]\n", "needs `sub");
    expect_load_error("bad.yml", "every:\n  - form: [publish!, \"x\"]\n", "needs `ms");
    expect_load_error("bad.yml", "on:\n  - sub: \"a\"\n    thread: []\n", "non-empty list");
}

static void test_yaml_scalar_rules(void) {
    expect_form_error("[publish!, \"x\", payload]", "use $payload");
    expect_form_error("[publish!, \"x\", replaying?]", "use $replaying?");
    expect_form_error("[publish!, \"x\", payload-flaot]", "unquoted word \"payload-flaot\"");
    expect_form_error("[publish!, \"x\", hello]", "quote it");
    expect_form_error("[publish!, \"x\", $payload-flaot]", "unknown binding $payload-flaot");
    expect_form_error("[publish!, \"x\", :ms]", "write the option without the colon: ms");
    expect_form_error("[moving-avg, :ms, 5, $payload-float]", "without the colon");
    expect_form_error("[publish!, \"x\", ms, bogus]", "quote it");
    expect_form_error("[>, 1, 2]", "must be quoted in YAML");
    expect_form_error("[->, 1, 2]", "must be quoted in YAML");
    expect_form_error("[*, 1, 2]", "must be quoted in YAML");
    expect_form_error("[$payload, 1]", "cannot be the first item");
    expect_form_error("[1, 2]", "operator name");
    expect_form_error("[]", "empty list");

    pb_arena arena = {0};
    const char *src =
        "on:\n"
        "  - sub: \"a\"\n"
        "    reentrant: true\n"
        "    form: [publish!, \"out\", \":ms\", \"$subject\", \"payload\", $subject, dp, 1, true, null, nil, ~, 5, -2.5, '>']\n";
    pb_parse_result r = pb_parse_patchbay_source(&arena, "scalars.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);
    pb_values form = r.forms.items[0].seq;
    CHECK(form.len == 5);
    CHECK(form.items[2].kind == PB_KEYWORD);
    pb_values call = form.items[4].seq;
    CHECK(call.items[2].kind == PB_STRING);
    check_text(call.items[2].text, ":ms");
    CHECK(call.items[3].kind == PB_STRING);
    check_text(call.items[3].text, "$subject");
    CHECK(call.items[4].kind == PB_STRING);
    check_text(call.items[4].text, "payload");
    CHECK(call.items[5].kind == PB_SYMBOL);
    check_text(call.items[5].text, "subject");
    CHECK(call.items[6].kind == PB_KEYWORD);
    check_text(call.items[6].text, "dp");
    CHECK(call.items[7].kind == PB_NUMBER);
    CHECK(call.items[8].kind == PB_BOOL);
    CHECK(call.items[9].kind == PB_NIL && call.items[10].kind == PB_NIL && call.items[11].kind == PB_NIL);
    CHECK(call.items[13].kind == PB_NUMBER && call.items[13].number == -2.5);
    CHECK(call.items[14].kind == PB_STRING);
    pb_arena_free(&arena);

    // Every accepted option word lowers to a keyword; $payload-bool is a binding.
    arena = (pb_arena){0};
    src = "on:\n  - sub: \"a\"\n    form: [publish!, \"o\", ms, dp, subject, lost, found, leaf, date, hour, minute, avg, sum, min, max, count, rate, reentrant, $payload-bool, $reply-to]\n";
    r = pb_parse_patchbay_source(&arena, "words.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    call = r.forms.items[0].seq.items[2].seq;
    for (size_t i = 2; i < 18; i += 1) {
        CHECK(call.items[i].kind == PB_KEYWORD);
    }
    CHECK(call.items[18].kind == PB_SYMBOL);
    check_text(call.items[18].text, "payload-bool");
    CHECK(call.items[19].kind == PB_SYMBOL);
    pb_arena_free(&arena);

    // Quoted operators in head position are the operator.
    arena = (pb_arena){0};
    src = "on:\n  - sub: \"a\"\n    form: [\">=\", $payload-float, 1]\n";
    r = pb_parse_patchbay_source(&arena, "ops.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.items[0].seq.items[2].seq.items[0].kind == PB_SYMBOL);
    check_text(r.forms.items[0].seq.items[2].seq.items[0].text, ">=");
    pb_arena_free(&arena);
}

static void test_yaml_rule_shape(void) {
    pb_arena arena = {0};
    const char *src =
        "every:\n"
        "  - ms: 1000\n"
        "    when: [not, $replaying?]\n"
        "    form: [publish!, \"tick\", [contains?, [vector, 1, 2, 3], 2]]\n"
        "on:\n"
        "  - sub: \"a\"\n"
        "    do:\n"
        "      - [count!]\n"
        "      - when: [\">\", $payload-float, 1]\n"
        "        thread:\n"
        "          - $payload\n"
        "          - [publish!, \"out\"]\n"
        "      - do:\n"
        "          - [publish!, \"b\", [subject-with, [vector, \"a\", $subject]]]\n";
    pb_parse_result r = pb_parse_patchbay_source(&arena, "shape.yml", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 2);

    pb_values every = r.forms.items[0].seq;
    check_text(every.items[0].text, "every");
    CHECK(every.items[1].kind == PB_KEYWORD);
    pb_values guard = every.items[3].seq;
    check_text(guard.items[0].text, "when");
    check_text(guard.items[1].seq.items[0].text, "not");
    check_text(guard.items[2].seq.items[0].text, "publish!");
    pb_value vec = guard.items[2].seq.items[2].seq.items[1];
    CHECK(vec.kind == PB_VECTOR);
    CHECK(vec.seq.len == 3);

    pb_values on = r.forms.items[1].seq;
    pb_values do_form = on.items[2].seq;
    check_text(do_form.items[0].text, "do");
    CHECK(do_form.len == 4);
    check_text(do_form.items[2].seq.items[0].text, "when");
    check_text(do_form.items[2].seq.items[2].seq.items[0].text, "->");
    check_text(do_form.items[3].seq.items[0].text, "do");
    CHECK(do_form.items[3].seq.items[1].seq.items[2].seq.items[1].kind == PB_VECTOR);
    pb_arena_free(&arena);
}

static void test_json_binding_rules(void) {
    // The bare string "payload" is just a string now.
    pb_arena arena = {0};
    const char *src = "[[\"on\", \"a\", [\"publish!\", \"out\", \"payload\", \"$payload-bool\", {\"str\": \"$x\"}]]]";
    pb_parse_result r = pb_parse_patchbay_source(&arena, "b.json", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    pb_values call = r.forms.items[0].seq.items[2].seq;
    CHECK(call.items[2].kind == PB_STRING);
    CHECK(call.items[3].kind == PB_SYMBOL);
    check_text(call.items[3].text, "payload-bool");
    CHECK(call.items[4].kind == PB_STRING);
    check_text(call.items[4].text, "$x");
    pb_arena_free(&arena);

    // Config forms keep plain strings, even ones starting with $.
    arena = (pb_arena){0};
    src = "[[\"lvc\", \"$weird.>\"]]";
    r = pb_parse_patchbay_source(&arena, "c.json", src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.items[0].seq.items[1].kind == PB_STRING);
    pb_arena_free(&arena);

    expect_load_error("b.json", "[[\"on\", \"a\", [\"publish!\", \"out\", \"$payload-flaot\"]]]", "unknown binding $payload-flaot");
    expect_load_error("b.json", "[[\"on\", \"a\", [\"contains?\", {\"vec\": [1, 2]}, \"$payload\"]]]", "write {\"vector\": [...]}");
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
          test_yaml_invalid_inputs,
          test_yaml_scalar_rules,
          test_yaml_rule_shape,
          test_json_binding_rules)
