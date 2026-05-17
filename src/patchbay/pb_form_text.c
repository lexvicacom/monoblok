// Forms: str-concat, contains?, starts-with?, ends-with?, subject-append,
// subject-token, subject-with, publish!, publish.
// Text, subject, and publish forms.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

#include "router.h"

static pb_eval_result call_str_concat(pb_eval_ctx *ctx, pb_values args) {
    size_t total = 0;
    for (size_t i = 0; i < args.len; i += 1) {
        pb_slice s = {0};
        if (!as_string(args.items[i], &s)) {
            return fail(PB_EVAL_TYPE);
        }
        total += s.len;
    }

    char *out = pb_arena_alloc(ctx->arena, total == 0 ? 1 : total, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t off = 0;
    for (size_t i = 0; i < args.len; i += 1) {
        pb_slice s = {0};
        (void)as_string(args.items[i], &s);
        memcpy(out + off, s.ptr, s.len);
        off += s.len;
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}

static pb_eval_result call_contains(pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    if (args.items[0].kind == PB_VECTOR) {
        for (size_t i = 0; i < args.items[0].seq.len; i += 1) {
            if (value_eq(args.items[0].seq.items[i], args.items[1])) {
                return ok(bool_value(true));
            }
        }
        return ok(bool_value(false));
    }

    pb_slice hay = {0};
    pb_slice needle = {0};
    if (!as_string(args.items[0], &hay) || !as_string(args.items[1], &needle)) {
        return fail(PB_EVAL_TYPE);
    }
    if (needle.len == 0) {
        return ok(bool_value(true));
    }
    for (size_t i = 0; i + needle.len <= hay.len; i += 1) {
        if (memcmp(hay.ptr + i, needle.ptr, needle.len) == 0) {
            return ok(bool_value(true));
        }
    }
    return ok(bool_value(false));
}

typedef enum affix_kind { AFFIX_STARTS, AFFIX_ENDS } affix_kind;

static pb_eval_result call_affix(pb_values args, affix_kind kind) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice s = {0};
    pb_slice affix = {0};
    if (!as_string(args.items[0], &s) || !as_string(args.items[1], &affix)) {
        return fail(PB_EVAL_TYPE);
    }
    if (affix.len > s.len) {
        return ok(bool_value(false));
    }
    const char *at = kind == AFFIX_STARTS ? s.ptr : s.ptr + s.len - affix.len;
    return ok(bool_value(memcmp(at, affix.ptr, affix.len) == 0));
}

static pb_eval_result call_subject_append(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice suffix = {0};
    if (!as_string(args.items[0], &suffix)) {
        return fail(PB_EVAL_TYPE);
    }

    const size_t len = ctx->subject.len + 1 + suffix.len;
    char *out = pb_arena_alloc(ctx->arena, len, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    memcpy(out, ctx->subject.ptr, ctx->subject.len);
    out[ctx->subject.len] = '.';
    memcpy(out + ctx->subject.len + 1, suffix.ptr, suffix.len);
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = len}});
}

static pb_eval_result call_subject_token(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1 ||
        args.items[0].kind != PB_NUMBER ||
        args.items[0].number < 0 ||
        floor(args.items[0].number) != args.items[0].number) {
        return fail(PB_EVAL_ARITY);
    }

    const size_t want = (size_t)args.items[0].number;
    size_t tok = 0;
    size_t start = 0;
    for (size_t i = 0; i <= ctx->subject.len; i += 1) {
        if (i == ctx->subject.len || ctx->subject.ptr[i] == '.') {
            if (tok == want) {
                return ok((pb_value){.kind = PB_STRING, .text = {.ptr = ctx->subject.ptr + start, .len = i - start}});
            }
            tok += 1;
            start = i + 1;
        }
    }
    return nil();
}

static pb_eval_result call_subject_with(pb_eval_ctx *ctx, pb_values args) {
    pb_values toks = args;
    if (args.len == 1 && args.items[0].kind == PB_VECTOR) {
        toks = args.items[0].seq;
    }
    if (toks.len == 0) {
        return fail(PB_EVAL_ARITY);
    }

    pb_slice *parts = pb_arena_alloc(ctx->arena, toks.len * sizeof parts[0], _Alignof(pb_slice));
    if (parts == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t total = toks.len - 1;
    for (size_t i = 0; i < toks.len; i += 1) {
        if (!as_string(toks.items[i], &parts[i]) || parts[i].len == 0) {
            return fail(PB_EVAL_TYPE);
        }
        total += parts[i].len;
    }

    char *out = pb_arena_alloc(ctx->arena, total, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t off = 0;
    for (size_t i = 0; i < toks.len; i += 1) {
        if (i != 0) {
            out[off] = '.';
            off += 1;
        }
        memcpy(out + off, parts[i].ptr, parts[i].len);
        off += parts[i].len;
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}

static pb_eval_result call_publish(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    if (args.items[1].kind == PB_NIL) {
        return nil();
    }

    pb_slice subject = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &subject) || !coerce_payload(ctx, args.items[1], &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject)) {
        return fail(PB_EVAL_INVALID_SUBJECT);
    }
    if (ctx->publish == NULL || !ctx->publish(ctx->publish_ctx, subject, payload)) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return nil();
}
