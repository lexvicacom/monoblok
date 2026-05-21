#define _POSIX_C_SOURCE 200809L

#include "jetstream.h"

#include "nats_common.h"

#include "nats.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MB_JS_CATCHUP_BATCH = 64,
    MB_JS_LIVE_FETCH_TIMEOUT_MS = 1000,
    MB_JS_CATCHUP_FETCH_TIMEOUT_MS = 1000,
};

// One configured JetStream source plus its live handoff state.
struct mb_js_stream {
    const pb_import_stream_config *config;
    void *conn;
    void *js;
    void *sub;
    uv_loop_t *loop;
    uv_async_t async;
    uv_mutex_t lock;
    uv_cond_t cond;
    uv_thread_t thread;
    mb_js_import_handler handler;
    void *handler_ctx;
    natsMsg *pending_msg;
    // Global stream LastSeq sampled at startup; filtered consumers may catch up earlier with pending=0.
    uint64_t catchup_to;
    uint64_t last_stream_seq;
    uint64_t last_event_now_ms;
    int64_t last_event_wall_ms;
    uint64_t received;
    uint64_t processed;
    uint64_t acked;
    uint64_t redelivered;
    uint64_t failed;
    int last_status;
    bool lock_started;
    bool cond_started;
    bool async_started;
    bool thread_started;
    bool started;
    bool closing;
    bool pending_ready;
    bool pending_in_process;
    bool pending_done;
    bool pending_ok;
    bool async_closed;
};

static bool set_status(mb_js_stream *stream, natsStatus status, const char *what) {
    if (status == NATS_OK) {
        return true;
    }
    if (stream != NULL) {
        stream->last_status = (int)status;
    }
    fprintf(stderr, "warn: jetstream import: %s failed: %s\n", what, natsStatus_GetText(status));
    return false;
}

static bool js_stream_is_closing(mb_js_stream *stream) {
    if (stream == NULL) {
        return true;
    }
    if (!stream->lock_started) {
        return stream->closing;
    }
    uv_mutex_lock(&stream->lock);
    const bool closing = stream->closing;
    uv_mutex_unlock(&stream->lock);
    return closing;
}

static char *track_slice(mb_nats_strings *strings, pb_slice slice) {
    return mb_nats_track_cstr(strings, slice);
}

static bool stream_config_strings(mb_js_stream *stream, mb_nats_strings *strings,
                                  char **subject, char **stream_name, char **consumer_name) {
    const pb_import_stream_config *config = stream->config;
    if (config->source.subjects_len != 1) {
        fprintf(stderr, "warn: jetstream import: stream %.*s expects exactly one subject filter for v1\n",
                (int)config->stream.len, config->stream.ptr);
        return false;
    }
    *subject = track_slice(strings, config->source.subjects[0]);
    *stream_name = track_slice(strings, config->stream);
    *consumer_name = track_slice(strings, config->consumer);
    return *subject != NULL && *stream_name != NULL && *consumer_name != NULL;
}

static bool js_stream_connect(mb_js_stream *stream) {
    natsOptions *opts = NULL;
    mb_nats_strings strings = {0};
    natsConnection *conn = NULL;
    jsCtx *js = NULL;
    natsSubscription *sub = NULL;
    const mb_nats_options_config options = mb_nats_options_from_import(&stream->config->source);
    char *subject = NULL;
    char *stream_name = NULL;
    char *consumer_name = NULL;
    jsSubOptions sub_opts;
    jsErrCode err = 0;
    bool retained = false;
    bool ok = false;

    natsStatus status = natsOptions_Create(&opts);
    if (!set_status(stream, status, "create options")) {
        goto cleanup;
    }

    if (!mb_nats_set_options(opts, &options, &strings)) {
        goto cleanup;
    }

    mb_nats_retain();
    retained = true;
    status = natsConnection_Connect(&conn, opts);
    if (!set_status(stream, status, "connect")) {
        goto cleanup;
    }

    status = natsConnection_JetStream(&js, conn, NULL);
    if (!set_status(stream, status, "create JetStream context")) {
        goto cleanup;
    }

    memset(&sub_opts, 0, sizeof sub_opts);
    if (!stream_config_strings(stream, &strings, &subject, &stream_name, &consumer_name)) {
        goto cleanup;
    }

    status = jsSubOptions_Init(&sub_opts);
    if (!set_status(stream, status, "initialize subscribe options")) {
        goto cleanup;
    }

    sub_opts.Stream = stream_name;
    sub_opts.Config.Name = consumer_name;
    sub_opts.Config.Durable = consumer_name;
    sub_opts.Config.AckPolicy = js_AckExplicit;
    sub_opts.Config.DeliverPolicy = js_DeliverAll;
    sub_opts.Config.ReplayPolicy = js_ReplayInstant;
    sub_opts.Config.FilterSubject = subject;
    status = js_PullSubscribe(&sub, js, subject, consumer_name, NULL, &sub_opts, &err);
    if (!set_status(stream, status, "pull subscribe")) {
        if (err != 0) {
            fprintf(stderr, "warn: jetstream import: server error code=%d\n", (int)err);
        }
        goto cleanup;
    }

    ok = true;
    stream->conn = conn;
    stream->js = js;
    stream->sub = sub;
    stream->started = true;

cleanup:
    if (!ok) {
        if (sub != NULL) {
            natsSubscription_Destroy(sub);
        }
        if (js != NULL) {
            jsCtx_Destroy(js);
        }
        if (conn != NULL) {
            natsConnection_Destroy(conn);
        }
        if (retained) {
            mb_nats_release();
        }
    }

    if (opts != NULL) {
        natsOptions_Destroy(opts);
    }
    mb_nats_strings_free(&strings);
    return ok;
}

static bool js_stream_load_highwater(mb_js_stream *stream) {
    jsStreamInfo *info = NULL;
    jsErrCode err = 0;
    natsStatus status = NATS_OK;
    const pb_import_stream_config *config = stream->config;
    mb_nats_strings strings = {0};
    char *stream_name = track_slice(&strings, config->stream);
    bool ok = false;
    if (stream_name == NULL) {
        goto cleanup;
    }

    status = js_GetStreamInfo(&info, stream->js, stream_name, NULL, &err);
    if (!set_status(stream, status, "get stream info")) {
        goto cleanup;
    }

    stream->catchup_to = info->State.LastSeq;
    fprintf(stderr, "info: jetstream import: stream=%s catch-up high-water=%" PRIu64 "\n",
            stream_name, stream->catchup_to);
    ok = true;

cleanup:
    if (!ok && err != 0) {
        fprintf(stderr, "warn: jetstream import: stream info server error code=%d\n", (int)err);
    }
    jsStreamInfo_Destroy(info);
    mb_nats_strings_free(&strings);
    return ok;
}

static bool subject_allowed(mb_slice subject) {
    return mb_proto_subject_valid(subject, false) &&
           !mb_router_subject_has_lvc_prefix(subject) &&
           !mb_router_subject_has_stats_prefix(subject);
}

static bool event_time_from_msg(mb_js_stream *stream, natsMsg *msg, jsMsgMetaData **out_meta,
                                uint64_t *out_now_ms, int64_t *out_wall_ms) {
    jsMsgMetaData *meta = NULL;
    natsStatus status = natsMsg_GetMetaData(&meta, msg);
    if (!set_status(stream, status, "read message metadata")) {
        return false;
    }
    if (meta->Timestamp <= 0) {
        fprintf(stderr, "warn: jetstream import: missing message timestamp\n");
        jsMsgMetaData_Destroy(meta);
        return false;
    }
    int64_t wall_ms = meta->Timestamp / 1000000;
    if (stream->last_event_wall_ms != 0 && wall_ms < stream->last_event_wall_ms) {
        fprintf(stderr, "warn: jetstream import: clamping out-of-order event time stream=%" PRIu64 "\n",
                meta->Sequence.Stream);
        wall_ms = stream->last_event_wall_ms;
    }
    if (wall_ms < 0) {
        jsMsgMetaData_Destroy(meta);
        return false;
    }
    *out_meta = meta;
    *out_wall_ms = wall_ms;
    *out_now_ms = (uint64_t)wall_ms;
    return true;
}

static bool js_stream_process_msg(mb_js_stream *stream, natsMsg *msg, bool replaying) {
    stream->received += 1;
    jsMsgMetaData *meta = NULL;
    uint64_t event_now_ms = 0;
    int64_t event_wall_ms = 0;
    if (!event_time_from_msg(stream, msg, &meta, &event_now_ms, &event_wall_ms)) {
        stream->failed += 1;
        return false;
    }

    const char *subject_ptr = natsMsg_GetSubject(msg);
    const int payload_len = natsMsg_GetDataLength(msg);
    const char *payload_ptr = natsMsg_GetData(msg);
    if (subject_ptr == NULL || payload_len < 0 || (payload_ptr == NULL && payload_len != 0) ||
        (size_t)payload_len > MB_MAX_PAYLOAD) {
        fprintf(stderr, "warn: jetstream import: invalid message frame\n");
        jsMsgMetaData_Destroy(meta);
        stream->failed += 1;
        return false;
    }
    const mb_slice subject = {.ptr = (const uint8_t *)subject_ptr, .len = strlen(subject_ptr)};
    const mb_slice payload = {.ptr = (const uint8_t *)payload_ptr, .len = (size_t)payload_len};
    if (!subject_allowed(subject)) {
        fprintf(stderr, "warn: jetstream import: invalid subject %s\n", subject_ptr);
        jsMsgMetaData_Destroy(meta);
        stream->failed += 1;
        return false;
    }

    if (meta->NumDelivered > 1) {
        stream->redelivered += 1;
    }
    const bool ok = stream->handler(stream->handler_ctx, stream->config, subject, payload,
                                    event_now_ms, event_wall_ms, replaying);
    if (!ok) {
        stream->failed += 1;
        jsMsgMetaData_Destroy(meta);
        return false;
    }
    const natsStatus ack_status = natsMsg_Ack(msg, NULL);
    if (!set_status(stream, ack_status, "ack message")) {
        stream->failed += 1;
        jsMsgMetaData_Destroy(meta);
        return false;
    }
    stream->acked += 1;
    stream->processed += 1;
    stream->last_stream_seq = meta->Sequence.Stream;
    stream->last_event_now_ms = event_now_ms;
    stream->last_event_wall_ms = event_wall_ms;
    jsMsgMetaData_Destroy(meta);
    return true;
}

static bool js_stream_pending_zero(mb_js_stream *stream, bool *out) {
    jsConsumerInfo *info = NULL;
    jsErrCode err = 0;
    natsStatus status = NATS_OK;
    mb_nats_strings strings = {0};
    char *stream_name = track_slice(&strings, stream->config->stream);
    char *consumer_name = track_slice(&strings, stream->config->consumer);
    bool ok = false;
    if (stream_name == NULL || consumer_name == NULL) {
        goto cleanup;
    }

    status = js_GetConsumerInfo(&info, stream->js, stream_name, consumer_name, NULL, &err);
    if (!set_status(stream, status, "get consumer info")) {
        goto cleanup;
    }

    *out = info->NumPending == 0;
    if (*out && info->AckFloor.Stream > stream->last_stream_seq) {
        stream->last_stream_seq = info->AckFloor.Stream;
    }
    ok = true;

cleanup:
    if (!ok && err != 0) {
        fprintf(stderr, "warn: jetstream import: consumer info server error code=%d\n", (int)err);
    }
    if (info != NULL) {
        jsConsumerInfo_Destroy(info);
    }
    mb_nats_strings_free(&strings);
    return ok;
}

static bool js_stream_fetch_and_process(mb_js_stream *stream, int batch, int64_t timeout_ms,
                                        bool replaying, bool *out_timeout) {
    *out_timeout = false;
    natsMsgList list = {0};
    jsErrCode err = 0;
    const natsStatus status = natsSubscription_Fetch(&list, stream->sub, batch, timeout_ms, &err);
    if (status == NATS_TIMEOUT || status == NATS_NOT_FOUND) {
        *out_timeout = true;
        return true;
    }
    if (!set_status(stream, status, "fetch messages")) {
        if (err != 0) {
            fprintf(stderr, "warn: jetstream import: fetch server error code=%d\n", (int)err);
        }
        return false;
    }
    bool ok = true;
    for (int i = 0; i < list.Count; i += 1) {
        natsMsg *msg = list.Msgs[i];
        list.Msgs[i] = NULL;
        if (msg != NULL) {
            ok = js_stream_process_msg(stream, msg, replaying) && ok;
            natsMsg_Destroy(msg);
        }
    }
    natsMsgList_Destroy(&list);
    return ok;
}

static bool js_stream_catch_up(mb_js_stream *stream) {
    if (!stream->config->catch_up) {
        fprintf(stderr, "info: jetstream import: stream=%.*s catch-up disabled\n",
                (int)stream->config->stream.len, stream->config->stream.ptr);
        return true;
    }
    if (!js_stream_load_highwater(stream)) {
        return false;
    }
    while (!js_stream_is_closing(stream)) {
        if (stream->catchup_to == 0 || stream->last_stream_seq >= stream->catchup_to) {
            return true;
        }
        bool timed_out = false;
        if (!js_stream_fetch_and_process(stream, MB_JS_CATCHUP_BATCH, MB_JS_CATCHUP_FETCH_TIMEOUT_MS, true, &timed_out)) {
            return false;
        }
        bool pending_zero = false;
        if (stream->last_stream_seq >= stream->catchup_to) {
            return true;
        }
        if ((timed_out || stream->last_stream_seq < stream->catchup_to) &&
            js_stream_pending_zero(stream, &pending_zero) && pending_zero) {
            fprintf(stderr, "info: jetstream import: stream=%.*s filtered consumer caught up (ack-floor=%" PRIu64 ", stream high-water=%" PRIu64 ", pending=0)\n",
                    (int)stream->config->stream.len, stream->config->stream.ptr, stream->last_stream_seq, stream->catchup_to);
            return true;
        }
    }
    return false;
}

static void on_js_async(uv_async_t *async) {
    mb_js_stream *stream = async->data;
    uv_mutex_lock(&stream->lock);
    natsMsg *msg = stream->pending_ready ? stream->pending_msg : NULL;
    if (msg != NULL) {
        stream->pending_msg = NULL;
        stream->pending_ready = false;
        stream->pending_in_process = true;
    }
    uv_mutex_unlock(&stream->lock);
    if (msg == NULL) {
        return;
    }

    const bool ok = js_stream_process_msg(stream, msg, false);
    natsMsg_Destroy(msg);

    uv_mutex_lock(&stream->lock);
    stream->pending_in_process = false;
    stream->pending_ok = ok;
    stream->pending_done = true;
    uv_cond_broadcast(&stream->cond);
    uv_mutex_unlock(&stream->lock);
}

static bool js_stream_handoff_msg(mb_js_stream *stream, natsMsg *msg, bool *out_consumed) {
    *out_consumed = false;
    uv_mutex_lock(&stream->lock);
    while (!stream->closing && (stream->pending_ready || stream->pending_in_process)) {
        uv_cond_wait(&stream->cond, &stream->lock);
    }
    if (stream->closing) {
        uv_mutex_unlock(&stream->lock);
        return false;
    }
    stream->pending_msg = msg;
    stream->pending_ready = true;
    stream->pending_done = false;
    stream->pending_ok = false;
    stream->pending_in_process = false;
    const int async_rc = uv_async_send(&stream->async);
    if (async_rc != 0) {
        stream->pending_msg = NULL;
        stream->pending_ready = false;
        uv_mutex_unlock(&stream->lock);
        fprintf(stderr, "warn: jetstream import: async handoff failed: %s\n", uv_strerror(async_rc));
        return false;
    }
    for (;;) {
        if (stream->pending_done) {
            *out_consumed = true;
            const bool ok = stream->pending_ok;
            uv_mutex_unlock(&stream->lock);
            return ok;
        }
        if (stream->closing && stream->pending_ready && stream->pending_msg == msg && !stream->pending_in_process) {
            stream->pending_msg = NULL;
            stream->pending_ready = false;
            uv_mutex_unlock(&stream->lock);
            return false;
        }
        uv_cond_wait(&stream->cond, &stream->lock);
    }
}

static void js_stream_thread_main(void *arg) {
    mb_js_stream *stream = arg;
    while (!js_stream_is_closing(stream)) {
        natsMsgList list = {0};
        jsErrCode err = 0;
        const natsStatus status = natsSubscription_Fetch(&list, stream->sub, 1, MB_JS_LIVE_FETCH_TIMEOUT_MS, &err);
        if (status == NATS_TIMEOUT || status == NATS_NOT_FOUND) {
            continue;
        }
        if (!set_status(stream, status, "live fetch")) {
            if (err != 0) {
                fprintf(stderr, "warn: jetstream import: live fetch server error code=%d\n", (int)err);
            }
            uv_sleep(50);
            continue;
        }
        for (int i = 0; i < list.Count; i += 1) {
            natsMsg *msg = list.Msgs[i];
            list.Msgs[i] = NULL;
            if (msg == NULL) {
                continue;
            }
            bool consumed = false;
            if (!js_stream_handoff_msg(stream, msg, &consumed)) {
                if (!consumed) {
                    natsMsg_Destroy(msg);
                }
                break;
            }
        }
        natsMsgList_Destroy(&list);
    }
}

static bool js_stream_start_live(mb_js_stream *stream) {
    if (stream->thread_started) {
        return true;
    }
    if (uv_thread_create(&stream->thread, js_stream_thread_main, stream) != 0) {
        fprintf(stderr, "warn: jetstream import: start live worker failed\n");
        return false;
    }
    stream->thread_started = true;
    return true;
}

static bool js_stream_init(mb_js_stream *stream, uv_loop_t *loop, const pb_import_stream_config *config,
                           mb_js_import_handler handler, void *handler_ctx) {
    *stream = (mb_js_stream){.config = config, .loop = loop, .handler = handler, .handler_ctx = handler_ctx};
    if (uv_mutex_init(&stream->lock) != 0) {
        return false;
    }
    stream->lock_started = true;
    if (uv_cond_init(&stream->cond) != 0) {
        return false;
    }
    stream->cond_started = true;
    if (uv_async_init(loop, &stream->async, on_js_async) != 0) {
        return false;
    }
    stream->async.data = stream;
    stream->async_started = true;
    return js_stream_connect(stream);
}

static void on_js_async_close(uv_handle_t *handle) {
    mb_js_stream *stream = handle->data;
    if (stream != NULL) {
        stream->async_closed = true;
    }
}

static void js_stream_close(mb_js_stream *stream) {
    if (stream == NULL) {
        return;
    }
    if (stream->lock_started) {
        uv_mutex_lock(&stream->lock);
        stream->closing = true;
        if (stream->cond_started) {
            uv_cond_broadcast(&stream->cond);
        }
        uv_mutex_unlock(&stream->lock);
    } else {
        stream->closing = true;
    }
    if (stream->thread_started) {
        uv_thread_join(&stream->thread);
        stream->thread_started = false;
    }
    if (stream->pending_msg != NULL) {
        natsMsg_Destroy(stream->pending_msg);
        stream->pending_msg = NULL;
    }
    if (stream->sub != NULL) {
        natsSubscription_Destroy(stream->sub);
        stream->sub = NULL;
    }
    if (stream->js != NULL) {
        jsCtx_Destroy(stream->js);
        stream->js = NULL;
    }
    if (stream->conn != NULL) {
        natsConnection_Destroy(stream->conn);
        stream->conn = NULL;
    }
    if (stream->started) {
        mb_nats_release();
        stream->started = false;
    }
    if (stream->async_started && !uv_is_closing((uv_handle_t *)&stream->async)) {
        stream->async_closed = false;
        uv_close((uv_handle_t *)&stream->async, on_js_async_close);
        while (!stream->async_closed && stream->loop != NULL) {
            (void)uv_run(stream->loop, UV_RUN_NOWAIT);
        }
    }
    stream->async_started = false;
    if (stream->cond_started) {
        uv_cond_destroy(&stream->cond);
        stream->cond_started = false;
    }
    if (stream->lock_started) {
        uv_mutex_destroy(&stream->lock);
        stream->lock_started = false;
    }
}

bool mb_js_importer_start(mb_js_importer *importer, uv_loop_t *loop, const pb_imports_config *config,
                          mb_js_import_handler handler, void *handler_ctx) {
    *importer = (mb_js_importer){.config = config, .handler = handler, .handler_ctx = handler_ctx, .loop = loop};
    if (config == NULL || config->streams_len == 0) {
        return true;
    }
    if (loop == NULL || handler == NULL) {
        return false;
    }
    importer->streams = calloc(config->streams_len, sizeof importer->streams[0]);
    if (importer->streams == NULL) {
        return false;
    }
    importer->streams_len = config->streams_len;
    for (size_t i = 0; i < config->streams_len; i += 1) {
        if (!js_stream_init(&importer->streams[i], loop, &config->streams[i], handler, handler_ctx)) {
            mb_js_importer_close(importer);
            return false;
        }
    }
    for (size_t i = 0; i < importer->streams_len; i += 1) {
        if (!js_stream_catch_up(&importer->streams[i])) {
            mb_js_importer_close(importer);
            return false;
        }
    }
    mb_js_importer_refresh(importer);
    return true;
}

bool mb_js_importer_start_live(mb_js_importer *importer) {
    if (importer == NULL) {
        return false;
    }
    for (size_t i = 0; i < importer->streams_len; i += 1) {
        if (!js_stream_start_live(&importer->streams[i])) {
            return false;
        }
    }
    return true;
}

void mb_js_importer_refresh(mb_js_importer *importer) {
    if (importer == NULL) {
        return;
    }
    importer->received = 0;
    importer->processed = 0;
    importer->acked = 0;
    importer->redelivered = 0;
    importer->failed = 0;
    for (size_t i = 0; i < importer->streams_len; i += 1) {
        importer->received += importer->streams[i].received;
        importer->processed += importer->streams[i].processed;
        importer->acked += importer->streams[i].acked;
        importer->redelivered += importer->streams[i].redelivered;
        importer->failed += importer->streams[i].failed;
    }
}

void mb_js_importer_close(mb_js_importer *importer) {
    if (importer == NULL) {
        return;
    }
    for (size_t i = 0; i < importer->streams_len; i += 1) {
        js_stream_close(&importer->streams[i]);
    }
    free(importer->streams);
    *importer = (mb_js_importer){0};
}
