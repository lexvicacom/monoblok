#ifndef MB_ROUTER_H
#define MB_ROUTER_H

#include "buf.h"
#include "proto.h"

#define MB_LVC_PREFIX "$LVC."

enum {
    // Per-connection queued-write cap; slow consumers are closed before appending past it.
    MB_MAX_PENDING = 64 * 1024 * 1024,
    // Per-connection subscription cap to bound one client's router footprint.
    MB_MAX_SUBS_PER_CONN = 4096,
    // Global subscription cap to bound router fanout indexes.
    MB_MAX_SUBS_TOTAL = 65536,
    // LVC subject-count cap to bound last-value index growth.
    MB_MAX_LVC_ENTRIES = 65536,
    // LVC payload-byte cap across all cached last values.
    MB_MAX_LVC_BYTES = 64 * 1024 * 1024,
};

// Router-facing connection state; server owns transport and kicks writes.
typedef struct mb_router_conn {
    mb_buf out;
    uint64_t kick_seen_epoch;
    uint64_t close_seen_epoch;
    size_t sub_count;
    bool closed;
    void (*kick_fn)(void *ctx);
    void *kick_ctx;
    void (*close_fn)(void *ctx);
    void *close_ctx;
} mb_router_conn;

// One literal subject subscription and the SID needed to serialize MSG frames.
typedef struct mb_subscription {
    mb_router_conn *conn;
    uint8_t *subject;
    size_t subject_len;
    uint8_t *queue;
    size_t queue_len;
    uint8_t *sid;
    size_t sid_len;
    size_t delivered;
    size_t max_msgs;
    bool has_max_msgs;
    bool is_lvc;
} mb_subscription;

// Small growable bucket of subscriptions sharing a routing key.
typedef struct mb_sub_list {
    mb_subscription *items;
    size_t len;
    size_t cap;
} mb_sub_list;

// Literal-filter bucket keyed by the full subject filter.
typedef struct mb_literal_bucket {
    uint8_t *key;
    size_t key_len;
    mb_sub_list subs;
} mb_literal_bucket;

// Wildcard-filter bucket keyed by the first literal token.
typedef struct mb_wildcard_bucket {
    uint8_t *key;
    size_t key_len;
    mb_sub_list subs;
} mb_wildcard_bucket;

// Last-value cache entry. Payload buffer is reused when a subject is updated.
typedef struct mb_lvc_entry {
    uint8_t *subject;
    size_t subject_len;
    mb_buf payload;
} mb_lvc_entry;

// Configured LVC filter. Subjects outside these filters are not cached.
typedef struct mb_lvc_filter {
    uint8_t *filter;
    size_t filter_len;
} mb_lvc_filter;

// Hash index entry mapping a routed key to an owning array index.
typedef struct mb_router_index_entry {
    uint64_t hash;
    size_t index;
    bool occupied;
} mb_router_index_entry;

// Per-publish queue-group selection scratch; slices point into subscriptions.
typedef struct mb_queue_delivery {
    mb_slice subject;
    mb_slice queue;
    size_t selected;
    size_t seen;
    bool is_lvc;
} mb_queue_delivery;

// Routing index: literals, first-token wildcards, and global wildcards.
typedef struct mb_router {
    mb_literal_bucket *literal;
    size_t literal_len;
    size_t literal_cap;
    mb_router_index_entry *literal_index;
    size_t literal_index_cap;
    mb_wildcard_bucket *wildcard;
    size_t wildcard_len;
    size_t wildcard_cap;
    mb_router_index_entry *wildcard_index;
    size_t wildcard_index_cap;
    mb_sub_list wildcard_global;
    mb_router_conn **kick_scratch;
    size_t kick_cap;
    mb_router_conn **close_scratch;
    size_t close_cap;
    mb_queue_delivery *queue_scratch;
    size_t queue_cap;
    uint64_t kick_epoch;
    uint64_t queue_rng;
    size_t sub_count;
    mb_lvc_entry *lvc;
    size_t lvc_len;
    size_t lvc_cap;
    size_t lvc_payload_bytes;
    mb_router_index_entry *lvc_index;
    size_t lvc_index_cap;
    mb_lvc_filter *lvc_filters;
    size_t lvc_filter_len;
    size_t lvc_filter_cap;
    void *bridge_ctx;
    void (*bridge_fn)(void *ctx, mb_slice subject, mb_slice payload);
    bool lvc_enabled;
} mb_router;

void mb_router_init(mb_router *router);
bool mb_router_configure_lvc(mb_router *router, const mb_slice *filters, size_t filter_len);
void mb_router_disable_lvc(mb_router *router);
void mb_router_free(mb_router *router);
bool mb_router_subscribe(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice sid);
bool mb_router_subscribe_queue(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice queue, mb_slice sid);
void mb_router_unsubscribe(mb_router *router, mb_router_conn *conn, mb_slice sid, size_t max_msgs, bool has_max_msgs);
void mb_router_remove_all_for(mb_router *router, mb_router_conn *conn);
bool mb_router_publish(mb_router *router, mb_slice subject, mb_slice payload);
bool mb_router_subject_has_lvc_prefix(mb_slice subject);
bool mb_router_subject_matches(mb_slice filter, mb_slice subject);
bool mb_router_store_lvc(mb_router *router, mb_slice subject, mb_slice payload);
size_t mb_router_lvc_count(const mb_router *router);
bool mb_router_lvc_entry(const mb_router *router, size_t index, mb_slice *subject, mb_slice *payload);

#endif
