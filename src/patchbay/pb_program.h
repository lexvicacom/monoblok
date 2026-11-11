#ifndef PB_PROGRAM_H
#define PB_PROGRAM_H

#include "pb_eval.h"
#include "router.h"

enum {
    PB_PROGRAM_MAX_REENTRY_DEPTH = 8, // re-entrant emissions stop feeding back at this depth
};

typedef struct pb_rule {
    pb_slice filter;
    pb_value body;
    uint64_t publishes_emitted;
    uint64_t publishes_suppressed;
    uint64_t eval_failures; // runtime eval failures, drives the log rate limit
    uint64_t every_ms;      // nonzero for (every :ms N ...) rules: empty filter, never matched to publishes
    bool reentrant;
    pb_eval_state state;
} pb_rule;

// Per-evaluation flags supplied by ingress paths.
// reply_to is borrowed for the call; empty means the message had no reply subject.
typedef struct pb_program_eval_options {
    bool replaying;
    mb_slice reply_to;
} pb_program_eval_options;

// Parsed top-level `(every :ms N [:reentrant BOOL] BODY...)`. body points into the
// parse arena, already resolved; several body forms are wrapped in an implicit `do`.
typedef struct pb_every_spec {
    uint64_t ms;
    bool reentrant;
    pb_value *body;
} pb_every_spec;

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

// NATS connection options shared by export and import entries; slices borrow the parse arena.
// An empty string slice means "not configured"; the four numeric options carry an explicit flag
// because 0 and -1 are valid user values (for example :max-reconnect -1).
typedef struct pb_remote_config {
    pb_slice *servers;
    size_t servers_len;
    size_t servers_cap;
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
    bool has_connect_timeout_ms;
    bool has_ping_interval_ms;
    bool has_reconnect_wait_ms;
    bool has_max_reconnect;
} pb_remote_config;

// Top-level `(export ...)` remote NATS config; deprecated `(bridge ...)` aliases it.
typedef struct pb_bridge_config {
    pb_remote_config remote;
    pb_slice *exports;
    size_t exports_len;
    size_t exports_cap;
    bool present;
    bool origin_header;
    bool replay_header;
} pb_bridge_config;

// One core-NATS import source; slices borrow from the parse arena.
// max_pending 0 means "use the importer default".
typedef struct pb_import_config {
    pb_remote_config remote;
    pb_slice *subjects;
    size_t subjects_len;
    size_t subjects_cap;
    size_t max_pending;
    bool present;
    bool origin_header;
} pb_import_config;

// One JetStream import source; base connection fields borrow from the parse arena.
// Empty stream/consumer slices mean "absent" (rejected at load).
typedef struct pb_import_stream_config {
    pb_import_config source;
    pb_slice stream;
    pb_slice consumer;
    bool catch_up;
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
// Validates an `every` form and resolves its body. On failure writes a short message into err.
bool pb_every_parse(pb_arena *arena, pb_values items, bool allow_user_call, bool allow_user_symbol,
                    pb_every_spec *out, char *err, size_t err_cap);
bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms, pb_program_eval_options options);
bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms,
                     pb_program_eval_options options);
bool pb_program_tick_until(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms,
                           pb_program_eval_options options);
bool pb_program_next_clock_deadline(const pb_program *program, uint64_t *out_ms);

#endif
