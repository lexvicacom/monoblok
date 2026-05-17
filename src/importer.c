#define _POSIX_C_SOURCE 200809L

#include "importer.h"

#include "array.h"
#include "nats_common.h"

#include "nats.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Reusable queue slot; bytes are subject followed by payload, owned by the importer ring.
typedef struct mb_import_slot {
    uint8_t *bytes;
    size_t cap;
    size_t subject_len;
    size_t payload_len;
} mb_import_slot;

enum {
    MB_IMPORT_POLL_TIMEOUT_MS = 10,
};

static bool set_status(natsStatus status, const char *what) {
    if (status == NATS_OK) {
        return true;
    }
    fprintf(stderr, "warn: import: %s failed: %s\n", what, natsStatus_GetText(status));
    return false;
}

static bool slot_reserve(mb_import_slot *slot, size_t needed) {
    if (slot->cap >= needed) {
        return true;
    }
    void *bytes = realloc(slot->bytes, needed == 0 ? 1 : needed);
    if (bytes == NULL) {
        return false;
    }
    slot->bytes = bytes;
    slot->cap = needed;
    return true;
}

static bool import_is_closing(mb_importer *importer) {
    bool closing = true;
    uv_mutex_lock(&importer->lock);
    closing = importer->closing;
    uv_mutex_unlock(&importer->lock);
    return closing;
}

static void import_record_drop(mb_importer *importer) {
    uv_mutex_lock(&importer->lock);
    importer->pending_received += 1;
    importer->pending_dropped += 1;
    if (!importer->closing && importer->async_started) {
        (void)uv_async_send(&importer->async);
    }
    uv_mutex_unlock(&importer->lock);
}

static void import_queue_msg(mb_importer *importer, const char *subject, const char *payload, size_t payload_len) {
    if (subject == NULL || (payload == NULL && payload_len != 0) || payload_len > MB_MAX_PAYLOAD) {
        import_record_drop(importer);
        return;
    }
    const size_t subject_len = strlen(subject);
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject, .len = subject_len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject) ||
        payload_len > SIZE_MAX - subject_len) {
        import_record_drop(importer);
        return;
    }

    uv_mutex_lock(&importer->lock);
    importer->pending_received += 1;
    if (importer->closing || importer->len >= importer->ring_cap) {
        importer->pending_dropped += 1;
        if (!importer->closing && importer->async_started) {
            (void)uv_async_send(&importer->async);
        }
        uv_mutex_unlock(&importer->lock);
        return;
    }

    const size_t idx = (importer->head + importer->len) % importer->ring_cap;
    mb_import_slot *slot = &importer->ring[idx];
    const size_t total = subject_len + payload_len;
    if (!slot_reserve(slot, total)) {
        importer->pending_dropped += 1;
        if (!importer->closing && importer->async_started) {
            (void)uv_async_send(&importer->async);
        }
        uv_mutex_unlock(&importer->lock);
        return;
    }
    memcpy(slot->bytes, subject, subject_len);
    if (payload_len != 0) {
        memcpy(slot->bytes + subject_len, payload, payload_len);
    }
    slot->subject_len = subject_len;
    slot->payload_len = payload_len;
    importer->len += 1;
    if (importer->async_started) {
        (void)uv_async_send(&importer->async);
    }
    uv_mutex_unlock(&importer->lock);
}

static bool import_has_origin_header(mb_importer *importer, natsMsg *msg) {
    if (!importer->config->origin_header) {
        return false;
    }
    const char *value = NULL;
    const natsStatus status = natsMsgHeader_Get(msg, MB_NATS_ORIGIN_HEADER, &value);
    if (status == NATS_OK) {
        return value != NULL && value[0] != '\0';
    }
    if (status != NATS_NOT_FOUND) {
        importer->last_status = (int)status;
    }
    return false;
}

static void import_handle_nats_msg(mb_importer *importer, natsMsg *msg) {
    if (msg == NULL) {
        return;
    }
    if (import_has_origin_header(importer, msg)) {
        import_record_drop(importer);
        natsMsg_Destroy(msg);
        return;
    }
    const int len = natsMsg_GetDataLength(msg);
    if (len < 0) {
        import_record_drop(importer);
        natsMsg_Destroy(msg);
        return;
    }
    import_queue_msg(importer, natsMsg_GetSubject(msg), natsMsg_GetData(msg), (size_t)len);
    natsMsg_Destroy(msg);
}

static void import_thread_main(void *arg) {
    mb_importer *importer = arg;
    while (!import_is_closing(importer)) {
        bool saw_message = false;
        for (size_t i = 0; i < importer->subs_len && !import_is_closing(importer); i += 1) {
            natsMsg *msg = NULL;
            const natsStatus status =
                natsSubscription_NextMsg(&msg, (natsSubscription *)importer->subs[i], MB_IMPORT_POLL_TIMEOUT_MS);
            if (status == NATS_OK) {
                saw_message = true;
                import_handle_nats_msg(importer, msg);
            } else if (status != NATS_TIMEOUT && status != NATS_NOT_FOUND) {
                importer->last_status = (int)status;
                uv_sleep(10);
            }
        }
        if (!saw_message && importer->subs_len == 0) {
            uv_sleep(MB_IMPORT_POLL_TIMEOUT_MS);
        }
    }
}

static void on_import_async(uv_async_t *async) {
    mb_importer_drain(async->data);
}

static bool sub_vec_append(mb_importer *importer, natsSubscription *sub) {
    if (!mb_array_reserve((void **)&importer->subs, &importer->subs_cap, importer->subs_len + 1,
                          sizeof importer->subs[0], 4)) {
        return false;
    }
    importer->subs[importer->subs_len] = sub;
    importer->subs_len += 1;
    return true;
}

bool mb_importer_start(mb_importer *importer, uv_loop_t *loop, const pb_import_config *config,
                       mb_importer_handler handler, void *handler_ctx) {
    *importer = (mb_importer){.config = config, .handler = handler, .handler_ctx = handler_ctx};
    if (config == NULL || !config->present) {
        return true;
    }
    if (loop == NULL || handler == NULL) {
        return false;
    }

    const size_t max_pending = config->has_max_pending ? config->max_pending : MB_IMPORT_DEFAULT_MAX_PENDING;
    importer->ring = calloc(max_pending, sizeof importer->ring[0]);
    if (importer->ring == NULL) {
        return false;
    }
    importer->ring_cap = max_pending;
    if (uv_mutex_init(&importer->lock) != 0) {
        mb_importer_close(importer);
        return false;
    }
    importer->lock_started = true;
    if (uv_async_init(loop, &importer->async, on_import_async) != 0) {
        mb_importer_close(importer);
        return false;
    }
    importer->async.data = importer;
    importer->async_started = true;

    natsOptions *opts = NULL;
    natsStatus status = natsOptions_Create(&opts);
    if (!set_status(status, "create options")) {
        mb_importer_close(importer);
        return false;
    }

    mb_nats_strings strings = {0};
    const mb_nats_options_config options = mb_nats_options_from_import(config);
    bool ok = mb_nats_set_options(opts, &options, &strings);
    if (ok) {
        natsConnection *conn = NULL;
        mb_nats_retain();
        status = natsConnection_Connect(&conn, opts);
        ok = set_status(status, "connect");
        if (ok && config->origin_header) {
            ok = set_status(natsConnection_HasHeaderSupport(conn), "check header support");
        }
        if (ok) {
            importer->conn = conn;
            importer->started = true;
        } else {
            if (conn != NULL) {
                natsConnection_Destroy(conn);
            }
            mb_nats_release();
        }
    }

    for (size_t i = 0; ok && i < config->subjects_len; i += 1) {
        char *subject = mb_nats_track_cstr(&strings, config->subjects[i]);
        natsSubscription *sub = NULL;
        if (subject == NULL) {
            ok = false;
            break;
        }
        status = natsConnection_SubscribeSync(&sub, (natsConnection *)importer->conn, subject);
        if (!set_status(status, "subscribe")) {
            ok = false;
            break;
        }
        if (!sub_vec_append(importer, sub)) {
            natsSubscription_Destroy(sub);
            ok = false;
            break;
        }
    }

    if (ok && uv_thread_create(&importer->thread, import_thread_main, importer) != 0) {
        fprintf(stderr, "warn: import: start worker thread failed\n");
        ok = false;
    }
    if (ok) {
        importer->thread_started = true;
    }

    natsOptions_Destroy(opts);
    mb_nats_strings_free(&strings);
    if (!ok) {
        mb_importer_close(importer);
    }
    return ok;
}

static void import_flush_pending_counters(mb_importer *importer) {
    importer->received += importer->pending_received;
    importer->dropped += importer->pending_dropped;
    importer->pending_received = 0;
    importer->pending_dropped = 0;
}

void mb_importer_drain(mb_importer *importer) {
    if (importer == NULL || importer->handler == NULL || !importer->lock_started) {
        return;
    }

    for (;;) {
        uv_mutex_lock(&importer->lock);
        import_flush_pending_counters(importer);
        if (importer->len == 0) {
            uv_mutex_unlock(&importer->lock);
            return;
        }
        mb_import_slot *slot = &importer->ring[importer->head];
        const size_t total = slot->subject_len + slot->payload_len;
        uv_mutex_unlock(&importer->lock);

        mb_buf_clear(&importer->drain_scratch);
        if (!mb_buf_reserve(&importer->drain_scratch, total)) {
            uv_mutex_lock(&importer->lock);
            import_flush_pending_counters(importer);
            if (importer->len != 0) {
                importer->head = (importer->head + 1) % importer->ring_cap;
                importer->len -= 1;
                importer->failed += 1;
            }
            uv_mutex_unlock(&importer->lock);
            continue;
        }

        uv_mutex_lock(&importer->lock);
        import_flush_pending_counters(importer);
        if (importer->len == 0) {
            uv_mutex_unlock(&importer->lock);
            continue;
        }
        slot = &importer->ring[importer->head];
        const size_t subject_len = slot->subject_len;
        const size_t payload_len = slot->payload_len;
        const size_t copy_total = subject_len + payload_len;
        memcpy(importer->drain_scratch.ptr, slot->bytes, copy_total);
        importer->drain_scratch.len = copy_total;
        importer->head = (importer->head + 1) % importer->ring_cap;
        importer->len -= 1;
        uv_mutex_unlock(&importer->lock);

        const mb_slice subject = {.ptr = importer->drain_scratch.ptr, .len = subject_len};
        const mb_slice payload = {.ptr = importer->drain_scratch.ptr + subject_len, .len = payload_len};
        if (importer->handler(importer->handler_ctx, subject, payload)) {
            importer->processed += 1;
        } else {
            importer->failed += 1;
        }
    }
}

void mb_importer_close(mb_importer *importer) {
    if (importer == NULL) {
        return;
    }
    if (importer->lock_started) {
        uv_mutex_lock(&importer->lock);
        importer->closing = true;
        import_flush_pending_counters(importer);
        uv_mutex_unlock(&importer->lock);
    } else {
        importer->closing = true;
    }

    if (importer->thread_started) {
        uv_thread_join(&importer->thread);
        importer->thread_started = false;
    }
    for (size_t i = 0; i < importer->subs_len; i += 1) {
        natsSubscription_Destroy((natsSubscription *)importer->subs[i]);
    }
    free(importer->subs);
    importer->subs = NULL;
    importer->subs_len = 0;
    importer->subs_cap = 0;
    if (importer->conn != NULL) {
        natsConnection_Destroy((natsConnection *)importer->conn);
        importer->conn = NULL;
    }
    if (importer->started) {
        mb_nats_release();
        importer->started = false;
    }
    if (importer->async_started && !uv_is_closing((uv_handle_t *)&importer->async)) {
        uv_close((uv_handle_t *)&importer->async, NULL);
    }
    importer->async_started = false;
    for (size_t i = 0; i < importer->ring_cap; i += 1) {
        free(importer->ring[i].bytes);
    }
    free(importer->ring);
    importer->ring = NULL;
    importer->ring_cap = 0;
    importer->head = 0;
    importer->len = 0;
    mb_buf_free(&importer->drain_scratch);
    if (importer->lock_started) {
        uv_mutex_destroy(&importer->lock);
        importer->lock_started = false;
    }
}
