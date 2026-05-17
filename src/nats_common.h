#ifndef MB_NATS_COMMON_H
#define MB_NATS_COMMON_H

#include "pb_program.h"

#include <stdbool.h>
#include <stddef.h>

#include "nats.h"

#define MB_NATS_ORIGIN_HEADER "x-monoblok"

// Temporary C strings passed into nats.c option setup.
typedef struct mb_nats_strings {
    char **items;
    size_t len;
    size_t cap;
} mb_nats_strings;

// View of connection options shared by bridge/export and import/tap mode.
typedef struct mb_nats_options_config {
    const pb_slice *servers;
    size_t servers_len;
    pb_slice name;
    pb_slice creds;
    pb_slice user;
    pb_slice password;
    pb_slice token;
    pb_slice tls_ca;
    pb_slice tls_cert;
    pb_slice tls_key;
    int64_t connect_timeout_ms;
    int64_t ping_interval_ms;
    int64_t reconnect_wait_ms;
    int max_reconnect;
    bool tls;
    bool tls_skip_verify;
    bool has_name;
    bool has_creds;
    bool has_user;
    bool has_password;
    bool has_token;
    bool has_tls_ca;
    bool has_tls_cert;
    bool has_tls_key;
    bool has_connect_timeout_ms;
    bool has_ping_interval_ms;
    bool has_reconnect_wait_ms;
    bool has_max_reconnect;
} mb_nats_options_config;

mb_nats_options_config mb_nats_options_from_bridge(const pb_bridge_config *config);
mb_nats_options_config mb_nats_options_from_import(const pb_import_config *config);
bool mb_nats_set_options(natsOptions *opts, const mb_nats_options_config *config, mb_nats_strings *strings);
char *mb_nats_track_cstr(mb_nats_strings *strings, pb_slice slice);
char *mb_nats_origin_header_value(void);
void mb_nats_strings_free(mb_nats_strings *strings);
void mb_nats_retain(void);
void mb_nats_release(void);

#endif
