#define _POSIX_C_SOURCE 200809L

#include "nats_common.h"

#include "array.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t g_nats_users;

void mb_nats_strings_free(mb_nats_strings *strings) {
    for (size_t i = 0; i < strings->len; i += 1) {
        free(strings->items[i]);
    }
    free(strings->items);
    *strings = (mb_nats_strings){0};
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

static char *cstr_dup(const char *s) {
    const size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

char *mb_nats_origin_header_value(void) {
    char host[256];
    if (gethostname(host, sizeof host) != 0 || host[0] == '\0') {
        return cstr_dup("unknown");
    }
    host[sizeof host - 1] = '\0';
    return cstr_dup(host);
}

char *mb_nats_track_cstr(mb_nats_strings *strings, pb_slice slice) {
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
    fprintf(stderr, "warn: nats: %s failed: %s\n", what, natsStatus_GetText(status));
    return false;
}

mb_nats_options_config mb_nats_options_from_bridge(const pb_bridge_config *config) {
    return (mb_nats_options_config){
        .servers = config->servers,
        .servers_len = config->servers_len,
        .name = config->name,
        .creds = config->creds,
        .user = config->user,
        .password = config->password,
        .token = config->token,
        .tls_ca = config->tls_ca,
        .tls_cert = config->tls_cert,
        .tls_key = config->tls_key,
        .connect_timeout_ms = config->connect_timeout_ms,
        .ping_interval_ms = config->ping_interval_ms,
        .reconnect_wait_ms = config->reconnect_wait_ms,
        .max_reconnect = config->max_reconnect,
        .tls = config->tls,
        .tls_skip_verify = config->tls_skip_verify,
        .has_name = config->has_name,
        .has_creds = config->has_creds,
        .has_user = config->has_user,
        .has_password = config->has_password,
        .has_token = config->has_token,
        .has_tls_ca = config->has_tls_ca,
        .has_tls_cert = config->has_tls_cert,
        .has_tls_key = config->has_tls_key,
        .has_connect_timeout_ms = config->has_connect_timeout_ms,
        .has_ping_interval_ms = config->has_ping_interval_ms,
        .has_reconnect_wait_ms = config->has_reconnect_wait_ms,
        .has_max_reconnect = config->has_max_reconnect,
    };
}

mb_nats_options_config mb_nats_options_from_import(const pb_import_config *config) {
    return (mb_nats_options_config){
        .servers = config->servers,
        .servers_len = config->servers_len,
        .name = config->name,
        .creds = config->creds,
        .user = config->user,
        .password = config->password,
        .token = config->token,
        .tls_ca = config->tls_ca,
        .tls_cert = config->tls_cert,
        .tls_key = config->tls_key,
        .connect_timeout_ms = config->connect_timeout_ms,
        .ping_interval_ms = config->ping_interval_ms,
        .reconnect_wait_ms = config->reconnect_wait_ms,
        .max_reconnect = config->max_reconnect,
        .tls = config->tls,
        .tls_skip_verify = config->tls_skip_verify,
        .has_name = config->has_name,
        .has_creds = config->has_creds,
        .has_user = config->has_user,
        .has_password = config->has_password,
        .has_token = config->has_token,
        .has_tls_ca = config->has_tls_ca,
        .has_tls_cert = config->has_tls_cert,
        .has_tls_key = config->has_tls_key,
        .has_connect_timeout_ms = config->has_connect_timeout_ms,
        .has_ping_interval_ms = config->has_ping_interval_ms,
        .has_reconnect_wait_ms = config->has_reconnect_wait_ms,
        .has_max_reconnect = config->has_max_reconnect,
    };
}

bool mb_nats_set_options(natsOptions *opts, const mb_nats_options_config *config, mb_nats_strings *strings) {
    if (config->servers_len > (size_t)INT_MAX) {
        fprintf(stderr, "warn: nats: too many configured servers\n");
        return false;
    }
    const char **servers = calloc(config->servers_len, sizeof servers[0]);
    if (servers == NULL) {
        return false;
    }
    for (size_t i = 0; i < config->servers_len; i += 1) {
        servers[i] = mb_nats_track_cstr(strings, config->servers[i]);
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
        char *name = mb_nats_track_cstr(strings, config->name);
        if (name == NULL || !set_status(natsOptions_SetName(opts, name), "set name")) {
            return false;
        }
    }

    if (config->has_creds) {
        char *creds = mb_nats_track_cstr(strings, config->creds);
        if (creds == NULL ||
            !set_status(natsOptions_SetUserCredentialsFromFiles(opts, creds, NULL), "set credentials")) {
            return false;
        }
    } else if (config->has_user || config->has_password) {
        if (!config->has_user || !config->has_password) {
            fprintf(stderr, "warn: nats: :user and :password must be configured together\n");
            return false;
        }
        char *user = mb_nats_track_cstr(strings, config->user);
        char *password = mb_nats_track_cstr(strings, config->password);
        if (user == NULL || password == NULL ||
            !set_status(natsOptions_SetUserInfo(opts, user, password), "set user info")) {
            return false;
        }
    } else if (config->has_token) {
        char *token = mb_nats_track_cstr(strings, config->token);
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
        char *ca = mb_nats_track_cstr(strings, config->tls_ca);
        if (ca == NULL || !set_status(natsOptions_LoadCATrustedCertificates(opts, ca), "load CA")) {
            return false;
        }
    }
    if (config->has_tls_cert || config->has_tls_key) {
        if (!config->has_tls_cert || !config->has_tls_key) {
            fprintf(stderr, "warn: nats: :tls-cert and :tls-key must be configured together\n");
            return false;
        }
        char *cert = mb_nats_track_cstr(strings, config->tls_cert);
        char *key = mb_nats_track_cstr(strings, config->tls_key);
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

void mb_nats_retain(void) {
    g_nats_users += 1;
}

void mb_nats_release(void) {
    if (g_nats_users == 0) {
        return;
    }
    g_nats_users -= 1;
    if (g_nats_users == 0) {
        nats_Close();
    }
}
