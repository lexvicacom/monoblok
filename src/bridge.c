#define _POSIX_C_SOURCE 200809L

#include "bridge.h"

#include "array.h"
#include "router.h"

#include "nats.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bridge_strings {
    char **items;
    size_t len;
    size_t cap;
} bridge_strings;

static void bridge_strings_free(bridge_strings *strings) {
    for (size_t i = 0; i < strings->len; i += 1) {
        free(strings->items[i]);
    }
    free(strings->items);
    *strings = (bridge_strings){0};
}

static char *slice_to_cstr(pb_slice slice) {
    char *s = malloc(slice.len + 1);
    if (s == NULL) {
        return NULL;
    }
    memcpy(s, slice.ptr, slice.len);
    s[slice.len] = '\0';
    return s;
}

static char *track_cstr(bridge_strings *strings, pb_slice slice) {
    if (!mb_array_reserve((void **)&strings->items, &strings->cap, strings->len + 1,
                          sizeof strings->items[0], 8)) {
        return NULL;
    }
    char *s = slice_to_cstr(slice);
    if (s == NULL) {
        return NULL;
    }
    strings->items[strings->len] = s;
    strings->len += 1;
    return s;
}

static bool set_status(natsStatus status, const char *what) {
    if (status == NATS_OK) {
        return true;
    }
    fprintf(stderr, "warn: bridge: %s failed: %s\n", what, natsStatus_GetText(status));
    return false;
}

static bool set_bridge_options(natsOptions *opts, const pb_bridge_config *config, bridge_strings *strings) {
    if (config->servers_len > (size_t)INT_MAX) {
        fprintf(stderr, "warn: bridge: too many configured servers\n");
        return false;
    }
    const char **servers = calloc(config->servers_len, sizeof servers[0]);
    if (servers == NULL) {
        return false;
    }
    for (size_t i = 0; i < config->servers_len; i += 1) {
        servers[i] = track_cstr(strings, config->servers[i]);
        if (servers[i] == NULL) {
            free(servers);
            return false;
        }
    }
    bool ok = set_status(natsOptions_SetServers(opts, servers, (int)config->servers_len), "set servers");
    free(servers);
    if (!ok) {
        return false;
    }

    if (config->has_name) {
        char *name = track_cstr(strings, config->name);
        if (name == NULL || !set_status(natsOptions_SetName(opts, name), "set name")) {
            return false;
        }
    }

    if (config->has_creds) {
        char *creds = track_cstr(strings, config->creds);
        if (creds == NULL ||
            !set_status(natsOptions_SetUserCredentialsFromFiles(opts, creds, NULL), "set credentials")) {
            return false;
        }
    } else if (config->has_user || config->has_password) {
        if (!config->has_user || !config->has_password) {
            fprintf(stderr, "warn: bridge: :user and :password must be configured together\n");
            return false;
        }
        char *user = track_cstr(strings, config->user);
        char *password = track_cstr(strings, config->password);
        if (user == NULL || password == NULL ||
            !set_status(natsOptions_SetUserInfo(opts, user, password), "set user info")) {
            return false;
        }
    } else if (config->has_token) {
        char *token = track_cstr(strings, config->token);
        if (token == NULL || !set_status(natsOptions_SetToken(opts, token), "set token")) {
            return false;
        }
    }

    if (config->tls || config->has_tls_ca || config->has_tls_cert || config->tls_skip_verify) {
        if (!set_status(natsOptions_SetSecure(opts, true), "enable TLS")) {
            return false;
        }
    }
    if (config->has_tls_ca) {
        char *ca = track_cstr(strings, config->tls_ca);
        if (ca == NULL || !set_status(natsOptions_LoadCATrustedCertificates(opts, ca), "load CA")) {
            return false;
        }
    }
    if (config->has_tls_cert || config->has_tls_key) {
        if (!config->has_tls_cert || !config->has_tls_key) {
            fprintf(stderr, "warn: bridge: :tls-cert and :tls-key must be configured together\n");
            return false;
        }
        char *cert = track_cstr(strings, config->tls_cert);
        char *key = track_cstr(strings, config->tls_key);
        if (cert == NULL || key == NULL ||
            !set_status(natsOptions_LoadCertificatesChain(opts, cert, key), "load client certificate")) {
            return false;
        }
    }
    if (config->tls_skip_verify &&
        !set_status(natsOptions_SkipServerVerification(opts, true), "set TLS skip verification")) {
        return false;
    }

    if (config->has_connect_timeout_ms &&
        !set_status(natsOptions_SetTimeout(opts, config->connect_timeout_ms), "set connect timeout")) {
        return false;
    }
    if (config->has_ping_interval_ms &&
        !set_status(natsOptions_SetPingInterval(opts, config->ping_interval_ms), "set ping interval")) {
        return false;
    }
    if (config->has_max_reconnect &&
        !set_status(natsOptions_SetMaxReconnect(opts, config->max_reconnect), "set max reconnect")) {
        return false;
    }
    if (config->has_reconnect_wait_ms &&
        !set_status(natsOptions_SetReconnectWait(opts, config->reconnect_wait_ms), "set reconnect wait")) {
        return false;
    }
    return true;
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

    bridge_strings strings = {0};
    bool ok = set_bridge_options(opts, config, &strings);
    if (ok) {
        natsConnection *conn = NULL;
        status = natsConnection_Connect(&conn, opts);
        ok = set_status(status, "connect");
        if (ok) {
            bridge->conn = conn;
            bridge->started = true;
        }
    }

    natsOptions_Destroy(opts);
    bridge_strings_free(&strings);
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

    natsStatus status =
        natsConnection_Publish((natsConnection *)bridge->conn, bridge->subject_scratch, payload.ptr, (int)payload.len);
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
    if (bridge->started) {
        nats_Close();
    }
    *bridge = (mb_bridge){0};
}
