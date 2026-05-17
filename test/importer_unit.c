#include "importer.h"
#include "nats.h"
#include "test_check.h"

#include <string.h>

typedef struct seen_import {
    size_t count;
    char subject[128];
    char payload[128];
} seen_import;

static pb_slice pbs(const char *s) {
    return (pb_slice){.ptr = s, .len = strlen(s)};
}

static pb_import_config basic_config(pb_slice *servers, size_t servers_len, pb_slice *subjects, size_t subjects_len) {
    return (pb_import_config){
        .servers = servers,
        .servers_len = servers_len,
        .subjects = subjects,
        .subjects_len = subjects_len,
        .present = true,
    };
}

static bool copy_seen(void *ctx, mb_slice subject, mb_slice payload) {
    seen_import *seen = ctx;
    seen->count += 1;
    size_t n = subject.len < sizeof seen->subject - 1 ? subject.len : sizeof seen->subject - 1;
    memcpy(seen->subject, subject.ptr, n);
    seen->subject[n] = '\0';
    n = payload.len < sizeof seen->payload - 1 ? payload.len : sizeof seen->payload - 1;
    memcpy(seen->payload, payload.ptr, n);
    seen->payload[n] = '\0';
    return true;
}

static bool wait_for_seen(uv_loop_t *loop, const seen_import *seen, size_t want) {
    for (size_t i = 0; i < 200; i += 1) {
        (void)uv_run(loop, UV_RUN_NOWAIT);
        if (seen->count >= want) {
            return true;
        }
        uv_sleep(1);
    }
    return false;
}

static bool wait_for_ring_len(mb_importer *importer, size_t want) {
    for (size_t i = 0; i < 200; i += 1) {
        uv_mutex_lock(&importer->lock);
        const size_t len = importer->len;
        uv_mutex_unlock(&importer->lock);
        if (len == want) {
            return true;
        }
        uv_sleep(1);
    }
    return false;
}

static bool wait_for_pending_drop(mb_importer *importer) {
    for (size_t i = 0; i < 200; i += 1) {
        uv_mutex_lock(&importer->lock);
        const uint64_t dropped = importer->pending_dropped;
        uv_mutex_unlock(&importer->lock);
        if (dropped != 0) {
            return true;
        }
        uv_sleep(1);
    }
    return false;
}

static void close_importer_loop(mb_importer *importer, uv_loop_t *loop) {
    mb_importer_close(importer);
    (void)uv_run(loop, UV_RUN_DEFAULT);
    CHECK(uv_loop_close(loop) == 0);
}

static void test_importer_subscribes_and_drains_on_loop(void) {
    fake_nats_reset();
    uv_loop_t loop;
    CHECK(uv_loop_init(&loop) == 0);
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice subjects[] = {pbs("raw.>")};
    pb_import_config config = basic_config(servers, 1, subjects, 1);
    seen_import seen = {0};
    mb_importer importer = {0};

    CHECK(mb_importer_start(&importer, &loop, &config, copy_seen, &seen));
    CHECK(importer.started);
    CHECK(fake_nats_get()->connect_calls == 1);
    CHECK(fake_nats_get()->subscribe_sync_calls == 1);

    fake_nats_deliver("raw.temp", "42");
    CHECK(wait_for_seen(&loop, &seen, 1));
    CHECK(strcmp(seen.subject, "raw.temp") == 0);
    CHECK(strcmp(seen.payload, "42") == 0);
    CHECK(importer.received == 1);
    CHECK(importer.processed == 1);
    CHECK(importer.dropped == 0);

    close_importer_loop(&importer, &loop);
    CHECK(fake_nats_get()->subscription_destroy_calls == 1);
    CHECK(fake_nats_get()->connection_destroy_calls == 1);
    CHECK(fake_nats_get()->close_calls == 1);
}

static void test_importer_bounds_pending_ring(void) {
    fake_nats_reset();
    uv_loop_t loop;
    CHECK(uv_loop_init(&loop) == 0);
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice subjects[] = {pbs("raw.>")};
    pb_import_config config = basic_config(servers, 1, subjects, 1);
    config.max_pending = 1;
    config.has_max_pending = true;
    seen_import seen = {0};
    mb_importer importer = {0};

    CHECK(mb_importer_start(&importer, &loop, &config, copy_seen, &seen));
    fake_nats_deliver("raw.one", "1");
    CHECK(wait_for_ring_len(&importer, 1));
    fake_nats_deliver("raw.two", "2");
    CHECK(wait_for_pending_drop(&importer));
    CHECK(wait_for_seen(&loop, &seen, 1));
    CHECK(strcmp(seen.subject, "raw.one") == 0);
    CHECK(importer.received == 2);
    CHECK(importer.processed == 1);
    CHECK(importer.dropped == 1);

    close_importer_loop(&importer, &loop);
}

static void test_importer_origin_header_drops_looped_messages(void) {
    fake_nats_reset();
    fake_nats_get()->header_get_status = NATS_OK;
    fake_nats_get()->next_msg_has_origin_header = true;
    uv_loop_t loop;
    CHECK(uv_loop_init(&loop) == 0);
    pb_slice servers[] = {pbs("nats://127.0.0.1:4222")};
    pb_slice subjects[] = {pbs(">")};
    pb_import_config config = basic_config(servers, 1, subjects, 1);
    config.origin_header = true;
    seen_import seen = {0};
    mb_importer importer = {0};

    CHECK(mb_importer_start(&importer, &loop, &config, copy_seen, &seen));
    CHECK(fake_nats_get()->header_support_calls == 1);
    fake_nats_deliver("clean.temp", "42");
    CHECK(wait_for_pending_drop(&importer));
    (void)uv_run(&loop, UV_RUN_NOWAIT);
    CHECK(seen.count == 0);
    CHECK(importer.received == 1);
    CHECK(importer.dropped == 1);

    close_importer_loop(&importer, &loop);
}

TEST_MAIN(importer,
          test_importer_subscribes_and_drains_on_loop,
          test_importer_bounds_pending_ring,
          test_importer_origin_header_drops_looped_messages)
