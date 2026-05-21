#define _POSIX_C_SOURCE 200809L

#include "bridge.h"

#include "array.h"
#include "nats_common.h"
#include "router.h"

#include "nats.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool set_status(natsStatus status, const char *what) {
    if (status == NATS_OK) {
        return true;
    }
    fprintf(stderr, "warn: bridge: %s failed: %s\n", what, natsStatus_GetText(status));
    return false;
}

bool mb_bridge_start(mb_bridge *bridge, const pb_bridge_config *config) {
    *bridge = (mb_bridge){.config = config};
    if (config == NULL || !config->present) {
        return true;
    }

    natsOptions *opts = NULL;
    natsStatus status = natsOptions_Create(&opts);
    if (!set_status(status, "create options")) {
        return false;
    }

    mb_nats_strings strings = {0};
    const mb_nats_options_config options = mb_nats_options_from_bridge(config);
    bool ok = mb_nats_set_options(opts, &options, &strings);
    char *origin_value = NULL;
    if (ok && config->origin_header) {
        origin_value = mb_nats_origin_header_value();
        if (origin_value == NULL) {
            fprintf(stderr, "warn: bridge: allocate origin header failed\n");
            ok = false;
        }
    }
    if (ok) {
        natsConnection *conn = NULL;
        mb_nats_retain();
        status = natsConnection_Connect(&conn, opts);
        ok = set_status(status, "connect");
        if (ok && (config->origin_header || config->replay_header)) {
            ok = set_status(natsConnection_HasHeaderSupport(conn), "check header support");
        }
        if (ok) {
            bridge->conn = conn;
            bridge->origin_header_value = origin_value;
            origin_value = NULL;
            bridge->started = true;
        } else if (conn != NULL) {
            natsConnection_Destroy(conn);
        }
        if (!ok) {
            mb_nats_release();
        }
    }

    natsOptions_Destroy(opts);
    mb_nats_strings_free(&strings);
    free(origin_value);
    return ok;
}

static bool bridge_matches_export(const mb_bridge *bridge, mb_slice subject) {
    const pb_bridge_config *config = bridge->config;
    for (size_t i = 0; i < config->exports_len; i += 1) {
        pb_slice filter = config->exports[i];
        if (mb_router_subject_matches((mb_slice){.ptr = (const uint8_t *)filter.ptr, .len = filter.len}, subject)) {
            return true;
        }
    }
    return false;
}

void mb_bridge_publish(void *ctx, mb_slice subject, mb_slice payload) {
    mb_bridge_publish_with_options(ctx, subject, payload, (mb_router_publish_options){0});
}

void mb_bridge_publish_with_options(void *ctx, mb_slice subject, mb_slice payload, mb_router_publish_options options) {
    mb_bridge *bridge = ctx;
    if (bridge == NULL || !bridge->started || bridge->conn == NULL || !bridge_matches_export(bridge, subject)) {
        return;
    }
    if (payload.len > (size_t)INT_MAX) {
        bridge->dropped += 1;
        return;
    }
    if (!mb_array_reserve((void **)&bridge->subject_scratch, &bridge->subject_scratch_cap,
                          subject.len + 1, sizeof bridge->subject_scratch[0], 64)) {
        bridge->dropped += 1;
        return;
    }
    memcpy(bridge->subject_scratch, subject.ptr, subject.len);
    bridge->subject_scratch[subject.len] = '\0';

    natsStatus status = NATS_OK;
    const bool add_origin_header = bridge->origin_header_value != NULL;
    const bool add_replay_header = bridge->config->replay_header && options.replaying;
    const bool add_assumed_ts_header = add_replay_header && options.has_assumed_ts_ms;
    char assumed_ts_value[32];
    if (add_assumed_ts_header) {
        snprintf(assumed_ts_value, sizeof assumed_ts_value, "%lld", (long long)options.assumed_ts_ms);
    }
    if (add_origin_header || add_replay_header || add_assumed_ts_header) {
        natsMsg *msg = NULL;
        status = natsMsg_Create(&msg, bridge->subject_scratch, NULL, (const char *)payload.ptr, (int)payload.len);
        if (status == NATS_OK && add_origin_header) {
            status = natsMsgHeader_Set(msg, MB_NATS_ORIGIN_HEADER, bridge->origin_header_value);
        }
        if (status == NATS_OK && add_replay_header) {
            status = natsMsgHeader_Set(msg, MB_NATS_REPLAY_HEADER, "true");
        }
        if (status == NATS_OK && add_assumed_ts_header) {
            status = natsMsgHeader_Set(msg, MB_NATS_ASSUMED_TS_HEADER, assumed_ts_value);
        }
        if (status == NATS_OK) {
            status = natsConnection_PublishMsg((natsConnection *)bridge->conn, msg);
        }
        if (msg != NULL) {
            natsMsg_Destroy(msg);
        }
    } else {
        status = natsConnection_Publish((natsConnection *)bridge->conn, bridge->subject_scratch, payload.ptr, (int)payload.len);
    }
    if (status == NATS_OK) {
        bridge->published += 1;
        return;
    }
    bridge->dropped += 1;
    bridge->last_status = (int)status;
    fprintf(stderr, "warn: bridge: publish failed: %s\n", natsStatus_GetText(status));
}

void mb_bridge_close(mb_bridge *bridge) {
    if (bridge == NULL) {
        return;
    }
    if (bridge->conn != NULL) {
        natsConnection_Destroy((natsConnection *)bridge->conn);
    }
    free(bridge->subject_scratch);
    free(bridge->origin_header_value);
    if (bridge->started) {
        mb_nats_release();
    }
    *bridge = (mb_bridge){0};
}
