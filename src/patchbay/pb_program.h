#ifndef PB_PROGRAM_H
#define PB_PROGRAM_H

#include "pb_eval.h"
#include "router.h"

typedef struct pb_rule {
    pb_slice filter;
    pb_value body;
    uint64_t publishes_emitted;
    uint64_t publishes_suppressed;
    bool reentrant;
    pb_eval_state state;
} pb_rule;

// Per-evaluation flags supplied by ingress paths.
typedef struct pb_program_eval_options {
    bool replaying;
} pb_program_eval_options;

// Top-level `(lvc ...)` filters borrowed from the parse arena.
typedef struct pb_lvc_config {
    pb_slice *filters;
    size_t len;
    size_t cap;
} pb_lvc_config;

// Ordered rule references used by the first-token dispatch index.
typedef struct pb_rule_ref_list {
    size_t *items;
    size_t len;
    size_t cap;
} pb_rule_ref_list;

// Bucket of rules sharing the same first literal subject token.
typedef struct pb_rule_bucket {
    pb_slice key;
    pb_rule_ref_list rules;
} pb_rule_bucket;

// Top-level `(export ...)` remote NATS config; deprecated `(bridge ...)` aliases it.
typedef struct pb_bridge_config {
    pb_slice *servers;
    size_t servers_len;
    size_t servers_cap;
    pb_slice *exports;
    size_t exports_len;
    size_t exports_cap;
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
    bool present;
    bool tls;
    bool tls_skip_verify;
    bool origin_header;
    bool replay_header;
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
} pb_bridge_config;

// One core-NATS import source; slices borrow from the parse arena.
typedef struct pb_import_config {
    pb_slice *servers;
    size_t servers_len;
    size_t servers_cap;
    pb_slice *subjects;
    size_t subjects_len;
    size_t subjects_cap;
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
    size_t max_pending;
    bool present;
    bool tls;
    bool tls_skip_verify;
    bool origin_header;
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
    bool has_max_pending;
} pb_import_config;

// One JetStream import source; base connection fields borrow from the parse arena.
typedef struct pb_import_stream_config {
    pb_import_config source;
    pb_slice stream;
    pb_slice consumer;
    bool catch_up;
    bool has_stream;
    bool has_consumer;
    bool has_catch_up;
} pb_import_stream_config;

// Top-level `(import ...)` config split by ingress kind.
typedef struct pb_imports_config {
    pb_import_config *cores;
    size_t cores_len;
    size_t cores_cap;
    pb_import_stream_config *streams;
    size_t streams_len;
    size_t streams_cap;
    bool present;
} pb_imports_config;

// The loaded and validated patchbay program: rules/config plus runtime state and eval scratch.
typedef struct pb_program {
    pb_arena parse_arena;
    pb_arena scratch;
    pb_rule *rules;
    size_t len;
    size_t cap;
    pb_rule_bucket *rule_buckets;
    size_t rule_bucket_len;
    size_t rule_bucket_cap;
    pb_rule_ref_list rule_global;
    pb_lvc_config lvc;
    pb_bridge_config bridge;
    pb_imports_config importer;
    bool uses_wall_clock;
    bool uses_clock_timer;
    size_t eval_depth;
    pb_eval_symbol_fn user_symbol;
    pb_eval_call_fn user_call;
    void *user_ctx;
} pb_program;

bool pb_program_load_file(pb_program *program, const char *path);
bool pb_program_load_source(pb_program *program, const char *label, const char *source, size_t source_len);
void pb_program_set_eval_hooks(pb_program *program, pb_eval_symbol_fn user_symbol, pb_eval_call_fn user_call, void *user_ctx);
void pb_program_free(pb_program *program);
bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms);
bool pb_program_eval_publish_with_options(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                                          uint64_t now_ms, int64_t wall_ms, pb_program_eval_options options);
bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms);
bool pb_program_tick_with_options(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms,
                                  pb_program_eval_options options);
bool pb_program_tick_until(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms,
                           pb_program_eval_options options);
bool pb_program_next_clock_deadline(const pb_program *program, uint64_t *out_ms);

#endif
