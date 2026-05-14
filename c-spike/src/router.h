#ifndef MB_ROUTER_H
#define MB_ROUTER_H

#include "buf.h"
#include "proto.h"

// Router-facing connection state; server owns transport and kicks writes.
typedef struct mb_router_conn {
    mb_buf out;
    bool closed;
    void (*kick_fn)(void *ctx);
    void *kick_ctx;
} mb_router_conn;

// One literal subject subscription and the SID needed to serialize MSG frames.
typedef struct mb_subscription {
    mb_router_conn *conn;
    uint8_t *subject;
    size_t subject_len;
    uint8_t *sid;
    size_t sid_len;
    size_t delivered;
    size_t max_msgs;
    bool has_max_msgs;
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

// Zig-shaped routing index: literals, first-token wildcards, and global wildcards.
typedef struct mb_router {
    mb_literal_bucket *literal;
    size_t literal_len;
    size_t literal_cap;
    mb_wildcard_bucket *wildcard;
    size_t wildcard_len;
    size_t wildcard_cap;
    mb_sub_list wildcard_global;
    mb_router_conn **kick_scratch;
    size_t kick_cap;
    size_t sub_count;
} mb_router;

void mb_router_init(mb_router *router);
void mb_router_free(mb_router *router);
bool mb_router_subscribe(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice sid);
void mb_router_unsubscribe(mb_router *router, mb_router_conn *conn, mb_slice sid, size_t max_msgs, bool has_max_msgs);
void mb_router_remove_all_for(mb_router *router, mb_router_conn *conn);
bool mb_router_publish(mb_router *router, mb_slice subject, mb_slice payload);

#endif
