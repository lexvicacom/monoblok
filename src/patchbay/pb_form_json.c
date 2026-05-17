// Forms: json-get, json-demux!.
// JSON lookup and demux forms backed by yyjson.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

#include "router.h"

static bool path_token(pb_slice path, size_t *pos, pb_slice *out) {
    if (*pos >= path.len) {
        return false;
    }
    const size_t start = *pos;
    while (*pos < path.len && path.ptr[*pos] != '.') {
        *pos += 1;
    }
    if (*pos == start) {
        return false;
    }
    *out = (pb_slice){.ptr = path.ptr + start, .len = *pos - start};
    if (*pos < path.len && path.ptr[*pos] == '.') {
        *pos += 1;
    }
    return true;
}

static bool token_index(pb_slice s, size_t *out) {
    if (s.len == 0) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') {
            return false;
        }
        n = n * 10 + (size_t)(s.ptr[i] - '0');
    }
    *out = n;
    return true;
}

static yyjson_val *json_lookup(yyjson_val *root, pb_slice path) {
    yyjson_val *cur = root;
    size_t pos = 0;
    pb_slice tok = {0};
    while (path_token(path, &pos, &tok)) {
        if (yyjson_is_obj(cur)) {
            cur = yyjson_obj_getn(cur, tok.ptr, tok.len);
        } else if (yyjson_is_arr(cur)) {
            size_t idx = 0;
            cur = token_index(tok, &idx) ? yyjson_arr_get(cur, idx) : NULL;
        } else {
            return NULL;
        }
        if (cur == NULL) {
            return NULL;
        }
    }
    return pos == path.len ? cur : NULL;
}

static bool json_path_valid(pb_slice path) {
    if (path.len == 0) {
        return false;
    }
    size_t tokens = 1;
    for (size_t i = 0; i < path.len; i += 1) {
        if (path.ptr[i] != '.') {
            continue;
        }
        if (i == 0 || i + 1 == path.len || path.ptr[i + 1] == '.') {
            return false;
        }
        tokens += 1;
        if (tokens > 4) {
            return false;
        }
    }
    return true;
}

static void *json_arena_malloc(void *ctx, size_t size) {
    return pb_arena_alloc(ctx, size == 0 ? 1 : size, _Alignof(max_align_t));
}

static void *json_arena_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    void *next = pb_arena_alloc(ctx, size == 0 ? 1 : size, _Alignof(max_align_t));
    if (next == NULL) {
        return NULL;
    }
    if (ptr != NULL) {
        memcpy(next, ptr, old_size < size ? old_size : size);
    }
    return next;
}

static void json_arena_free(void *ctx, void *ptr) {
    (void)ctx;
    (void)ptr;
}

static yyjson_doc *read_json_slice(pb_eval_ctx *ctx, pb_slice source) {
    yyjson_alc alc = {
        .malloc = json_arena_malloc,
        .realloc = json_arena_realloc,
        .free = json_arena_free,
        .ctx = ctx->arena,
    };
    return yyjson_read_opts((char *)(void *)source.ptr, source.len, YYJSON_READ_NOFLAG, &alc, NULL);
}

static pb_eval_result json_scalar_value(pb_eval_ctx *ctx, yyjson_val *v) {
    if (v == NULL || yyjson_is_null(v) || yyjson_is_obj(v) || yyjson_is_arr(v)) {
        return nil();
    }
    if (yyjson_is_bool(v)) {
        return ok((pb_value){.kind = PB_BOOL, .boolean = yyjson_get_bool(v)});
    }
    if (yyjson_is_num(v)) {
        return ok((pb_value){.kind = PB_NUMBER, .number = yyjson_get_num(v)});
    }
    if (yyjson_is_str(v)) {
        const size_t len = yyjson_get_len(v);
        char *owned = pb_arena_memdup(ctx->arena, yyjson_get_str(v), len);
        if (owned == NULL) {
            return fail(PB_EVAL_OOM);
        }
        return ok((pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = len}});
    }
    return nil();
}

static pb_eval_result call_json_get(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice path = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &path) || !as_string(args.items[1], &payload) || !json_path_valid(path)) {
        return fail(PB_EVAL_TYPE);
    }

    yyjson_doc *doc = read_json_slice(ctx, payload);
    if (doc == NULL) {
        return nil();
    }
    yyjson_val *found = json_lookup(yyjson_doc_get_root(doc), path);
    const pb_eval_result r = json_scalar_value(ctx, found);
    yyjson_doc_free(doc);
    return r;
}

static pb_slice path_leaf(pb_slice path) {
    size_t start = 0;
    for (size_t i = 0; i < path.len; i += 1) {
        if (path.ptr[i] == '.') {
            start = i + 1;
        }
    }
    return (pb_slice){.ptr = path.ptr + start, .len = path.len - start};
}

static bool demux_override_spec(pb_value spec) {
    return spec.kind == PB_VECTOR &&
           spec.seq.len == 2 &&
           (spec.seq.items[0].kind == PB_STRING || spec.seq.items[0].kind == PB_SYMBOL) &&
           (spec.seq.items[1].kind == PB_STRING || spec.seq.items[1].kind == PB_SYMBOL);
}

static bool demux_spec(pb_value spec, bool leaf, pb_slice *path, pb_slice *suffix) {
    if (spec.kind == PB_STRING || spec.kind == PB_SYMBOL) {
        *path = spec.text;
        *suffix = leaf ? path_leaf(spec.text) : spec.text;
        return json_path_valid(*path) && suffix->len != 0;
    }
    if (demux_override_spec(spec)) {
        *path = spec.seq.items[0].text;
        *suffix = spec.seq.items[1].text;
        return json_path_valid(*path) && suffix->len != 0;
    }
    return false;
}

static pb_eval_error publish_json_demux_one(pb_eval_ctx *ctx, yyjson_val *root, pb_value spec, bool leaf) {
    pb_slice path = {0};
    pb_slice suffix = {0};
    if (!demux_spec(spec, leaf, &path, &suffix)) {
        return PB_EVAL_TYPE;
    }

    pb_eval_result scalar = json_scalar_value(ctx, json_lookup(root, path));
    if (scalar.err != PB_EVAL_OK) {
        return scalar.err;
    }
    if (scalar.value.kind == PB_NIL) {
        return PB_EVAL_OK;
    }

    const size_t subject_len = ctx->subject.len + 1 + suffix.len;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return PB_EVAL_OOM;
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    subject_ptr[ctx->subject.len] = '.';
    memcpy(subject_ptr + ctx->subject.len + 1, suffix.ptr, suffix.len);
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject_ptr, .len = subject_len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject)) {
        return PB_EVAL_INVALID_SUBJECT;
    }

    pb_slice payload = {0};
    if (!coerce_payload(ctx, scalar.value, &payload)) {
        return PB_EVAL_TYPE;
    }
    if (ctx->publish == NULL ||
        !ctx->publish(ctx->publish_ctx, (pb_slice){.ptr = subject_ptr, .len = subject_len}, payload)) {
        return PB_EVAL_PUBLISH_FAILED;
    }
    return PB_EVAL_OK;
}

static pb_eval_result call_json_demux(pb_eval_ctx *ctx, pb_values args) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice payload = {0};
    if (!as_string(args.items[args.len - 1], &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    bool leaf = false;
    size_t spec_start = 0;
    if (args.items[0].kind == PB_KEYWORD) {
        if (!text_eq(args.items[0].text, "leaf")) {
            return fail(PB_EVAL_UNKNOWN_SYMBOL);
        }
        leaf = true;
        spec_start = 1;
        if (spec_start + 1 >= args.len) {
            return fail(PB_EVAL_ARITY);
        }
    }

    yyjson_doc *doc = read_json_slice(ctx, payload);
    if (doc == NULL) {
        return nil();
    }

    pb_eval_error err = PB_EVAL_OK;
    if (args.len == spec_start + 2 &&
        args.items[spec_start].kind == PB_VECTOR &&
        !demux_override_spec(args.items[spec_start])) {
        for (size_t i = 0; i < args.items[spec_start].seq.len; i += 1) {
            err = publish_json_demux_one(ctx, yyjson_doc_get_root(doc), args.items[spec_start].seq.items[i], leaf);
            if (err != PB_EVAL_OK) {
                break;
            }
        }
    } else {
        for (size_t i = spec_start; i + 1 < args.len; i += 1) {
            err = publish_json_demux_one(ctx, yyjson_doc_get_root(doc), args.items[i], leaf);
            if (err != PB_EVAL_OK) {
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    if (err != PB_EVAL_OK) {
        return fail(err);
    }
    return nil();
}
