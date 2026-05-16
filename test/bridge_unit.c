#include "bridge.h"
#include "nats.h"
#include "test_check.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static pb_slice pbs(const char *s) {
    return (pb_slice){.ptr = s, .len = strlen(s)};
}

static mb_slice mbs(const char *s) {
    return (mb_slice){.ptr = (const uint8_t *)s, .len = strlen(s)};
}

static void check_cstr(const char *got, const char *want) {
    CHECK(strcmp(got, want) == 0);
}

static pb_bridge_config basic_config(pb_slice *servers, size_t servers_len, pb_slice *exports, size_t exports_len) {
    return (pb_bridge_config){
        .servers = servers,
        .servers_len = servers_len,
        .exports = exports,
        .exports_len = exports_len,
        .present = true,
    };
}

static void test_bridge_absent_config_is_noop(void) {
    fake_nats_reset();
    mb_bridge bridge = {0};

    CHECK(mb_bridge_start(&bridge, NULL));
    CHECK(!bridge.started);
    CHECK(fake_nats_get()->options_create_calls == 0);

    mb_bridge_publish(&bridge, mbs("telemetry.temp"), mbs("1"));
    CHECK(fake_nats_get()->publish_calls == 0);
    CHECK(fake_nats_get()->publish_msg_calls == 0);

    mb_bridge_close(&bridge);
    CHECK(fake_nats_get()->close_calls == 0);
}

static void test_bridge_publish_filters_and_counts(void) {
    fake_nats_reset();
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    mb_bridge bridge = {0};

    CHECK(mb_bridge_start(&bridge, &config));
    CHECK(bridge.started);
    CHECK(fake_nats_get()->connect_calls == 1);
    CHECK(fake_nats_get()->header_support_calls == 0);

    mb_bridge_publish(&bridge, mbs("unrelated.temp"), mbs("99"));
    CHECK(fake_nats_get()->publish_calls == 0);
    CHECK(bridge.published == 0);
    CHECK(bridge.dropped == 0);

    mb_bridge_publish(&bridge, mbs("telemetry.temp"), mbs("42"));
    CHECK(fake_nats_get()->publish_calls == 1);
    CHECK(bridge.published == 1);
    CHECK(bridge.dropped == 0);
    check_cstr(fake_nats_get()->last_publish_subject, "telemetry.temp");
    check_cstr(fake_nats_get()->last_publish_payload, "42");

    char long_subject[128] = "telemetry.";
    memset(long_subject + strlen("telemetry."), 'a', 90);
    long_subject[strlen("telemetry.") + 90] = '\0';
    mb_bridge_publish(&bridge, mbs(long_subject), mbs("x"));
    CHECK(fake_nats_get()->publish_calls == 2);
    CHECK(bridge.published == 2);
    check_cstr(fake_nats_get()->last_publish_subject, long_subject);

    mb_bridge_close(&bridge);
    CHECK(fake_nats_get()->connection_destroy_calls == 1);
    CHECK(fake_nats_get()->close_calls == 1);
}

static void test_bridge_origin_header_publish_destroys_message(void) {
    fake_nats_reset();
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    config.origin_header = true;
    mb_bridge bridge = {0};

    CHECK(mb_bridge_start(&bridge, &config));
    CHECK(fake_nats_get()->header_support_calls == 1);

    mb_bridge_publish(&bridge, mbs("telemetry.temp"), mbs("42"));
    CHECK(fake_nats_get()->msg_create_calls == 1);
    CHECK(fake_nats_get()->msg_header_set_calls == 1);
    CHECK(fake_nats_get()->publish_msg_calls == 1);
    CHECK(fake_nats_get()->msg_destroy_calls == 1);
    CHECK(bridge.published == 1);
    CHECK(bridge.dropped == 0);
    check_cstr(fake_nats_get()->last_msg_subject, "telemetry.temp");
    check_cstr(fake_nats_get()->last_msg_payload, "42");
    check_cstr(fake_nats_get()->last_header_name, "x-monoblok");
    CHECK(fake_nats_get()->last_header_value[0] != '\0');

    mb_bridge_close(&bridge);
}

static void test_bridge_publish_failures_are_counted(void) {
    fake_nats_reset();
    fake_nats_get()->publish_status = NATS_NOT_CONNECTED;
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    mb_bridge bridge = {0};

    CHECK(mb_bridge_start(&bridge, &config));
    mb_bridge_publish(&bridge, mbs("telemetry.temp"), mbs("42"));
    CHECK(fake_nats_get()->publish_calls == 1);
    CHECK(bridge.published == 0);
    CHECK(bridge.dropped == 1);
    CHECK(bridge.last_status == NATS_NOT_CONNECTED);

    mb_bridge_close(&bridge);
}

static void test_bridge_origin_header_failure_destroys_message(void) {
    fake_nats_reset();
    fake_nats_get()->msg_header_set_status = NATS_ERR;
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    config.origin_header = true;
    mb_bridge bridge = {0};

    CHECK(mb_bridge_start(&bridge, &config));
    mb_bridge_publish(&bridge, mbs("telemetry.temp"), mbs("42"));
    CHECK(fake_nats_get()->msg_create_calls == 1);
    CHECK(fake_nats_get()->msg_header_set_calls == 1);
    CHECK(fake_nats_get()->publish_msg_calls == 0);
    CHECK(fake_nats_get()->msg_destroy_calls == 1);
    CHECK(bridge.published == 0);
    CHECK(bridge.dropped == 1);
    CHECK(bridge.last_status == NATS_ERR);

    mb_bridge_close(&bridge);
}

static void test_bridge_drops_oversized_payload_before_nats(void) {
    fake_nats_reset();
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    mb_bridge bridge = {0};

    CHECK(mb_bridge_start(&bridge, &config));
    mb_bridge_publish(&bridge, mbs("telemetry.temp"),
                      (mb_slice){.ptr = (const uint8_t *)"x", .len = (size_t)INT_MAX + 1});
    CHECK(fake_nats_get()->publish_calls == 0);
    CHECK(fake_nats_get()->msg_create_calls == 0);
    CHECK(bridge.published == 0);
    CHECK(bridge.dropped == 1);

    mb_bridge_close(&bridge);
}

static void test_bridge_start_config_failure_leaves_bridge_closed(void) {
    fake_nats_reset();
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    config.has_user = true;
    config.user = pbs("user");
    mb_bridge bridge = {0};

    CHECK(!mb_bridge_start(&bridge, &config));
    CHECK(!bridge.started);
    CHECK(bridge.conn == NULL);
    CHECK(fake_nats_get()->options_create_calls == 1);
    CHECK(fake_nats_get()->options_destroy_calls == 1);
    CHECK(fake_nats_get()->connect_calls == 0);

    mb_bridge_close(&bridge);
    CHECK(fake_nats_get()->close_calls == 0);
}

static void test_bridge_start_connect_failure_destroys_partial_conn(void) {
    fake_nats_reset();
    fake_nats_get()->connect_status = NATS_TIMEOUT;
    fake_nats_get()->connect_returns_conn_on_failure = true;
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    mb_bridge bridge = {0};

    CHECK(!mb_bridge_start(&bridge, &config));
    CHECK(!bridge.started);
    CHECK(bridge.conn == NULL);
    CHECK(fake_nats_get()->connect_calls == 1);
    CHECK(fake_nats_get()->connection_destroy_calls == 1);
    CHECK(fake_nats_get()->close_calls == 1);

    mb_bridge_close(&bridge);
    CHECK(fake_nats_get()->close_calls == 1);
}

static void test_bridge_start_header_support_failure_destroys_conn(void) {
    fake_nats_reset();
    fake_nats_get()->header_support_status = NATS_ERR;
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice exports[] = {pbs("telemetry.>")};
    pb_bridge_config config = basic_config(servers, 1, exports, 1);
    config.origin_header = true;
    mb_bridge bridge = {0};

    CHECK(!mb_bridge_start(&bridge, &config));
    CHECK(!bridge.started);
    CHECK(bridge.conn == NULL);
    CHECK(fake_nats_get()->connect_calls == 1);
    CHECK(fake_nats_get()->header_support_calls == 1);
    CHECK(fake_nats_get()->connection_destroy_calls == 1);
    CHECK(fake_nats_get()->close_calls == 1);

    mb_bridge_close(&bridge);
    CHECK(fake_nats_get()->close_calls == 1);
}

TEST_MAIN(bridge,
          test_bridge_absent_config_is_noop,
          test_bridge_publish_filters_and_counts,
          test_bridge_origin_header_publish_destroys_message,
          test_bridge_publish_failures_are_counted,
          test_bridge_origin_header_failure_destroys_message,
          test_bridge_drops_oversized_payload_before_nats,
          test_bridge_start_config_failure_leaves_bridge_closed,
          test_bridge_start_connect_failure_destroys_partial_conn,
          test_bridge_start_header_support_failure_destroys_conn)
