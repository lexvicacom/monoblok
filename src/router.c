#include "router.h"

#include "array.h"
#include "slice.h"

#include <stdint.h>
#include <stdlib.h>

static void free_sub(mb_subscription *sub) {
    free(sub->subject);
    free(sub->queue);
    free(sub->sid);
    *sub = (mb_subscription){0};
}

static bool sid_eq(const mb_subscription *sub, mb_slice sid) {
    return mb_slice_eq((mb_slice){.ptr = sub->sid, .len = sub->sid_len}, sid);
}

bool mb_router_subject_has_lvc_prefix(mb_slice subject) {
    static const char prefix[] = MB_LVC_PREFIX;
    return mb_slice_has_prefix(subject, prefix, sizeof prefix - 1);
}

bool mb_router_subject_has_stats_prefix(mb_slice subject) {
    static const char prefix[] = MB_STATS_PREFIX;
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

static void note_sub_removed(mb_router *router, const mb_subscription *sub) {
    if (router->sub_count > 0) {
        router->sub_count -= 1;
    }
    if (sub->conn != NULL && sub->conn->sub_count > 0) {
        sub->conn->sub_count -= 1;
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

static bool ensure_close_capacity(mb_router *router, size_t needed) {
    return mb_array_reserve((void **)&router->close_scratch, &router->close_cap,
                            needed, sizeof router->close_scratch[0], 1);
}

static bool ensure_queue_capacity(mb_router *router, size_t needed) {
    return mb_array_reserve((void **)&router->queue_scratch, &router->queue_cap,
                            needed, sizeof router->queue_scratch[0], 1);
}

static bool ensure_publish_capacity(mb_router *router, size_t needed) {
    return ensure_kick_capacity(router, needed) && ensure_close_capacity(router, needed) && ensure_queue_capacity(router, needed);
}

static size_t next_index_cap(size_t needed) {
    size_t cap = 16;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }
    return cap;
}

static void index_insert(mb_router_index_entry *entries, size_t cap, uint64_t hash, size_t index) {
    size_t slot = (size_t)(hash & (uint64_t)(cap - 1));
    while (entries[slot].occupied) {
        slot = (slot + 1) & (cap - 1);
    }
    entries[slot] = (mb_router_index_entry){.hash = hash, .index = index, .occupied = true};
}

static void clear_index(mb_router_index_entry *entries, size_t cap) {
    for (size_t i = 0; i < cap; i += 1) {
        entries[i] = (mb_router_index_entry){0};
    }
}

static bool literal_index_ensure(mb_router *router, size_t needed) {
    if (router->literal_index_cap != 0 && needed < router->literal_index_cap / 2) {
        return true;
    }
    if (needed > SIZE_MAX / 4) {
        return false;
    }
    const size_t cap = next_index_cap(needed == 0 ? 16 : needed * 4);
    if (cap == 0) {
        return false;
    }
    mb_router_index_entry *entries = calloc(cap, sizeof entries[0]);
    if (entries == NULL) {
        return false;
    }
    for (size_t i = 0; i < router->literal_len; i += 1) {
        const mb_slice key = {.ptr = router->literal[i].key, .len = router->literal[i].key_len};
        index_insert(entries, cap, mb_slice_hash(key), i);
    }
    free(router->literal_index);
    router->literal_index = entries;
    router->literal_index_cap = cap;
    return true;
}

static bool wildcard_index_ensure(mb_router *router, size_t needed) {
    if (router->wildcard_index_cap != 0 && needed < router->wildcard_index_cap / 2) {
        return true;
    }
    if (needed > SIZE_MAX / 4) {
        return false;
    }
    const size_t cap = next_index_cap(needed == 0 ? 16 : needed * 4);
    if (cap == 0) {
        return false;
    }
    mb_router_index_entry *entries = calloc(cap, sizeof entries[0]);
    if (entries == NULL) {
        return false;
    }
    for (size_t i = 0; i < router->wildcard_len; i += 1) {
        const mb_slice key = {.ptr = router->wildcard[i].key, .len = router->wildcard[i].key_len};
        index_insert(entries, cap, mb_slice_hash(key), i);
    }
    free(router->wildcard_index);
    router->wildcard_index = entries;
    router->wildcard_index_cap = cap;
    return true;
}

static bool lvc_index_ensure(mb_router *router, size_t needed) {
    if (router->lvc_index_cap != 0 && needed < router->lvc_index_cap / 2) {
        return true;
    }
    if (needed > SIZE_MAX / 4) {
        return false;
    }
    const size_t cap = next_index_cap(needed == 0 ? 16 : needed * 4);
    if (cap == 0) {
        return false;
    }
    mb_router_index_entry *entries = calloc(cap, sizeof entries[0]);
    if (entries == NULL) {
        return false;
    }
    for (size_t i = 0; i < router->lvc_len; i += 1) {
        const mb_slice key = {.ptr = router->lvc[i].subject, .len = router->lvc[i].subject_len};
        index_insert(entries, cap, mb_slice_hash(key), i);
    }
    free(router->lvc_index);
    router->lvc_index = entries;
    router->lvc_index_cap = cap;
    return true;
}

static void literal_index_refresh(mb_router *router) {
    if (router->literal_index_cap == 0) {
        return;
    }
    clear_index(router->literal_index, router->literal_index_cap);
    for (size_t i = 0; i < router->literal_len; i += 1) {
        const mb_slice key = {.ptr = router->literal[i].key, .len = router->literal[i].key_len};
        index_insert(router->literal_index, router->literal_index_cap, mb_slice_hash(key), i);
    }
}

static void wildcard_index_refresh(mb_router *router) {
    if (router->wildcard_index_cap == 0) {
        return;
    }
    clear_index(router->wildcard_index, router->wildcard_index_cap);
    for (size_t i = 0; i < router->wildcard_len; i += 1) {
        const mb_slice key = {.ptr = router->wildcard[i].key, .len = router->wildcard[i].key_len};
        index_insert(router->wildcard_index, router->wildcard_index_cap, mb_slice_hash(key), i);
    }
}

static size_t literal_index_find(const mb_router *router, mb_slice key, uint64_t hash) {
    if (router->literal_index_cap == 0) {
        return SIZE_MAX;
    }
    size_t slot = (size_t)(hash & (uint64_t)(router->literal_index_cap - 1));
    for (;;) {
        const mb_router_index_entry *entry = &router->literal_index[slot];
        if (!entry->occupied) {
            return SIZE_MAX;
        }
        const mb_literal_bucket *bucket = &router->literal[entry->index];
        if (entry->hash == hash &&
            mb_slice_eq((mb_slice){.ptr = bucket->key, .len = bucket->key_len}, key)) {
            return entry->index;
        }
        slot = (slot + 1) & (router->literal_index_cap - 1);
    }
}

static size_t wildcard_index_find(const mb_router *router, mb_slice key, uint64_t hash) {
    if (router->wildcard_index_cap == 0) {
        return SIZE_MAX;
    }
    size_t slot = (size_t)(hash & (uint64_t)(router->wildcard_index_cap - 1));
    for (;;) {
        const mb_router_index_entry *entry = &router->wildcard_index[slot];
        if (!entry->occupied) {
            return SIZE_MAX;
        }
        const mb_wildcard_bucket *bucket = &router->wildcard[entry->index];
        if (entry->hash == hash &&
            mb_slice_eq((mb_slice){.ptr = bucket->key, .len = bucket->key_len}, key)) {
            return entry->index;
        }
        slot = (slot + 1) & (router->wildcard_index_cap - 1);
    }
}

static size_t lvc_index_find(const mb_router *router, mb_slice key, uint64_t hash) {
    if (router->lvc_index_cap == 0) {
        return SIZE_MAX;
    }
    size_t slot = (size_t)(hash & (uint64_t)(router->lvc_index_cap - 1));
    for (;;) {
        const mb_router_index_entry *entry = &router->lvc_index[slot];
        if (!entry->occupied) {
            return SIZE_MAX;
        }
        const mb_lvc_entry *lvc = &router->lvc[entry->index];
        if (entry->hash == hash &&
            mb_slice_eq((mb_slice){.ptr = lvc->subject, .len = lvc->subject_len}, key)) {
            return entry->index;
        }
        slot = (slot + 1) & (router->lvc_index_cap - 1);
    }
}

static mb_literal_bucket *literal_bucket(mb_router *router, mb_slice key, bool create) {
    const uint64_t hash = mb_slice_hash(key);
    const size_t found = literal_index_find(router, key, hash);
    if (found != SIZE_MAX) {
        return &router->literal[found];
    }
    if (router->literal_index_cap == 0) {
        for (size_t i = 0; i < router->literal_len; i += 1) {
            if (mb_slice_eq((mb_slice){.ptr = router->literal[i].key, .len = router->literal[i].key_len}, key)) {
                return &router->literal[i];
            }
        }
    }
    if (!create || !literal_index_ensure(router, router->literal_len + 1) || !grow_literal_buckets(router)) {
        return NULL;
    }
    mb_literal_bucket *bucket = &router->literal[router->literal_len];
    *bucket = (mb_literal_bucket){0};
    if (!mb_slice_dup(key, &bucket->key, &bucket->key_len)) {
        return NULL;
    }
    index_insert(router->literal_index, router->literal_index_cap, hash, router->literal_len);
    router->literal_len += 1;
    return bucket;
}

static mb_wildcard_bucket *wildcard_bucket(mb_router *router, mb_slice key, bool create) {
    const uint64_t hash = mb_slice_hash(key);
    const size_t found = wildcard_index_find(router, key, hash);
    if (found != SIZE_MAX) {
        return &router->wildcard[found];
    }
    if (router->wildcard_index_cap == 0) {
        for (size_t i = 0; i < router->wildcard_len; i += 1) {
            if (mb_slice_eq((mb_slice){.ptr = router->wildcard[i].key, .len = router->wildcard[i].key_len}, key)) {
                return &router->wildcard[i];
            }
        }
    }
    if (!create || !wildcard_index_ensure(router, router->wildcard_len + 1) || !grow_wildcard_buckets(router)) {
        return NULL;
    }
    mb_wildcard_bucket *bucket = &router->wildcard[router->wildcard_len];
    *bucket = (mb_wildcard_bucket){0};
    if (!mb_slice_dup(key, &bucket->key, &bucket->key_len)) {
        return NULL;
    }
    index_insert(router->wildcard_index, router->wildcard_index_cap, hash, router->wildcard_len);
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
    literal_index_refresh(router);
}

static void remove_wildcard_bucket_at(mb_router *router, size_t i) {
    free(router->wildcard[i].key);
    free(router->wildcard[i].subs.items);
    router->wildcard_len -= 1;
    if (i != router->wildcard_len) {
        router->wildcard[i] = router->wildcard[router->wildcard_len];
        router->wildcard[router->wildcard_len] = (mb_wildcard_bucket){0};
    }
    wildcard_index_refresh(router);
}

void mb_router_init(mb_router *router) {
    *router = (mb_router){0};
    router->queue_rng = UINT64_C(0x9e3779b97f4a7c15);
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
    for (size_t i = 0; i < filter_len; i += 1) {
        if (!mb_proto_subject_valid(filters[i], true)) {
            return false;
        }
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
    const uint64_t hash = mb_slice_hash(subject);
    const size_t found = lvc_index_find(router, subject, hash);
    if (found != SIZE_MAX) {
        return &router->lvc[found];
    }
    if (router->lvc_index_cap == 0) {
        for (size_t i = 0; i < router->lvc_len; i += 1) {
            if (mb_slice_eq((mb_slice){.ptr = router->lvc[i].subject, .len = router->lvc[i].subject_len}, subject)) {
                return &router->lvc[i];
            }
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

static bool lvc_stats_subject_enabled(const mb_router *router, mb_slice subject) {
    if (!router->lvc_enabled) {
        return false;
    }
    for (size_t i = 0; i < router->lvc_filter_len; i += 1) {
        const mb_lvc_filter *filter = &router->lvc_filters[i];
        const mb_slice filter_slice = {.ptr = filter->filter, .len = filter->filter_len};
        if (mb_router_subject_has_stats_prefix(filter_slice) && subject_matches(filter_slice, subject)) {
            return true;
        }
    }
    return false;
}

static bool lvc_store(mb_router *router, mb_slice subject, mb_slice payload) {
    if (mb_router_subject_has_lvc_prefix(subject)) {
        return true;
    }
    if (mb_router_subject_has_stats_prefix(subject)) {
        if (!lvc_stats_subject_enabled(router, subject)) {
            return true;
        }
    } else if (!lvc_subject_enabled(router, subject)) {
        return true;
    }
    if (!mb_proto_subject_valid(subject, false)) {
        return false;
    }
    mb_lvc_entry *entry = lvc_entry_find(router, subject);
    const size_t old_payload_len = entry == NULL ? 0 : entry->payload.len;
    if (router->lvc_payload_bytes < old_payload_len) {
        return false;
    }
    const size_t payload_base = router->lvc_payload_bytes - old_payload_len;
    if (payload_base > MB_MAX_LVC_BYTES || payload.len > MB_MAX_LVC_BYTES - payload_base) {
        return false;
    }
    if (entry == NULL) {
        if (router->lvc_len >= MB_MAX_LVC_ENTRIES) {
            return false;
        }
        const uint64_t hash = mb_slice_hash(subject);
        if (!lvc_index_ensure(router, router->lvc_len + 1)) {
            return false;
        }
        if (!mb_array_reserve((void **)&router->lvc, &router->lvc_cap,
                              router->lvc_len + 1, sizeof router->lvc[0], 16)) {
            return false;
        }
        entry = &router->lvc[router->lvc_len];
        *entry = (mb_lvc_entry){0};
        if (!mb_slice_dup(subject, &entry->subject, &entry->subject_len)) {
            return false;
        }
        index_insert(router->lvc_index, router->lvc_index_cap, hash, router->lvc_len);
        router->lvc_len += 1;
    }
    mb_buf_clear(&entry->payload);
    if (!mb_buf_append(&entry->payload, payload.ptr, payload.len)) {
        return false;
    }
    router->lvc_payload_bytes = payload_base + payload.len;
    return true;
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

bool mb_router_lvc_latest(const mb_router *router, mb_slice subject, mb_slice *payload) {
    if (payload == NULL || !mb_proto_subject_valid(subject, false) || !lvc_subject_enabled(router, subject)) {
        return false;
    }
    const uint64_t hash = mb_slice_hash(subject);
    const size_t found = lvc_index_find(router, subject, hash);
    if (found != SIZE_MAX) {
        const mb_lvc_entry *entry = &router->lvc[found];
        *payload = (mb_slice){.ptr = entry->payload.ptr, .len = entry->payload.len};
        return true;
    }
    if (router->lvc_index_cap == 0) {
        for (size_t i = 0; i < router->lvc_len; i += 1) {
            const mb_lvc_entry *entry = &router->lvc[i];
            if (mb_slice_eq((mb_slice){.ptr = entry->subject, .len = entry->subject_len}, subject)) {
                *payload = (mb_slice){.ptr = entry->payload.ptr, .len = entry->payload.len};
                return true;
            }
        }
    }
    return false;
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
        size_t frame_len = 0;
        if (!mb_msg_frame_len_prefixed(&frame_len, sizeof(MB_LVC_PREFIX) - 1, subject, sid, payload) ||
            conn->out.len > MB_MAX_PENDING ||
            frame_len > MB_MAX_PENDING - conn->out.len) {
            return false;
        }
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
    free(router->literal_index);
    free(router->wildcard_index);
    free(router->kick_scratch);
    free(router->close_scratch);
    free(router->queue_scratch);
    for (size_t i = 0; i < router->lvc_len; i += 1) {
        lvc_entry_free(&router->lvc[i]);
    }
    free(router->lvc);
    free(router->lvc_index);
    for (size_t i = 0; i < router->lvc_filter_len; i += 1) {
        lvc_filter_free(&router->lvc_filters[i]);
    }
    free(router->lvc_filters);
    sub_list_free(&router->wildcard_global);
    *router = (mb_router){0};
}

bool mb_router_subscribe_queue(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice queue, mb_slice sid) {
    if (conn == NULL) {
        return false;
    }
    const bool is_lvc = mb_router_subject_has_lvc_prefix(subject);
    const mb_slice match_subject = is_lvc ? lvc_inner_subject(subject) : subject;
    if (is_lvc && (!router->lvc_enabled || match_subject.len == 0)) {
        return false;
    }
    if (!mb_proto_subject_valid(match_subject, true) ||
        !mb_proto_token_valid(sid) ||
        (queue.len != 0 && !mb_proto_token_valid(queue))) {
        return false;
    }
    if (conn->sub_count >= MB_MAX_SUBS_PER_CONN || router->sub_count >= MB_MAX_SUBS_TOTAL) {
        return false;
    }
    if (!ensure_publish_capacity(router, router->sub_count + 1)) {
        return false;
    }

    mb_subscription sub = {.conn = conn, .is_lvc = is_lvc};
    if (!mb_slice_dup(match_subject, &sub.subject, &sub.subject_len)) {
        return false;
    }
    if (queue.len != 0 && !mb_slice_dup(queue, &sub.queue, &sub.queue_len)) {
        free_sub(&sub);
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
    conn->sub_count += 1;
    if (is_lvc && !emit_cached(router, conn, match_subject, sid)) {
        mb_router_unsubscribe(router, conn, sid, 0, false);
        return false;
    }
    return true;
}

bool mb_router_subscribe(mb_router *router, mb_router_conn *conn, mb_slice subject, mb_slice sid) {
    return mb_router_subscribe_queue(router, conn, subject, (mb_slice){0}, sid);
}

static void remove_for_conn_from_list(mb_router *router, mb_sub_list *list, mb_router_conn *conn) {
    size_t i = 0;
    while (i < list->len) {
        if (list->items[i].conn != conn) {
            i += 1;
            continue;
        }
        note_sub_removed(router, &list->items[i]);
        sub_list_remove_at(list, i);
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
            note_sub_removed(router, sub);
            sub_list_remove_at(list, i);
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

static uint64_t queue_random(mb_router *router) {
    uint64_t x = router->queue_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    router->queue_rng = x == 0 ? UINT64_C(0x9e3779b97f4a7c15) : x;
    return router->queue_rng * UINT64_C(2685821657736338717);
}

static bool sub_deliverable(const mb_router *router, const mb_subscription *sub, mb_slice subject, bool check_filter) {
    if (sub->conn->closed) {
        return false;
    }
    if (check_filter && !subject_matches((mb_slice){.ptr = sub->subject, .len = sub->subject_len}, subject)) {
        return false;
    }
    return !sub->is_lvc || lvc_subject_enabled(router, subject);
}

static bool queue_group_eq(const mb_queue_delivery *group, const mb_subscription *sub) {
    return group->is_lvc == sub->is_lvc &&
           mb_slice_eq(group->subject, (mb_slice){.ptr = sub->subject, .len = sub->subject_len}) &&
           mb_slice_eq(group->queue, (mb_slice){.ptr = sub->queue, .len = sub->queue_len});
}

static bool collect_queue_delivery(mb_router *router, mb_subscription *sub, size_t index, size_t *group_len) {
    mb_queue_delivery *group = NULL;
    for (size_t i = 0; i < *group_len; i += 1) {
        if (queue_group_eq(&router->queue_scratch[i], sub)) {
            group = &router->queue_scratch[i];
            break;
        }
    }
    if (group == NULL) {
        if (*group_len >= router->queue_cap) {
            return false;
        }
        group = &router->queue_scratch[*group_len];
        *group = (mb_queue_delivery){
            .subject = {.ptr = sub->subject, .len = sub->subject_len},
            .queue = {.ptr = sub->queue, .len = sub->queue_len},
            .selected = index,
            .is_lvc = sub->is_lvc,
        };
        *group_len += 1;
    }

    group->seen += 1;
    if (group->seen == 1 || queue_random(router) % group->seen == 0) {
        group->selected = index;
    }
    return true;
}

static bool queue_selected(const mb_router *router, const mb_subscription *sub, size_t index, size_t group_len) {
    if (sub->queue_len == 0) {
        return true;
    }
    for (size_t i = 0; i < group_len; i += 1) {
        if (router->queue_scratch[i].selected == index && queue_group_eq(&router->queue_scratch[i], sub)) {
            return true;
        }
    }
    return false;
}

static bool queue_conn_close(mb_router *router, mb_router_conn *conn,
                             mb_router_conn **closed, size_t *closed_len) {
    if (conn->closed) {
        return true;
    }
    conn->closed = true;
    if (conn->close_fn == NULL || conn->close_seen_epoch == router->kick_epoch) {
        return true;
    }
    if (*closed_len >= router->close_cap) {
        return false;
    }
    conn->close_seen_epoch = router->kick_epoch;
    closed[*closed_len] = conn;
    *closed_len += 1;
    return true;
}

static bool deliver_to_sub(mb_router *router, mb_subscription *sub, mb_slice subject, mb_slice payload, mb_slice reply_to,
                           mb_router_conn **kicked, size_t *kicked_len,
                           mb_router_conn **closed, size_t *closed_len) {
    const mb_slice sid = {.ptr = sub->sid, .len = sub->sid_len};
    const char *prefix = sub->is_lvc ? MB_LVC_PREFIX : "";
    const size_t prefix_len = sub->is_lvc ? sizeof(MB_LVC_PREFIX) - 1 : 0;
    size_t frame_len = 0;
    const bool measured = sub->conn->msg_len_fn != NULL
                              ? sub->conn->msg_len_fn(sub->conn->write_msg_ctx, &frame_len, prefix, prefix_len, subject, sid, reply_to, payload)
                              : mb_msg_frame_len_prefixed_with_reply(&frame_len, prefix_len, subject, sid, reply_to, payload);
    if (!measured) {
        return false;
    }
    if (frame_len == 0) {
        sub->delivered += 1;
        return true;
    }
    if (sub->conn->out.len > MB_MAX_PENDING || frame_len > MB_MAX_PENDING - sub->conn->out.len) {
        return queue_conn_close(router, sub->conn, closed, closed_len);
    }
    const bool wrote = sub->conn->write_msg_fn != NULL
                           ? sub->conn->write_msg_fn(sub->conn->write_msg_ctx, &sub->conn->out, prefix, prefix_len, subject, sid, reply_to, payload)
                           : mb_write_msg_prefixed_with_reply(&sub->conn->out, prefix, prefix_len, subject, sid, reply_to, payload);
    if (!wrote) {
        return false;
    }
    sub->delivered += 1;
    if (sub->conn->kick_fn != NULL && sub->conn->kick_seen_epoch != router->kick_epoch) {
        sub->conn->kick_seen_epoch = router->kick_epoch;
        kicked[*kicked_len] = sub->conn;
        *kicked_len += 1;
    }
    return true;
}

static void remove_delivered_max(mb_router *router, mb_sub_list *list) {
    size_t i = 0;
    while (i < list->len) {
        mb_subscription *sub = &list->items[i];
        if (sub->has_max_msgs && sub->delivered >= sub->max_msgs) {
            note_sub_removed(router, sub);
            sub_list_remove_at(list, i);
            continue;
        }
        i += 1;
    }
}

static bool deliver_matching_list(mb_router *router, mb_sub_list *list, mb_slice subject, mb_slice payload, mb_slice reply_to,
                                  bool check_filter, mb_router_conn **kicked, size_t *kicked_len,
                                  mb_router_conn **closed, size_t *closed_len) {
    size_t group_len = 0;
    // First choose one member per matching queue group without mutating the list.
    for (size_t i = 0; i < list->len; i += 1) {
        mb_subscription *sub = &list->items[i];
        if (sub->queue_len == 0 || !sub_deliverable(router, sub, subject, check_filter)) {
            continue;
        }
        if (!collect_queue_delivery(router, sub, i, &group_len)) {
            return false;
        }
    }

    // Then deliver normal subscribers plus the selected queue members.
    for (size_t i = 0; i < list->len; i += 1) {
        mb_subscription *sub = &list->items[i];
        if (!sub_deliverable(router, sub, subject, check_filter) || !queue_selected(router, sub, i, group_len)) {
            continue;
        }
        if (!deliver_to_sub(router, sub, subject, payload, reply_to, kicked, kicked_len, closed, closed_len)) {
            remove_delivered_max(router, list);
            return false;
        }
    }
    remove_delivered_max(router, list);
    return true;
}

bool mb_router_publish(mb_router *router, mb_slice subject, mb_slice payload) {
    return mb_router_publish_with_reply(router, subject, payload, (mb_slice){0});
}

bool mb_router_publish_with_reply(mb_router *router, mb_slice subject, mb_slice payload, mb_slice reply_to) {
    return mb_router_publish_with_reply_and_options(router, subject, payload, reply_to, (mb_router_publish_options){0});
}

bool mb_router_publish_with_options(mb_router *router, mb_slice subject, mb_slice payload, mb_router_publish_options options) {
    return mb_router_publish_with_reply_and_options(router, subject, payload, (mb_slice){0}, options);
}

bool mb_router_publish_with_reply_and_options(mb_router *router, mb_slice subject, mb_slice payload, mb_slice reply_to,
                                              mb_router_publish_options options) {
    if (!mb_proto_subject_valid(subject, false)) {
        return false;
    }
    if (reply_to.len != 0 && !mb_proto_subject_valid(reply_to, false)) {
        return false;
    }
    if (mb_router_subject_has_lvc_prefix(subject) || !lvc_store(router, subject, payload)) {
        return false;
    }
    if (router->kick_cap < router->sub_count ||
        router->close_cap < router->sub_count ||
        router->queue_cap < router->sub_count) {
        return false;
    }
    mb_router_conn **kicked = router->kick_scratch;
    mb_router_conn **closed = router->close_scratch;
    size_t kicked_len = 0;
    size_t closed_len = 0;
    router->kick_epoch += 1;
    if (router->kick_epoch == 0) {
        router->kick_epoch = 1;
    }
    bool ok = true;

    mb_literal_bucket *lit = literal_bucket(router, subject, false);
    if (lit != NULL && !deliver_matching_list(router, &lit->subs, subject, payload, reply_to, false, kicked, &kicked_len, closed, &closed_len)) {
        ok = false;
    }

    if (ok) {
        const mb_slice first = first_token(subject);
        mb_wildcard_bucket *wild = wildcard_bucket(router, first, false);
        if (wild != NULL && !deliver_matching_list(router, &wild->subs, subject, payload, reply_to, true, kicked, &kicked_len, closed, &closed_len)) {
            ok = false;
        }
    }
    if (ok && !deliver_matching_list(router, &router->wildcard_global, subject, payload, reply_to, true, kicked, &kicked_len, closed, &closed_len)) {
        ok = false;
    }

    for (size_t i = 0; i < closed_len; i += 1) {
        mb_router_conn *conn = closed[i];
        if (conn->close_fn != NULL) {
            conn->close_fn(conn->close_ctx);
        }
    }
    for (size_t i = 0; i < kicked_len; i += 1) {
        mb_router_conn *conn = kicked[i];
        if (!conn->closed && conn->kick_fn != NULL) {
            conn->kick_fn(conn->kick_ctx);
        }
    }
    if (ok && router->bridge_fn != NULL && router->bridge_ctx != NULL) {
        router->bridge_fn(router->bridge_ctx, subject, payload, options);
    }
    return ok;
}
