#include "router.h"

#include "array.h"
#include "slice.h"

#include <stdlib.h>

static void free_sub(mb_subscription *sub) {
    free(sub->subject);
    free(sub->sid);
    *sub = (mb_subscription){0};
}

static bool sid_eq(const mb_subscription *sub, mb_slice sid) {
    return mb_slice_eq_bytes(sub->sid, sub->sid_len, sid);
}

bool mb_router_subject_has_lvc_prefix(mb_slice subject) {
    static const char prefix[] = MB_LVC_PREFIX;
    return mb_slice_has_prefix(subject, prefix, sizeof prefix - 1);
}

static mb_slice lvc_inner_subject(mb_slice subject) {
    static const char prefix[] = MB_LVC_PREFIX;
    return (mb_slice){.ptr = subject.ptr + sizeof(prefix) - 1, .len = subject.len - (sizeof(prefix) - 1)};
}

static bool sub_list_append(mb_sub_list *list, mb_subscription sub) {
    if (!mb_array_reserve((void **)&list->items, &list->cap, list->len + 1,
                          sizeof list->items[0], 8)) {
        return false;
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
        if (!(ftok.len == 1 && ftok.ptr[0] == '*') && !mb_slice_eq(ftok, stok)) {
            return false;
        }
    }
}

bool mb_router_subject_matches(mb_slice filter, mb_slice subject) {
    return subject_matches(filter, subject);
}

static bool grow_literal_buckets(mb_router *router) {
    return mb_array_reserve((void **)&router->literal, &router->literal_cap,
                            router->literal_len + 1, sizeof router->literal[0], 8);
}

static bool grow_wildcard_buckets(mb_router *router) {
    return mb_array_reserve((void **)&router->wildcard, &router->wildcard_cap,
                            router->wildcard_len + 1, sizeof router->wildcard[0], 8);
}

static bool ensure_kick_capacity(mb_router *router, size_t needed) {
    return mb_array_reserve((void **)&router->kick_scratch, &router->kick_cap,
                            needed, sizeof router->kick_scratch[0], 1);
}

static mb_literal_bucket *literal_bucket(mb_router *router, mb_slice key, bool create) {
    for (size_t i = 0; i < router->literal_len; i += 1) {
        if (mb_slice_eq_bytes(router->literal[i].key, router->literal[i].key_len, key)) {
            return &router->literal[i];
        }
    }
    if (!create || !grow_literal_buckets(router)) {
        return NULL;
    }
    mb_literal_bucket *bucket = &router->literal[router->literal_len];
    *bucket = (mb_literal_bucket){0};
    if (!mb_slice_dup(key, &bucket->key, &bucket->key_len)) {
        return NULL;
    }
    router->literal_len += 1;
    return bucket;
}

static mb_wildcard_bucket *wildcard_bucket(mb_router *router, mb_slice key, bool create) {
    for (size_t i = 0; i < router->wildcard_len; i += 1) {
        if (mb_slice_eq_bytes(router->wildcard[i].key, router->wildcard[i].key_len, key)) {
            return &router->wildcard[i];
        }
    }
    if (!create || !grow_wildcard_buckets(router)) {
        return NULL;
    }
    mb_wildcard_bucket *bucket = &router->wildcard[router->wildcard_len];
    *bucket = (mb_wildcard_bucket){0};
    if (!mb_slice_dup(key, &bucket->key, &bucket->key_len)) {
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

void mb_router_disable_lvc(mb_router *router) {
    router->lvc_enabled = false;
}

static void lvc_filter_free(mb_lvc_filter *filter) {
    free(filter->filter);
    *filter = (mb_lvc_filter){0};
}

bool mb_router_configure_lvc(mb_router *router, const mb_slice *filters, size_t filter_len) {
    router->lvc_enabled = false;
    for (size_t i = 0; i < router->lvc_filter_len; i += 1) {
        lvc_filter_free(&router->lvc_filters[i]);
    }
    router->lvc_filter_len = 0;

    if (filter_len == 0) {
        return true;
    }
    if (!mb_array_reserve((void **)&router->lvc_filters, &router->lvc_filter_cap,
                          filter_len, sizeof router->lvc_filters[0], 8)) {
        return false;
    }
    for (size_t i = 0; i < filter_len; i += 1) {
        mb_lvc_filter *filter = &router->lvc_filters[i];
        *filter = (mb_lvc_filter){0};
        if (!mb_slice_dup(filters[i], &filter->filter, &filter->filter_len)) {
            for (size_t j = 0; j < i; j += 1) {
                lvc_filter_free(&router->lvc_filters[j]);
            }
            return false;
        }
    }
    router->lvc_filter_len = filter_len;
    router->lvc_enabled = true;
    return true;
}

static void lvc_entry_free(mb_lvc_entry *entry) {
    free(entry->subject);
    mb_buf_free(&entry->payload);
    *entry = (mb_lvc_entry){0};
}

static mb_lvc_entry *lvc_entry_find(mb_router *router, mb_slice subject) {
    for (size_t i = 0; i < router->lvc_len; i += 1) {
        if (mb_slice_eq_bytes(router->lvc[i].subject, router->lvc[i].subject_len, subject)) {
            return &router->lvc[i];
        }
    }
    return NULL;
}

static bool lvc_subject_enabled(const mb_router *router, mb_slice subject) {
    if (!router->lvc_enabled) {
        return false;
    }
    for (size_t i = 0; i < router->lvc_filter_len; i += 1) {
        const mb_lvc_filter *filter = &router->lvc_filters[i];
        if (subject_matches((mb_slice){.ptr = filter->filter, .len = filter->filter_len}, subject)) {
            return true;
        }
    }
    return false;
}

static bool lvc_store(mb_router *router, mb_slice subject, mb_slice payload) {
    if (mb_router_subject_has_lvc_prefix(subject) || !lvc_subject_enabled(router, subject)) {
        return true;
    }
    mb_lvc_entry *entry = lvc_entry_find(router, subject);
    if (entry == NULL) {
        if (!mb_array_reserve((void **)&router->lvc, &router->lvc_cap,
                              router->lvc_len + 1, sizeof router->lvc[0], 16)) {
            return false;
        }
        entry = &router->lvc[router->lvc_len];
        *entry = (mb_lvc_entry){0};
        if (!mb_slice_dup(subject, &entry->subject, &entry->subject_len)) {
            return false;
        }
        router->lvc_len += 1;
    }
    mb_buf_clear(&entry->payload);
    return mb_buf_append(&entry->payload, payload.ptr, payload.len);
}

bool mb_router_store_lvc(mb_router *router, mb_slice subject, mb_slice payload) {
    return lvc_store(router, subject, payload);
}

size_t mb_router_lvc_count(const mb_router *router) {
    return router->lvc_len;
}

bool mb_router_lvc_entry(const mb_router *router, size_t index, mb_slice *subject, mb_slice *payload) {
    if (index >= router->lvc_len) {
        return false;
    }
    const mb_lvc_entry *entry = &router->lvc[index];
    *subject = (mb_slice){.ptr = entry->subject, .len = entry->subject_len};
    *payload = (mb_slice){.ptr = entry->payload.ptr, .len = entry->payload.len};
    return true;
}

static bool emit_cached(mb_router *router, mb_router_conn *conn, mb_slice filter, mb_slice sid) {
    if (!router->lvc_enabled) {
        return false;
    }
    for (size_t i = 0; i < router->lvc_len; i += 1) {
        mb_lvc_entry *entry = &router->lvc[i];
        const mb_slice subject = {.ptr = entry->subject, .len = entry->subject_len};
        if (!lvc_subject_enabled(router, subject)) {
            continue;
        }
        if (!subject_matches(filter, subject)) {
            continue;
        }
        const mb_slice payload = {.ptr = entry->payload.ptr, .len = entry->payload.len};
        if (!mb_write_msg_prefixed(&conn->out, MB_LVC_PREFIX, sizeof(MB_LVC_PREFIX) - 1, subject, sid, payload)) {
            return false;
        }
    }
    return true;
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
    for (size_t i = 0; i < router->lvc_len; i += 1) {
        lvc_entry_free(&router->lvc[i]);
    }
    free(router->lvc);
    for (size_t i = 0; i < router->lvc_filter_len; i += 1) {
        lvc_filter_free(&router->lvc_filters[i]);
    }
    free(router->lvc_filters);
    sub_list_free(&router->wildcard_global);
    *router = (mb_router){0};
}

bool mb_router_subscribe(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice sid) {
    const bool is_lvc = mb_router_subject_has_lvc_prefix(subject);
    const mb_slice match_subject = is_lvc ? lvc_inner_subject(subject) : subject;
    if (is_lvc && (!router->lvc_enabled || match_subject.len == 0)) {
        return false;
    }
    if (!ensure_kick_capacity(router, router->sub_count + 1)) {
        return false;
    }

    mb_subscription sub = {.conn = conn, .is_lvc = is_lvc};
    if (!mb_slice_dup(match_subject, &sub.subject, &sub.subject_len)) {
        return false;
    }
    if (!mb_slice_dup(sid, &sub.sid, &sub.sid_len)) {
        free_sub(&sub);
        return false;
    }

    bool ok = false;
    if (is_literal_filter(match_subject)) {
        mb_literal_bucket *bucket = literal_bucket(router, match_subject, true);
        ok = bucket != NULL && sub_list_append(&bucket->subs, sub);
    } else {
        const mb_slice first = first_token(match_subject);
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
    if (is_lvc && !emit_cached(router, conn, match_subject, sid)) {
        mb_router_unsubscribe(router, conn, sid, 0, false);
        return false;
    }
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
        if (sub->is_lvc && !lvc_subject_enabled(router, subject)) {
            i += 1;
            continue;
        }
        const bool wrote = sub->is_lvc
                               ? mb_write_msg_prefixed(&sub->conn->out, MB_LVC_PREFIX, sizeof(MB_LVC_PREFIX) - 1, subject, sid, payload)
                               : mb_write_msg(&sub->conn->out, subject, sid, payload);
        if (!wrote) {
            return false;
        }
        sub->delivered += 1;
        if (sub->conn->kick_fn != NULL && !already_kicked(kicked, *kicked_len, sub->conn)) {
            kicked[*kicked_len] = sub->conn;
            *kicked_len += 1;
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
    if (mb_router_subject_has_lvc_prefix(subject) || !lvc_store(router, subject, payload)) {
        return false;
    }
    if (router->kick_cap < router->sub_count) {
        return false;
    }
    mb_router_conn **kicked = router->kick_scratch;
    size_t kicked_len = 0;
    bool ok = true;

    mb_literal_bucket *lit = literal_bucket(router, subject, false);
    if (lit != NULL && !deliver_matching_list(router, &lit->subs, subject, payload, false, kicked, &kicked_len)) {
        ok = false;
    }

    if (ok) {
        const mb_slice first = first_token(subject);
        mb_wildcard_bucket *wild = wildcard_bucket(router, first, false);
        if (wild != NULL && !deliver_matching_list(router, &wild->subs, subject, payload, true, kicked, &kicked_len)) {
            ok = false;
        }
    }
    if (ok && !deliver_matching_list(router, &router->wildcard_global, subject, payload, true, kicked, &kicked_len)) {
        ok = false;
    }

    for (size_t i = 0; i < kicked_len; i += 1) {
        mb_router_conn *conn = kicked[i];
        if (!conn->closed && conn->kick_fn != NULL) {
            conn->kick_fn(conn->kick_ctx);
        }
    }
    if (ok && router->bridge_fn != NULL && router->bridge_ctx != NULL) {
        router->bridge_fn(router->bridge_ctx, subject, payload);
    }
    return ok;
}
