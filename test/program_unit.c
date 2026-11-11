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
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0, (pb_program_eval_options){0}));
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
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0, (pb_program_eval_options){0}));
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
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0, (pb_program_eval_options){0}));
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
    CHECK(pb_program_eval_publish(&program, &router, lit("loop.a"), lit("x"), 0, 0, (pb_program_eval_options){0}));
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
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_eq(&conn.out,
                 "MSG one 1 1\r\nx\r\n"
                 "MSG two 1 1\r\nx\r\n"
                 "MSG three 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_rule_stats_count_emits_and_suppression(void) {
    pb_program program = {0};
    load_program(&program, "(on \"a\" (publish! \"b\" (squelch payload)))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 1, 0, (pb_program_eval_options){0}));
    CHECK(program.rules[0].publishes_emitted == 1);
    CHECK(program.rules[0].publishes_suppressed == 1);
    CHECK(count_msgs(&conn.out) == 1);

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_replaying_publish_option(void) {
    pb_program program = {0};
    load_program(&program, "(on \"foo\" (if replaying?\n"
                           "              (publish! \"replay.foo\" payload)\n"
                           "              (publish! \"live.foo\" payload)))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));

    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG live.foo 1 1\r\nx\r\n"));
    CHECK(!buf_contains(&conn.out, "MSG replay.foo 1 1\r\nx\r\n"));

    mb_buf_clear(&conn.out);
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0,
                                               (pb_program_eval_options){.replaying = true}));
    CHECK(buf_contains(&conn.out, "MSG replay.foo 1 1\r\nx\r\n"));
    CHECK(!buf_contains(&conn.out, "MSG live.foo 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_tick_until_drains_due_deadlines(void) {
    pb_program program = {0};
    load_program(&program, "(on \"foo\" (debounce! :ms 100 \"settled.foo\" payload))\n");

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));

    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 1000, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_tick_until(&program, &router, 1099, 1099, (pb_program_eval_options){0}));
    CHECK(conn.out.len == 0);
    CHECK(pb_program_tick_until(&program, &router, 1100, 1100, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG settled.foo 1 1\r\nx\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_time_window_deadline_uses_inclusive_cutoff(void) {
    pb_program program = {0};
    load_program(&program, "(on \"foo\" (moving-avg :ms 1000 payload-float))\n");

    mb_router router;
    mb_router_init(&router);

    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("10"), 1000, 0, (pb_program_eval_options){0}));

    uint64_t deadline = 0;
    CHECK(pb_program_next_clock_deadline(&program, &deadline));
    CHECK(deadline == 2001);

    CHECK(pb_program_tick_until(&program, &router, 2000, 2000, (pb_program_eval_options){0}));
    CHECK(pb_program_next_clock_deadline(&program, &deadline));
    CHECK(deadline == 2001);

    CHECK(pb_program_tick_until(&program, &router, 2001, 2001, (pb_program_eval_options){0}));
    CHECK(!pb_program_next_clock_deadline(&program, &deadline));

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

static void test_export_config_loads(void) {
    pb_program program = {0};
    load_program(&program, "(export :servers [\"nats://a:4222\" \"nats://b:4222\"]\n"
                           "        :name \"monoblok\"\n"
                           "        :export [\"foo.>\" \"bar\"]\n"
                           "        :tls true\n"
                           "        :origin-header true\n"
                           "        :max-reconnect 5\n"
                           "        :reconnect-wait-ms 250)\n"
                           "(on \"a\" (publish! \"b\" payload))\n");
    CHECK(program.bridge.present);
    CHECK(program.bridge.remote.servers_len == 2);
    CHECK(program.bridge.exports_len == 2);
    CHECK(slice_is(program.bridge.remote.servers[0], "nats://a:4222"));
    CHECK(slice_is(program.bridge.remote.servers[1], "nats://b:4222"));
    CHECK(slice_is(program.bridge.exports[0], "foo.>"));
    CHECK(slice_is(program.bridge.exports[1], "bar"));
    CHECK(program.bridge.remote.name.len != 0);
    CHECK(slice_is(program.bridge.remote.name, "monoblok"));
    CHECK(program.bridge.remote.tls);
    CHECK(program.bridge.origin_header);
    CHECK(program.bridge.remote.has_max_reconnect);
    CHECK(program.bridge.remote.max_reconnect == 5);
    CHECK(program.bridge.remote.has_reconnect_wait_ms);
    CHECK(program.bridge.remote.reconnect_wait_ms == 250);
    pb_program_free(&program);
}

static void test_config_negative_and_absent_remote_values(void) {
    pb_program program = {0};
    load_program(&program, "(export :servers \"nats://a:4222\" :max-reconnect -1 :connect-timeout-ms 0)\n"
                           "(on \"a\" (publish! \"b\" payload))\n");
    CHECK(program.bridge.remote.has_max_reconnect);
    CHECK(program.bridge.remote.max_reconnect == -1);
    CHECK(program.bridge.remote.has_connect_timeout_ms);
    CHECK(program.bridge.remote.connect_timeout_ms == 0);
    CHECK(!program.bridge.remote.has_ping_interval_ms);
    CHECK(program.bridge.remote.name.len == 0);
    CHECK(program.bridge.remote.creds.len == 0);
    pb_program_free(&program);
}

static void test_config_env_values_load(void) {
    CHECK(setenv("MB_TEST_PB_LVC", "hot.>", 1) == 0);
    CHECK(setenv("MB_TEST_PB_SERVER", "nats://env:4222", 1) == 0);
    CHECK(setenv("MB_TEST_PB_NAME", "env-name", 1) == 0);
    CHECK(setenv("MB_TEST_PB_EXPORT", "telemetry.>", 1) == 0);
    CHECK(setenv("MB_TEST_PB_TOKEN", "env-token", 1) == 0);
    CHECK(setenv("MB_TEST_PB_SUBJECT", "raw.>", 1) == 0);
    CHECK(setenv("MB_TEST_PB_CREDS", "/tmp/env.creds", 1) == 0);

    pb_program program = {0};
    const char *src =
        "(lvc (env \"MB_TEST_PB_LVC\"))\n"
        "(export :servers (env \"MB_TEST_PB_SERVER\")\n"
        "        :name (env \"MB_TEST_PB_NAME\")\n"
        "        :export [(env \"MB_TEST_PB_EXPORT\")]\n"
        "        :token (env \"MB_TEST_PB_TOKEN\"))\n"
        "(import :servers [(env \"MB_TEST_PB_SERVER\")]\n"
        "        :subject (env \"MB_TEST_PB_SUBJECT\")\n"
        "        :creds (env \"MB_TEST_PB_CREDS\"))\n";
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    CHECK(program.lvc.len == 1);
    CHECK(slice_is(program.lvc.filters[0], "hot.>"));
    CHECK(program.bridge.present);
    CHECK(program.bridge.remote.servers_len == 1);
    CHECK(program.bridge.exports_len == 1);
    CHECK(slice_is(program.bridge.remote.servers[0], "nats://env:4222"));
    CHECK(slice_is(program.bridge.exports[0], "telemetry.>"));
    CHECK(program.bridge.remote.name.len != 0);
    CHECK(slice_is(program.bridge.remote.name, "env-name"));
    CHECK(program.bridge.remote.token.len != 0);
    CHECK(slice_is(program.bridge.remote.token, "env-token"));
    CHECK(program.importer.present);
    CHECK(program.importer.cores_len == 1);
    const pb_import_config *core = &program.importer.cores[0];
    CHECK(core->remote.servers_len == 1);
    CHECK(core->subjects_len == 1);
    CHECK(slice_is(core->remote.servers[0], "nats://env:4222"));
    CHECK(slice_is(core->subjects[0], "raw.>"));
    CHECK(core->remote.creds.len != 0);
    CHECK(slice_is(core->remote.creds, "/tmp/env.creds"));
    pb_program_free(&program);

    unsetenv("MB_TEST_PB_LVC");
    unsetenv("MB_TEST_PB_SERVER");
    unsetenv("MB_TEST_PB_NAME");
    unsetenv("MB_TEST_PB_EXPORT");
    unsetenv("MB_TEST_PB_TOKEN");
    unsetenv("MB_TEST_PB_SUBJECT");
    unsetenv("MB_TEST_PB_CREDS");
}

static void test_missing_config_env_fails(void) {
    unsetenv("MB_TEST_PB_MISSING");
    pb_program program = {0};
    const char *src = "(export :servers (env \"MB_TEST_PB_MISSING\"))\n";
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

static void test_yaml_replaying_alias(void) {
    const char *spellings[] = {"$replaying", "$replaying?"};
    for (size_t i = 0; i < sizeof spellings / sizeof spellings[0]; i += 1) {
        char src[512];
        snprintf(src, sizeof src,
                 "on:\n"
                 "  - sub: \"foo\"\n"
                 "    do:\n"
                 "      - [if, %s, [publish!, \"replay.foo\", $payload], [publish!, \"live.foo\", $payload]]\n"
                 "      - [publish!, \"literal\", \"$replaying\"]\n", spellings[i]);
        pb_program program = {0};
        CHECK(pb_program_load_source(&program, "replaying.yml", src, strlen(src)));
        mb_router router;
        mb_router_init(&router);
        mb_router_conn conn = {0};
        CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
        CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0, (pb_program_eval_options){0}));
        CHECK(buf_contains(&conn.out, "MSG live.foo 1 1\r\nx\r\n"));
        CHECK(!buf_contains(&conn.out, "MSG replay.foo 1 1\r\nx\r\n"));
        CHECK(buf_contains(&conn.out, "MSG literal 1 10\r\n$replaying\r\n"));
        mb_buf_clear(&conn.out);
        CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0,
                                      (pb_program_eval_options){.replaying = true}));
        CHECK(buf_contains(&conn.out, "MSG replay.foo 1 1\r\nx\r\n"));
        CHECK(!buf_contains(&conn.out, "MSG live.foo 1 1\r\nx\r\n"));
        mb_buf_free(&conn.out);
        mb_router_free(&router);
        pb_program_free(&program);
    }
}

static void test_yaml_config_env_values_load(void) {
    CHECK(setenv("MB_TEST_PB_YAML_SERVER", "nats://yaml:4222", 1) == 0);
    CHECK(setenv("MB_TEST_PB_YAML_EXPORT", "yaml.>", 1) == 0);
    CHECK(setenv("MB_TEST_PB_YAML_NAME", "yaml-name", 1) == 0);
    CHECK(setenv("MB_TEST_PB_YAML_SUBJECT", "raw.yaml.>", 1) == 0);

    pb_program program = {0};
    const char *src =
        "export:\n"
        "  servers:\n"
        "    env: \"MB_TEST_PB_YAML_SERVER\"\n"
        "  export:\n"
        "    - env: \"MB_TEST_PB_YAML_EXPORT\"\n"
        "  name:\n"
        "    env: \"MB_TEST_PB_YAML_NAME\"\n"
        "import:\n"
        "  servers:\n"
        "    - env: \"MB_TEST_PB_YAML_SERVER\"\n"
        "  subject:\n"
        "    env: \"MB_TEST_PB_YAML_SUBJECT\"\n";
    CHECK(pb_program_load_source(&program, "env.yml", src, strlen(src)));
    CHECK(program.bridge.present);
    CHECK(slice_is(program.bridge.remote.servers[0], "nats://yaml:4222"));
    CHECK(slice_is(program.bridge.exports[0], "yaml.>"));
    CHECK(program.bridge.remote.name.len != 0);
    CHECK(slice_is(program.bridge.remote.name, "yaml-name"));
    CHECK(program.importer.present);
    CHECK(program.importer.cores_len == 1);
    CHECK(slice_is(program.importer.cores[0].remote.servers[0], "nats://yaml:4222"));
    CHECK(slice_is(program.importer.cores[0].subjects[0], "raw.yaml.>"));
    pb_program_free(&program);

    unsetenv("MB_TEST_PB_YAML_SERVER");
    unsetenv("MB_TEST_PB_YAML_EXPORT");
    unsetenv("MB_TEST_PB_YAML_NAME");
    unsetenv("MB_TEST_PB_YAML_SUBJECT");
}

static void test_deprecated_bridge_config_loads(void) {
    pb_program program = {0};
    load_program(&program, "(bridge :servers [\"nats://a:4222\"]\n"
                           "        :export [\"foo.>\"])\n");
    CHECK(program.bridge.present);
    CHECK(program.bridge.remote.servers_len == 1);
    CHECK(program.bridge.exports_len == 1);
    CHECK(slice_is(program.bridge.remote.servers[0], "nats://a:4222"));
    CHECK(slice_is(program.bridge.exports[0], "foo.>"));
    pb_program_free(&program);
}

static void test_export_requires_servers(void) {
    pb_program program = {0};
    const char *src = "(export :export [\"foo.>\"])\n";
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

static void test_import_config_loads(void) {
    pb_program program = {0};
    load_program(&program, "(import :servers [\"nats://a:4222\"]\n"
                           "        :subject [\"raw.>\" \"replay.*\"]\n"
                           "        :name \"monoblok-import\"\n"
                           "        :origin-header true\n"
                           "        :max-pending 128)\n"
                           "(on \"raw.*\" (publish! \"clean\" payload))\n");
    CHECK(program.importer.present);
    CHECK(program.importer.cores_len == 1);
    CHECK(program.importer.streams_len == 0);
    const pb_import_config *core = &program.importer.cores[0];
    CHECK(core->remote.servers_len == 1);
    CHECK(core->subjects_len == 2);
    CHECK(slice_is(core->remote.servers[0], "nats://a:4222"));
    CHECK(slice_is(core->subjects[0], "raw.>"));
    CHECK(slice_is(core->subjects[1], "replay.*"));
    CHECK(core->remote.name.len != 0);
    CHECK(slice_is(core->remote.name, "monoblok-import"));
    CHECK(core->origin_header);
    CHECK(core->max_pending != 0);
    CHECK(core->max_pending == 128);
    pb_program_free(&program);
}

static void test_nested_import_config_loads(void) {
    pb_program program = {0};
    load_program(&program, "(import\n"
                           "  :core [[ :servers [\"nats://core:4222\"]\n"
                           "           :subject [\"raw.>\"]\n"
                           "           :max-pending 64 ]]\n"
                           "  :streams [[ :servers [\"nats://js:4222\"]\n"
                           "              :subject [\"sensors.>\"]\n"
                           "              :stream \"SENSORS\"\n"
                           "              :consumer \"monoblok-sensors\"\n"
                           "              :catch-up true ]\n"
                           "            [ :servers [\"nats://js2:4222\"]\n"
                           "              :subject \"events.>\"\n"
                           "              :stream \"EVENTS\"\n"
                           "              :consumer \"monoblok-events\"\n"
                           "              :catch-up false ]])\n");
    CHECK(program.importer.present);
    CHECK(program.importer.cores_len == 1);
    CHECK(program.importer.streams_len == 2);
    CHECK(slice_is(program.importer.cores[0].remote.servers[0], "nats://core:4222"));
    CHECK(slice_is(program.importer.cores[0].subjects[0], "raw.>"));
    CHECK(program.importer.cores[0].max_pending != 0);
    CHECK(program.importer.cores[0].max_pending == 64);

    const pb_import_stream_config *sensors = &program.importer.streams[0];
    CHECK(slice_is(sensors->source.remote.servers[0], "nats://js:4222"));
    CHECK(slice_is(sensors->source.subjects[0], "sensors.>"));
    CHECK(slice_is(sensors->stream, "SENSORS"));
    CHECK(slice_is(sensors->consumer, "monoblok-sensors"));
        CHECK(sensors->catch_up);

    const pb_import_stream_config *events = &program.importer.streams[1];
    CHECK(slice_is(events->source.remote.servers[0], "nats://js2:4222"));
    CHECK(slice_is(events->source.subjects[0], "events.>"));
    CHECK(slice_is(events->stream, "EVENTS"));
    CHECK(slice_is(events->consumer, "monoblok-events"));
        CHECK(!events->catch_up);
    pb_program_free(&program);
}

static void test_yaml_nested_import_config_loads(void) {
    pb_program program = {0};
    const char *src =
        "import:\n"
        "  core:\n"
        "    - servers: [\"nats://core-yaml:4222\"]\n"
        "      subject: [\"raw.yaml.>\"]\n"
        "      max-pending: 32\n"
        "  streams:\n"
        "    - servers: [\"nats://js-yaml:4222\"]\n"
        "      subject: [\"sensors.yaml.>\"]\n"
        "      stream: \"SENSORS_YAML\"\n"
        "      consumer: \"monoblok-sensors-yaml\"\n"
        "      catch-up: true\n";
    CHECK(pb_program_load_source(&program, "nested-import.yml", src, strlen(src)));
    CHECK(program.importer.present);
    CHECK(program.importer.cores_len == 1);
    CHECK(program.importer.streams_len == 1);
    CHECK(slice_is(program.importer.cores[0].remote.servers[0], "nats://core-yaml:4222"));
    CHECK(slice_is(program.importer.cores[0].subjects[0], "raw.yaml.>"));
    CHECK(program.importer.cores[0].max_pending == 32);
    CHECK(slice_is(program.importer.streams[0].source.remote.servers[0], "nats://js-yaml:4222"));
    CHECK(slice_is(program.importer.streams[0].source.subjects[0], "sensors.yaml.>"));
    CHECK(slice_is(program.importer.streams[0].stream, "SENSORS_YAML"));
    CHECK(slice_is(program.importer.streams[0].consumer, "monoblok-sensors-yaml"));
    CHECK(program.importer.streams[0].catch_up);
    pb_program_free(&program);
}

static void test_import_requires_subject(void) {
    pb_program program = {0};
    const char *src = "(import :servers [\"nats://a:4222\"])\n";
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

static void test_nested_import_rejects_malformed_stream(void) {
    pb_program program = {0};
    const char *src = "(import :streams [[ :servers [\"nats://js:4222\"] :subject [\"sensors.>\"] :stream \"SENSORS\" ]])\n";
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

static void test_nested_import_rejects_multi_filter_stream(void) {
    pb_program program = {0};
    const char *src = "(import :streams [[ :servers [\"nats://js:4222\"] :subject [\"sensors.>\" \"events.>\"] :stream \"SENSORS\" :consumer \"monoblok\" ]])\n";
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

static bool user_call_stub(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_values args, pb_eval_result *out) {
    (void)user_ctx;
    (void)ctx;
    (void)name;
    (void)args;
    *out = (pb_eval_result){.err = PB_EVAL_OK, .value = {.kind = PB_NIL}};
    return true;
}

static void test_unknown_form_rejected_at_load(void) {
    // Bug: only the path taken by one sample evaluation was checked.
    const char *src = "(on \"t.>\" (when (> payload-float 5) (bogus 1 2)))";
    pb_program program = {0};
    CHECK(!pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);

    // The user_call hook owns unknown heads, so loading still works with it installed.
    program = (pb_program){0};
    pb_program_set_eval_hooks(&program, NULL, user_call_stub, NULL);
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    pb_program_free(&program);
}

static void test_same_form_twice_keeps_separate_state(void) {
    const char *src = "(on \"t.>\" (do (publish! \"out.a\" (squelch (json-get \"a\" payload)))"
                      "               (publish! \"out.b\" (squelch (json-get \"b\" payload)))))";
    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("out.*"), lit("1")));
    for (int i = 0; i < 3; i += 1) {
        CHECK(pb_program_eval_publish(&program, &router, lit("t.x"), lit("{\"a\":1,\"b\":2}"), 0, 0, (pb_program_eval_options){0}));
    }
    CHECK(count_msgs(&conn.out) == 2);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}
// `last` reads the router LVC: covered subjects return the latest payload,
// uncovered or never-published subjects return nil.
static void test_last_reads_router_lvc(void) {
    pb_program program = {0};
    load_program(&program,
                 "(lvc [\"temp.*\"])\n"
                 "(on \"q\" (publish! \"ans\" (if (last \"temp.outside\") (last \"temp.outside\") \"none\")))\n"
                 "(on \"q\" (publish! \"ans2\" (if (last \"other.x\") (last \"other.x\") \"none\")))\n"
                 "(on \"q\" (publish! \"ans3\" (if (last \"temp.never\") (last \"temp.never\") \"none\")))\n"
                 "(on \"q\" (publish! \"ans4\" (if (> (last \"temp.outside\") 30) \"hot\" \"mild\")))\n");
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[1] = {lit("temp.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("ans"), lit("1")));
    CHECK(mb_router_subscribe(&router, &conn, lit("ans2"), lit("2")));
    CHECK(mb_router_subscribe(&router, &conn, lit("ans3"), lit("3")));
    CHECK(mb_router_subscribe(&router, &conn, lit("ans4"), lit("4")));
    CHECK(mb_router_publish(&router, lit("temp.outside"), lit("31")));
    CHECK(mb_router_publish(&router, lit("other.x"), lit("7")));
    CHECK(pb_program_eval_publish(&program, &router, lit("q"), lit(""), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG ans 1 2\r\n31\r\n"));
    CHECK(buf_contains(&conn.out, "MSG ans2 2 4\r\nnone\r\n"));
    CHECK(buf_contains(&conn.out, "MSG ans3 3 4\r\nnone\r\n"));
    CHECK(buf_contains(&conn.out, "MSG ans4 4 3\r\nhot\r\n"));

    // A newer value replaces the older one.
    CHECK(mb_router_publish(&router, lit("temp.outside"), lit("12")));
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};
    CHECK(pb_program_eval_publish(&program, &router, lit("q"), lit(""), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG ans 1 2\r\n12\r\n"));
    CHECK(buf_contains(&conn.out, "MSG ans4 4 4\r\nmild\r\n"));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

// Without any (lvc ...) filter the router keeps nothing, so `last` is always nil.
static void test_last_is_nil_without_lvc(void) {
    pb_program program = {0};
    load_program(&program, "(on \"q\" (publish! \"ans\" (if (last \"temp.outside\") \"seen\" \"none\")))\n");
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("ans"), lit("1")));
    CHECK(mb_router_publish(&router, lit("temp.outside"), lit("31")));
    CHECK(pb_program_eval_publish(&program, &router, lit("q"), lit(""), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG ans 1 4\r\nnone\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

// A client publish reaches the LVC before its rules run, so (last subject) is the current payload.
static void test_last_of_current_subject_is_current_payload(void) {
    pb_program program = {0};
    load_program(&program, "(lvc [\"temp.*\"])\n(on \"temp.*\" (publish! \"seen\" (last subject)))\n");
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[1] = {lit("temp.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("seen"), lit("1")));
    CHECK(mb_router_publish(&router, lit("temp.a"), lit("5")));
    CHECK(pb_program_eval_publish(&program, &router, lit("temp.a"), lit("5"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG seen 1 1\r\n5\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

// A re-entrant rule funnels several subjects onto one subject, so the second rule keeps one slot.
static void test_reentrant_funnel_rollup(void) {
    pb_program program = {0};
    load_program(&program, "(on \"temp.*\" :reentrant true (publish! \"temp.all\" payload))\n"
                           "(on \"temp.all\" (publish! \"temp.sum\" (moving-sum 100 payload-float)))\n");
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("temp.sum"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("temp.a"), lit("1"), 0, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_eval_publish(&program, &router, lit("temp.b"), lit("2"), 0, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_eval_publish(&program, &router, lit("temp.c"), lit("4"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG temp.sum 1 1\r\n1\r\n"));
    CHECK(buf_contains(&conn.out, "MSG temp.sum 1 1\r\n3\r\n"));
    CHECK(buf_contains(&conn.out, "MSG temp.sum 1 1\r\n7\r\n"));
    CHECK(program.rules[1].state.len == 1);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static bool tick_at(pb_program *program, mb_router *router, uint64_t now_ms) {
    return pb_program_tick(program, router, now_ms, 0, (pb_program_eval_options){0});
}

// An every rule is scheduled by its first tick (now + ms) without firing, fires on
// each interval, and fires once (no burst) after a long stall, realigning to now + ms.
static void test_every_schedules_then_fires_without_burst(void) {
    pb_program program = {0};
    load_program(&program, "(every :ms 100 (publish! \"tick\" (str-concat \"[\" subject payload \"]\")))\n");
    CHECK(program.len == 1 && program.rules[0].every_ms == 100);
    CHECK(program.uses_clock_timer);

    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("tick"), lit("1")));

    uint64_t deadline = 123;
    CHECK(pb_program_next_clock_deadline(&program, &deadline) && deadline == 0); // not yet scheduled
    CHECK(tick_at(&program, &router, 1000));
    CHECK(count_msgs(&conn.out) == 0);
    CHECK(pb_program_next_clock_deadline(&program, &deadline) && deadline == 1100);
    CHECK(tick_at(&program, &router, 1099));
    CHECK(count_msgs(&conn.out) == 0);
    CHECK(tick_at(&program, &router, 1100));
    CHECK(count_msgs(&conn.out) == 1);
    CHECK(buf_contains(&conn.out, "MSG tick 1 2\r\n[]\r\n")); // subject and payload are empty strings
    CHECK(pb_program_next_clock_deadline(&program, &deadline) && deadline == 1200);

    CHECK(tick_at(&program, &router, 9000)); // stalled: one fire, realigned
    CHECK(count_msgs(&conn.out) == 2);
    CHECK(pb_program_next_clock_deadline(&program, &deadline) && deadline == 9100);
    CHECK(tick_at(&program, &router, 9050));
    CHECK(count_msgs(&conn.out) == 2);

    // tick_until drains due deadlines in event time and schedules a fresh program at the event time.
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};
    pb_program second = {0};
    load_program(&second, "(every :ms 100 (publish! \"tick\" \"t\"))\n");
    CHECK(pb_program_tick_until(&second, &router, 5000, 0, (pb_program_eval_options){0}));
    CHECK(count_msgs(&conn.out) == 0);
    CHECK(pb_program_next_clock_deadline(&second, &deadline) && deadline == 5100);
    CHECK(pb_program_tick_until(&second, &router, 5350, 0, (pb_program_eval_options){0}));
    CHECK(count_msgs(&conn.out) == 3); // 5100, 5200, 5300 replayed in event time

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&second);
    pb_program_free(&program);
}

// every rules have no filter: they are not in the dispatch index and no publish evaluates them.
static void test_every_is_never_evaluated_for_publishes(void) {
    pb_program program = {0};
    load_program(&program, "(every :ms 100 (publish! \"tick\" \"t\"))\n"
                           "(on \"a\" (publish! \"b\" payload))\n"
                           "(on \">\" (publish! \"any\" payload))\n");
    CHECK(program.len == 3);
    CHECK(program.rule_global.len == 1 && program.rule_global.items[0] == 2);
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("tick"), lit("1")));
    CHECK(mb_router_subscribe(&router, &conn, lit("b"), lit("2")));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_eval_publish(&program, &router, lit(""), lit("x"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG b 2 1\r\nx\r\n"));
    CHECK(!buf_contains(&conn.out, "MSG tick"));
    CHECK(program.rules[0].publishes_emitted == 0);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static void test_every_multiple_bodies_and_reentrant(void) {
    pb_program program = {0};
    load_program(&program, "(every :ms 10 :reentrant true (publish! \"a\" \"1\") (publish! \"c\" \"3\"))\n"
                           "(on \"a\" (publish! \"b\" payload))\n");
    CHECK(program.rules[0].reentrant);
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(tick_at(&program, &router, 0));
    CHECK(tick_at(&program, &router, 10));
    CHECK(buf_contains(&conn.out, "MSG a 1 1\r\n1\r\n"));
    CHECK(buf_contains(&conn.out, "MSG c 1 1\r\n3\r\n"));
    CHECK(buf_contains(&conn.out, "MSG b 1 1\r\n1\r\n")); // re-entry reached the on rule
    CHECK(program.rules[0].publishes_emitted == 2);
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);

    // Without :reentrant the emission does not reach the on rule.
    pb_program plain = {0};
    load_program(&plain, "(every :ms 10 (publish! \"a\" \"1\"))\n(on \"a\" (publish! \"b\" payload))\n");
    mb_router router2;
    mb_router_init(&router2);
    mb_router_conn conn2 = {0};
    CHECK(mb_router_subscribe(&router2, &conn2, lit("b"), lit("1")));
    CHECK(tick_at(&plain, &router2, 0));
    CHECK(tick_at(&plain, &router2, 10));
    CHECK(count_msgs(&conn2.out) == 0);
    mb_buf_free(&conn2.out);
    mb_router_free(&router2);
    pb_program_free(&plain);
}

static void test_every_load_errors(void) {
    const char *bad[] = {
        "(on \"a\" (every :ms 5 (publish! \"x\" \"y\")))\n",       // only top level
        "(on \"a\" (do (when true (every :ms 5 1))))\n",            // nested deeper
        "(every :ms 0 (publish! \"x\" \"y\"))\n",                   // ms must be positive
        "(every :ms 1.5 (publish! \"x\" \"y\"))\n",                 // ms must be an integer
        "(every :ms \"100\" (publish! \"x\" \"y\"))\n",             // ms must be a number
        "(every :ms 100)\n",                                        // no body
        "(every 100 (publish! \"x\" \"y\"))\n",                     // missing :ms
        "(every :ms 100 :bogus true (publish! \"x\" \"y\"))\n",     // unknown option
        "(every :ms 100 :reentrant 1 (publish! \"x\" \"y\"))\n",    // option type
        "(every :ms 100 (bogus 1))\n",                              // unknown form in body
        "(every :ms 100 (publish! \"x\" nonsense))\n",              // unknown symbol in body
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i += 1) {
        pb_program program = {0};
        CHECK(!pb_program_load_source(&program, "<test>", bad[i], strlen(bad[i])));
    }
}

// Slots armed under an every body see the empty subject and payload at clock time.
static void test_every_clock_slot_has_empty_subject(void) {
    pb_program program = {0};
    load_program(&program, "(every :ms 100 (on-silence :ms 50 (publish! \"quiet\" (str-concat \"[\" subject payload \"]\"))))\n");
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("quiet"), lit("1")));
    CHECK(tick_at(&program, &router, 1000));
    CHECK(tick_at(&program, &router, 1100)); // every fires and arms on-silence for 1150
    CHECK(count_msgs(&conn.out) == 0);
    CHECK(program.rules[0].state.len == 2);
    CHECK(tick_at(&program, &router, 1150));
    CHECK(buf_contains(&conn.out, "MSG quiet 1 2\r\n[]\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

// Stateful forms share the empty key inside every.
static void test_every_stateful_forms_share_empty_key(void) {
    pb_program program = {0};
    load_program(&program, "(every :ms 10 (publish! \"n\" (moving-sum 100 1)))\n");
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("n"), lit("1")));
    CHECK(tick_at(&program, &router, 0));
    CHECK(tick_at(&program, &router, 10));
    CHECK(tick_at(&program, &router, 20));
    CHECK(buf_contains(&conn.out, "MSG n 1 1\r\n1\r\n"));
    CHECK(buf_contains(&conn.out, "MSG n 1 1\r\n2\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

// every can read `last` and `now`.
static void test_every_reads_last_and_now(void) {
    pb_program program = {0};
    load_program(&program, "(lvc [\"temp.*\"])\n(every :ms 10 (publish! \"out\" (last \"temp.a\")))\n");
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[1] = {lit("temp.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("out"), lit("1")));
    CHECK(mb_router_publish(&router, lit("temp.a"), lit("77")));
    CHECK(tick_at(&program, &router, 0));
    CHECK(tick_at(&program, &router, 10));
    CHECK(buf_contains(&conn.out, "MSG out 1 2\r\n77\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

static pb_program_eval_options with_reply(const char *reply) {
    return (pb_program_eval_options){.reply_to = lit(reply)};
}

// A request-reply round trip: reply! publishes the answer to the request's reply subject.
static void test_reply_answers_request(void) {
    pb_program program = {0};
    load_program(&program, "(lvc [\"temp.*\"])\n"
                           "(on \"q.avg\" (reply! (last \"temp.kitchen\")))\n"
                           "(on \"q.thread\" (-> (last \"temp.kitchen\") (reply!)))\n"
                           "(on \"q.who\" (publish! \"seen\" (if reply-to reply-to \"none\")))\n");
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[1] = {lit("temp.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("_INBOX.1"), lit("1")));
    CHECK(mb_router_subscribe(&router, &conn, lit("seen"), lit("2")));
    CHECK(mb_router_publish(&router, lit("temp.kitchen"), lit("21.5")));

    CHECK(pb_program_eval_publish(&program, &router, lit("q.avg"), lit(""), 0, 0, with_reply("_INBOX.1")));
    CHECK(buf_contains(&conn.out, "MSG _INBOX.1 1 4\r\n21.5\r\n"));
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};
    CHECK(pb_program_eval_publish(&program, &router, lit("q.thread"), lit(""), 0, 0, with_reply("_INBOX.1")));
    CHECK(buf_contains(&conn.out, "MSG _INBOX.1 1 4\r\n21.5\r\n"));
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};

    // No reply subject: nothing is published, the rule is counted as suppressed, reply-to is nil.
    CHECK(pb_program_eval_publish(&program, &router, lit("q.avg"), lit(""), 0, 0, (pb_program_eval_options){0}));
    CHECK(count_msgs(&conn.out) == 0);
    CHECK(program.rules[0].publishes_emitted == 1 && program.rules[0].publishes_suppressed == 1);
    CHECK(pb_program_eval_publish(&program, &router, lit("q.who"), lit(""), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG seen 2 4\r\nnone\r\n"));
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};
    CHECK(pb_program_eval_publish(&program, &router, lit("q.who"), lit(""), 0, 0, with_reply("_INBOX.9")));
    CHECK(buf_contains(&conn.out, "MSG seen 2 8\r\n_INBOX.9\r\n"));

    // Nothing in the LVC yet for an unknown subject: nil value, no reply, no failure.
    pb_program empty = {0};
    load_program(&empty, "(lvc [\"temp.*\"])\n(on \"q\" (reply! (last \"temp.none\")))\n");
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};
    CHECK(pb_program_eval_publish(&empty, &router, lit("q"), lit(""), 0, 0, with_reply("_INBOX.1")));
    CHECK(count_msgs(&conn.out) == 0);
    CHECK(empty.rules[0].publishes_suppressed == 1);

    // A hostile reply subject fails like a bad publish! target.
    pb_program bad = {0};
    load_program(&bad, "(on \"q\" (reply! \"x\"))\n");
    CHECK(!pb_program_eval_publish(&bad, &router, lit("q"), lit(""), 0, 0, with_reply("$LVC.temp.kitchen")));
    CHECK(!pb_program_eval_publish(&bad, &router, lit("q"), lit(""), 0, 0, with_reply("$STATS.x")));

    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&bad);
    pb_program_free(&empty);
    pb_program_free(&program);
}

// Re-entered evaluation of a rule's own emission carries no reply subject.
static void test_reentry_has_no_reply_subject(void) {
    pb_program program = {0};
    load_program(&program, "(on \"a\" :reentrant true (publish! \"b\" payload))\n"
                           "(on \"b\" (publish! \"r\" (if reply-to \"yes\" \"no\")))\n");
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit("r"), lit("1")));
    CHECK(pb_program_eval_publish(&program, &router, lit("a"), lit("x"), 0, 0, with_reply("_INBOX.1")));
    CHECK(buf_contains(&conn.out, "MSG r 1 2\r\nno\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);
}

// JSON patchbay with every, reply!/reply-to, last, hysteresis and ewma.
static void test_json_every_reply_and_new_forms(void) {
    const char *src =
        "[\n"
        "  [\"lvc\", \"temp.*\"],\n"
        "  [\"every\", {\"ms\": 100}, [\"publish!\", \"tick\", [\"last\", \"temp.a\"]]],\n"
        "  [\"every\", {\"ms\": 100, \"reentrant\": false}, [\"publish!\", \"tick2\", \"x\"], [\"publish!\", \"tick3\", \"y\"]],\n"
        "  [\"on\", \"q\", [\"reply!\", [\"last\", \"temp.a\"]]],\n"
        "  [\"on\", \"q.who\", [\"publish!\", \"who\", [\"if\", \"$reply-to\", \"$reply-to\", \"none\"]]],\n"
        "  [\"on\", \"temp.*\", [\"do\",\n"
        "    [\"publish!\", \"sm\", [\"ewma\", 0.5, \"$payload-float\"]],\n"
        "    [\"publish!\", \"alarm\", [\"hysteresis\", 20, 25, \"$payload-float\"]]]]\n"
        "]\n";
    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "t.json", src, strlen(src)));
    CHECK(program.len == 5 && program.rules[0].every_ms == 100 && program.rules[1].every_ms == 100);
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[1] = {lit("temp.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(mb_router_publish(&router, lit("temp.a"), lit("30")));
    CHECK(pb_program_eval_publish(&program, &router, lit("temp.a"), lit("30"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG sm 1 2\r\n30\r\n"));
    CHECK(buf_contains(&conn.out, "MSG alarm 1 4\r\ntrue\r\n"));
    CHECK(pb_program_eval_publish(&program, &router, lit("q"), lit(""), 0, 0, with_reply("_INBOX.5")));
    CHECK(buf_contains(&conn.out, "MSG _INBOX.5 1 2\r\n30\r\n"));
    CHECK(pb_program_eval_publish(&program, &router, lit("q.who"), lit(""), 0, 0, with_reply("_INBOX.6")));
    CHECK(buf_contains(&conn.out, "MSG who 1 8\r\n_INBOX.6\r\n"));
    CHECK(tick_at(&program, &router, 0));
    CHECK(tick_at(&program, &router, 100));
    CHECK(buf_contains(&conn.out, "MSG tick 1 2\r\n30\r\n"));
    CHECK(buf_contains(&conn.out, "MSG tick2 1 1\r\nx\r\n"));
    CHECK(buf_contains(&conn.out, "MSG tick3 1 1\r\ny\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);

    const char *bad = "[[\"on\", \"a\", [\"every\", {\"ms\": 5}, [\"publish!\", \"x\", \"y\"]]]]\n";
    CHECK(!pb_program_load_source(&program, "bad.json", bad, strlen(bad)));
}

// YAML patchbay with an every: list, reply!/reply-to, last, hysteresis and ewma.
static void test_yaml_every_reply_and_new_forms(void) {
    const char *src =
        "lvc:\n"
        "  - \"temp.*\"\n"
        "every:\n"
        "  - ms: 100\n"
        "    form: [publish!, \"tick\", [last, \"temp.a\"]]\n"
        "  - ms: 100\n"
        "    reentrant: true\n"
        "    do:\n"
        "      - [publish!, \"tick2\", \"x\"]\n"
        "      - thread:\n"
        "          - \"y\"\n"
        "          - [publish!, \"tick3\"]\n"
        "on:\n"
        "  - sub: \"q\"\n"
        "    form: [reply!, dp, 1, [\"+\", 0, [last, \"temp.a\"]]]\n"
        "  - sub: \"q.who\"\n"
        "    form: [publish!, \"who\", [if, $reply-to, $reply-to, \"none\"]]\n"
        "  - sub: \"temp.*\"\n"
        "    do:\n"
        "      - [publish!, \"sm\", [ewma, 0.5, $payload-float]]\n"
        "      - [publish!, \"alarm\", [hysteresis, 20, 25, $payload-float]]\n";
    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "t.yml", src, strlen(src)));
    CHECK(program.len == 5);
    CHECK(program.rules[0].every_ms == 100 && !program.rules[0].reentrant);
    CHECK(program.rules[1].every_ms == 100 && program.rules[1].reentrant);
    mb_router router;
    mb_router_init(&router);
    mb_slice filters[1] = {lit("temp.*")};
    CHECK(mb_router_configure_lvc(&router, filters, 1));
    mb_router_conn conn = {0};
    CHECK(mb_router_subscribe(&router, &conn, lit(">"), lit("1")));
    CHECK(mb_router_publish(&router, lit("temp.a"), lit("30")));
    CHECK(pb_program_eval_publish(&program, &router, lit("temp.a"), lit("30"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&conn.out, "MSG sm 1 2\r\n30\r\n"));
    CHECK(buf_contains(&conn.out, "MSG alarm 1 4\r\ntrue\r\n"));
    CHECK(pb_program_eval_publish(&program, &router, lit("q.who"), lit(""), 0, 0, with_reply("_INBOX.6")));
    CHECK(buf_contains(&conn.out, "MSG who 1 8\r\n_INBOX.6\r\n"));
    CHECK(tick_at(&program, &router, 0));
    CHECK(tick_at(&program, &router, 100));
    CHECK(buf_contains(&conn.out, "MSG tick 1 2\r\n30\r\n"));
    CHECK(buf_contains(&conn.out, "MSG tick2 1 1\r\nx\r\n"));
    CHECK(buf_contains(&conn.out, "MSG tick3 1 1\r\ny\r\n"));
    mb_buf_free(&conn.out);
    conn.out = (mb_buf){0};
    CHECK(mb_router_publish(&router, lit("temp.a"), lit("21.37")));
    CHECK(pb_program_eval_publish(&program, &router, lit("q"), lit(""), 0, 0, with_reply("_INBOX.5")));
    CHECK(buf_contains(&conn.out, "MSG _INBOX.5 1 4\r\n21.4\r\n"));
    mb_buf_free(&conn.out);
    mb_router_free(&router);
    pb_program_free(&program);

    const char *bad_ms = "every:\n  - ms: abc\n    form: [publish!, \"x\", \"y\"]\n";
    CHECK(!pb_program_load_source(&program, "bad.yml", bad_ms, strlen(bad_ms)));
    const char *no_ms = "every:\n  - form: [publish!, \"x\", \"y\"]\n";
    CHECK(!pb_program_load_source(&program, "bad2.yml", no_ms, strlen(no_ms)));
}

TEST_MAIN(program,
          test_reentry_is_opt_in,
          test_last_reads_router_lvc,
          test_last_is_nil_without_lvc,
          test_last_of_current_subject_is_current_payload,
          test_reentrant_funnel_rollup,
          test_reentry_runs_downstream_rules,
          test_reentry_eval_failure_does_not_fail_publish,
          test_reentry_depth_cap,
          test_rule_dispatch_preserves_source_order,
          test_rule_stats_count_emits_and_suppression,
          test_replaying_publish_option,
          test_tick_until_drains_due_deadlines,
          test_time_window_deadline_uses_inclusive_cutoff,
          test_lvc_filters_load,
          test_export_config_loads,
          test_config_env_values_load,
          test_config_negative_and_absent_remote_values,
          test_missing_config_env_fails,
          test_yaml_replaying_alias,
          test_yaml_config_env_values_load,
          test_deprecated_bridge_config_loads,
          test_export_requires_servers,
          test_import_config_loads,
          test_nested_import_config_loads,
          test_yaml_nested_import_config_loads,
          test_import_requires_subject,
          test_nested_import_rejects_malformed_stream,
          test_nested_import_rejects_multi_filter_stream,
          test_unknown_form_rejected_at_load,
          test_same_form_twice_keeps_separate_state,
          test_every_schedules_then_fires_without_burst,
          test_every_is_never_evaluated_for_publishes,
          test_every_multiple_bodies_and_reentrant,
          test_every_load_errors,
          test_every_clock_slot_has_empty_subject,
          test_every_stateful_forms_share_empty_key,
          test_every_reads_last_and_now,
          test_reply_answers_request,
          test_reentry_has_no_reply_subject,
          test_json_every_reply_and_new_forms,
          test_yaml_every_reply_and_new_forms)
