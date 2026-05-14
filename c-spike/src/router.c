#include "router.h"

#include <stdlib.h>
#include <string.h>

static void free_sub(mb_subscription *sub) {
    free(sub->subject);
    free(sub->sid);
    *sub = (mb_subscription){0};
}

static bool dup_slice(mb_slice s, uint8_t **out_ptr, size_t *out_len) {
    uint8_t *ptr = malloc(s.len == 0 ? 1 : s.len);
    if (ptr == NULL) {
        return false;
    }
    memcpy(ptr, s.ptr, s.len);
    *out_ptr = ptr;
    *out_len = s.len;
    return true;
}

static bool slice_eq_bytes(const uint8_t *ptr, size_t len, mb_slice s) {
    return len == s.len && memcmp(ptr, s.ptr, len) == 0;
}

static bool sid_eq(const mb_subscription *sub, mb_slice sid) {
    return slice_eq_bytes(sub->sid, sub->sid_len, sid);
}

static bool sub_list_append(mb_sub_list *list, mb_subscription sub) {
    if (list->len == list->cap) {
        const size_t next = list->cap == 0 ? 8 : list->cap * 2;
        mb_subscription *items = realloc(list->items, next * sizeof items[0]);
        if (items == NULL) {
            return false;
        }
        list->items = items;
        list->cap = next;
    }
    list->items[list->len] = sub;
    list->len += 1;
    return true;
}

static void sub_list_free(mb_sub_list *list) {
    for (size_t i = 0; i < list->len; i += 1) {
        free_sub(&list->items[i]);
    }
    free(list->items);
    *list = (mb_sub_list){0};
}

static void sub_list_remove_at(mb_sub_list *list, size_t i) {
    free_sub(&list->items[i]);
    list->len -= 1;
    if (i != list->len) {
        list->items[i] = list->items[list->len];
        list->items[list->len] = (mb_subscription){0};
    }
}

static bool next_subject_token(mb_slice s, size_t *pos, mb_slice *tok) {
    if (*pos > s.len) {
        return false;
    }
    const size_t start = *pos;
    while (*pos < s.len && s.ptr[*pos] != '.') {
        *pos += 1;
    }
    if (*pos == start) {
        return false;
    }
    *tok = (mb_slice){.ptr = s.ptr + start, .len = *pos - start};
    *pos = *pos < s.len ? *pos + 1 : s.len + 1;
    return true;
}

static mb_slice first_token(mb_slice s) {
    size_t pos = 0;
    mb_slice tok = {0};
    (void)next_subject_token(s, &pos, &tok);
    return tok;
}

static bool is_wildcard_token(mb_slice tok) {
    return tok.len == 1 && (tok.ptr[0] == '*' || tok.ptr[0] == '>');
}

static bool is_literal_filter(mb_slice filter) {
    for (size_t i = 0; i < filter.len; i += 1) {
        if (filter.ptr[i] == '*' || filter.ptr[i] == '>') {
            return false;
        }
    }
    return true;
}

static bool token_eq(mb_slice a, mb_slice b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static bool subject_matches(mb_slice filter, mb_slice subject) {
    size_t fp = 0;
    size_t sp = 0;
    for (;;) {
        mb_slice ftok = {0};
        if (!next_subject_token(filter, &fp, &ftok)) {
            return sp > subject.len;
        }
        if (ftok.len == 1 && ftok.ptr[0] == '>') {
            return fp > filter.len;
        }

        mb_slice stok = {0};
        if (!next_subject_token(subject, &sp, &stok)) {
            return false;
        }
        if (!(ftok.len == 1 && ftok.ptr[0] == '*') && !token_eq(ftok, stok)) {
            return false;
        }
    }
}

static bool grow_literal_buckets(mb_router *router) {
    if (router->literal_len != router->literal_cap) {
        return true;
    }
    const size_t next = router->literal_cap == 0 ? 8 : router->literal_cap * 2;
    mb_literal_bucket *items = realloc(router->literal, next * sizeof items[0]);
    if (items == NULL) {
        return false;
    }
    router->literal = items;
    router->literal_cap = next;
    return true;
}

static bool grow_wildcard_buckets(mb_router *router) {
    if (router->wildcard_len != router->wildcard_cap) {
        return true;
    }
    const size_t next = router->wildcard_cap == 0 ? 8 : router->wildcard_cap * 2;
    mb_wildcard_bucket *items = realloc(router->wildcard, next * sizeof items[0]);
    if (items == NULL) {
        return false;
    }
    router->wildcard = items;
    router->wildcard_cap = next;
    return true;
}

static bool ensure_kick_capacity(mb_router *router, size_t needed) {
    if (router->kick_cap >= needed) {
        return true;
    }
    const size_t next = needed == 0 ? 1 : needed;
    mb_router_conn **items = realloc(router->kick_scratch, next * sizeof items[0]);
    if (items == NULL) {
        return false;
    }
    router->kick_scratch = items;
    router->kick_cap = next;
    return true;
}

static mb_literal_bucket *literal_bucket(mb_router *router, mb_slice key, bool create) {
    for (size_t i = 0; i < router->literal_len; i += 1) {
        if (slice_eq_bytes(router->literal[i].key, router->literal[i].key_len, key)) {
            return &router->literal[i];
        }
    }
    if (!create || !grow_literal_buckets(router)) {
        return NULL;
    }
    mb_literal_bucket *bucket = &router->literal[router->literal_len];
    *bucket = (mb_literal_bucket){0};
    if (!dup_slice(key, &bucket->key, &bucket->key_len)) {
        return NULL;
    }
    router->literal_len += 1;
    return bucket;
}

static mb_wildcard_bucket *wildcard_bucket(mb_router *router, mb_slice key, bool create) {
    for (size_t i = 0; i < router->wildcard_len; i += 1) {
        if (slice_eq_bytes(router->wildcard[i].key, router->wildcard[i].key_len, key)) {
            return &router->wildcard[i];
        }
    }
    if (!create || !grow_wildcard_buckets(router)) {
        return NULL;
    }
    mb_wildcard_bucket *bucket = &router->wildcard[router->wildcard_len];
    *bucket = (mb_wildcard_bucket){0};
    if (!dup_slice(key, &bucket->key, &bucket->key_len)) {
        return NULL;
    }
    router->wildcard_len += 1;
    return bucket;
}

static void remove_literal_bucket_at(mb_router *router, size_t i) {
    free(router->literal[i].key);
    free(router->literal[i].subs.items);
    router->literal_len -= 1;
    if (i != router->literal_len) {
        router->literal[i] = router->literal[router->literal_len];
        router->literal[router->literal_len] = (mb_literal_bucket){0};
    }
}

static void remove_wildcard_bucket_at(mb_router *router, size_t i) {
    free(router->wildcard[i].key);
    free(router->wildcard[i].subs.items);
    router->wildcard_len -= 1;
    if (i != router->wildcard_len) {
        router->wildcard[i] = router->wildcard[router->wildcard_len];
        router->wildcard[router->wildcard_len] = (mb_wildcard_bucket){0};
    }
}

static bool already_kicked(mb_router_conn **kicked, size_t len, mb_router_conn *conn) {
    for (size_t i = 0; i < len; i += 1) {
        if (kicked[i] == conn) {
            return true;
        }
    }
    return false;
}

void mb_router_init(mb_router *router) {
    *router = (mb_router){0};
}

void mb_router_free(mb_router *router) {
    for (size_t i = 0; i < router->literal_len; i += 1) {
        free(router->literal[i].key);
        sub_list_free(&router->literal[i].subs);
    }
    for (size_t i = 0; i < router->wildcard_len; i += 1) {
        free(router->wildcard[i].key);
        sub_list_free(&router->wildcard[i].subs);
    }
    free(router->literal);
    free(router->wildcard);
    free(router->kick_scratch);
    sub_list_free(&router->wildcard_global);
    *router = (mb_router){0};
}

bool mb_router_subscribe(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice sid) {
    if (!ensure_kick_capacity(router, router->sub_count + 1)) {
        return false;
    }

    mb_subscription sub = {.conn = conn};
    if (!dup_slice(subject, &sub.subject, &sub.subject_len)) {
        return false;
    }
    if (!dup_slice(sid, &sub.sid, &sub.sid_len)) {
        free_sub(&sub);
        return false;
    }

    bool ok = false;
    if (is_literal_filter(subject)) {
        mb_literal_bucket *bucket = literal_bucket(router, subject, true);
        ok = bucket != NULL && sub_list_append(&bucket->subs, sub);
    } else {
        const mb_slice first = first_token(subject);
        if (is_wildcard_token(first)) {
            ok = sub_list_append(&router->wildcard_global, sub);
        } else {
            mb_wildcard_bucket *bucket = wildcard_bucket(router, first, true);
            ok = bucket != NULL && sub_list_append(&bucket->subs, sub);
        }
    }
    if (!ok) {
        free_sub(&sub);
        return false;
    }
    router->sub_count += 1;
    return true;
}

static void remove_for_conn_from_list(mb_router *router, mb_sub_list *list, mb_router_conn *conn) {
    size_t i = 0;
    while (i < list->len) {
        if (list->items[i].conn != conn) {
            i += 1;
            continue;
        }
        sub_list_remove_at(list, i);
        router->sub_count -= 1;
    }
}

void mb_router_remove_all_for(mb_router *router, mb_router_conn *conn) {
    for (size_t i = 0; i < router->literal_len;) {
        remove_for_conn_from_list(router, &router->literal[i].subs, conn);
        if (router->literal[i].subs.len == 0) {
            remove_literal_bucket_at(router, i);
            continue;
        }
        i += 1;
    }
    for (size_t i = 0; i < router->wildcard_len;) {
        remove_for_conn_from_list(router, &router->wildcard[i].subs, conn);
        if (router->wildcard[i].subs.len == 0) {
            remove_wildcard_bucket_at(router, i);
            continue;
        }
        i += 1;
    }
    remove_for_conn_from_list(router, &router->wildcard_global, conn);
}

static void unsubscribe_from_list(mb_router *router, mb_sub_list *list, mb_router_conn *conn,
                                  mb_slice sid, size_t max_msgs, bool has_max_msgs) {
    size_t i = 0;
    while (i < list->len) {
        mb_subscription *sub = &list->items[i];
        if (sub->conn != conn || !sid_eq(sub, sid)) {
            i += 1;
            continue;
        }
        if (!has_max_msgs || max_msgs <= sub->delivered) {
            sub_list_remove_at(list, i);
            router->sub_count -= 1;
            continue;
        }
        sub->max_msgs = max_msgs;
        sub->has_max_msgs = true;
        i += 1;
    }
}

void mb_router_unsubscribe(mb_router *router, mb_router_conn *conn, mb_slice sid, size_t max_msgs, bool has_max_msgs) {
    for (size_t i = 0; i < router->literal_len;) {
        unsubscribe_from_list(router, &router->literal[i].subs, conn, sid, max_msgs, has_max_msgs);
        if (router->literal[i].subs.len == 0) {
            remove_literal_bucket_at(router, i);
            continue;
        }
        i += 1;
    }
    for (size_t i = 0; i < router->wildcard_len;) {
        unsubscribe_from_list(router, &router->wildcard[i].subs, conn, sid, max_msgs, has_max_msgs);
        if (router->wildcard[i].subs.len == 0) {
            remove_wildcard_bucket_at(router, i);
            continue;
        }
        i += 1;
    }
    unsubscribe_from_list(router, &router->wildcard_global, conn, sid, max_msgs, has_max_msgs);
}

static bool deliver_matching_list(mb_router *router, mb_sub_list *list, mb_slice subject, mb_slice payload,
                                  bool check_filter, mb_router_conn **kicked, size_t *kicked_len) {
    size_t i = 0;
    while (i < list->len) {
        mb_subscription *sub = &list->items[i];
        if (sub->conn->closed) {
            i += 1;
            continue;
        }
        if (check_filter && !subject_matches((mb_slice){.ptr = sub->subject, .len = sub->subject_len}, subject)) {
            i += 1;
            continue;
        }
        const mb_slice sid = {.ptr = sub->sid, .len = sub->sid_len};
        if (!mb_write_msg(&sub->conn->out, subject, sid, payload)) {
            return false;
        }
        sub->delivered += 1;
        if (sub->conn->kick_fn != NULL && !already_kicked(kicked, *kicked_len, sub->conn)) {
            kicked[*kicked_len] = sub->conn;
            *kicked_len += 1;
            sub->conn->kick_fn(sub->conn->kick_ctx);
        }
        if (sub->has_max_msgs && sub->delivered >= sub->max_msgs) {
            sub_list_remove_at(list, i);
            router->sub_count -= 1;
            continue;
        }
        i += 1;
    }
    return true;
}

bool mb_router_publish(mb_router *router, mb_slice subject, mb_slice payload) {
    if (router->kick_cap < router->sub_count) {
        return false;
    }
    mb_router_conn **kicked = router->kick_scratch;
    size_t kicked_len = 0;

    mb_literal_bucket *lit = literal_bucket(router, subject, false);
    if (lit != NULL && !deliver_matching_list(router, &lit->subs, subject, payload, false, kicked, &kicked_len)) {
        return false;
    }

    const mb_slice first = first_token(subject);
    mb_wildcard_bucket *wild = wildcard_bucket(router, first, false);
    if (wild != NULL && !deliver_matching_list(router, &wild->subs, subject, payload, true, kicked, &kicked_len)) {
        return false;
    }
    if (!deliver_matching_list(router, &router->wildcard_global, subject, payload, true, kicked, &kicked_len)) {
        return false;
    }

    return true;
}
