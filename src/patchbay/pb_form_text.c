// Forms: str-concat, contains?, starts-with?, ends-with?, subject-append,
// subject-token, subject-with, print!, publish!, publish, publish-to!, publish-to.
// Text, subject, and publish forms.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

#include "router.h"

static bool keyword_eq(pb_value v, const char *lit) {
    return v.kind == PB_KEYWORD && text_eq(v.text, lit);
}

static bool publish_subject_valid(pb_slice subject) {
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    return mb_proto_subject_valid(mb_subject, false) &&
           !mb_router_subject_has_lvc_prefix(mb_subject) &&
           !mb_router_subject_has_stats_prefix(mb_subject);
}

static bool parse_decimal_places(pb_value v, int *out) {
    double places = 0;
    if (!as_number(v, &places) ||
        places < 0 ||
        places > 15 ||
        floor(places) != places) {
        return false;
    }
    *out = (int)places;
    return true;
}

static bool format_number_dp(pb_eval_ctx *ctx, double number, int places, pb_slice *out) {
    const int n = snprintf(NULL, 0, "%.*f", places, number);
    if (n < 0 || (size_t)n == SIZE_MAX) {
        return false;
    }
    char *owned = pb_arena_alloc(ctx->arena, (size_t)n + 1, 1);
    if (owned == NULL) {
        return false;
    }
    if (snprintf(owned, (size_t)n + 1, "%.*f", places, number) != n) {
        return false;
    }
    *out = (pb_slice){.ptr = owned, .len = (size_t)n};
    return true;
}

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
    if ((args.len != 1 && args.len != 2) ||
        args.items[0].kind != PB_NUMBER ||
        args.items[0].number < 0 ||
        floor(args.items[0].number) != args.items[0].number) {
        return fail(PB_EVAL_ARITY);
    }

    pb_slice subject = ctx->subject;
    if (args.len == 2 && !as_string(args.items[1], &subject)) {
        return fail(PB_EVAL_TYPE);
    }
    const size_t want = (size_t)args.items[0].number;
    size_t tok = 0;
    size_t start = 0;
    for (size_t i = 0; i <= subject.len; i += 1) {
        if (i == subject.len || subject.ptr[i] == '.') {
            if (tok == want) {
                return ok((pb_value){.kind = PB_STRING, .text = {.ptr = subject.ptr + start, .len = i - start}});
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
        if (!coerce_payload(ctx, toks.items[i], &parts[i]) || parts[i].len == 0) {
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
    if (!publish_subject_valid((pb_slice){.ptr = out, .len = total})) {
        return fail(PB_EVAL_INVALID_SUBJECT);
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}

static void print_slice(FILE *out, pb_slice s) {
    (void)fwrite(s.ptr, 1, s.len, out);
}

static void print_value(FILE *out, pb_value v) {
    switch (v.kind) {
    case PB_NIL:
        fputs("nil", out);
        break;
    case PB_BOOL:
        fputs(v.boolean ? "true" : "false", out);
        break;
    case PB_NUMBER:
        fprintf(out, "%.17g", v.number);
        break;
    case PB_STRING:
    case PB_SYMBOL:
        print_slice(out, v.text);
        break;
    case PB_KEYWORD:
        fputc(':', out);
        print_slice(out, v.text);
        break;
    case PB_VECTOR:
    case PB_LIST: {
        const char open = v.kind == PB_VECTOR ? '[' : '(';
        const char close = v.kind == PB_VECTOR ? ']' : ')';
        fputc(open, out);
        for (size_t i = 0; i < v.seq.len; i += 1) {
            if (i != 0) {
                fputc(' ', out);
            }
            print_value(out, v.seq.items[i]);
        }
        fputc(close, out);
        break;
    }
    }
}

static pb_eval_result call_print(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1 && args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    const pb_value value = args.items[args.len - 1];
    pb_slice label = {0};
    if (args.len == 2 && !as_string(args.items[0], &label)) {
        return fail(PB_EVAL_TYPE);
    }
    fprintf(stderr, "print! [");
    print_slice(stderr, ctx->subject);
    fputs("] ", stderr);
    if (args.len == 2) {
        print_slice(stderr, label);
        fputs(" = ", stderr);
    }
    print_value(stderr, value);
    fputc('\n', stderr);
    return ok(value);
}

static pb_eval_result call_publish(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2 && args.len != 4) {
        return fail(PB_EVAL_ARITY);
    }

    size_t value_idx = 1;
    int places = -1;
    if (args.len == 4) {
        if (keyword_eq(args.items[1], "dp")) {
            value_idx = 3;
            if (!parse_decimal_places(args.items[2], &places)) {
                return fail(PB_EVAL_TYPE);
            }
        } else if (keyword_eq(args.items[2], "dp")) {
            value_idx = 1;
            if (!parse_decimal_places(args.items[3], &places)) {
                return fail(PB_EVAL_TYPE);
            }
        } else {
            return fail(PB_EVAL_ARITY);
        }
    }

    if (args.items[value_idx].kind == PB_NIL) {
        return nil();
    }

    pb_slice subject = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &subject)) {
        return fail(PB_EVAL_TYPE);
    }
    if (places >= 0) {
        if (args.items[value_idx].kind != PB_NUMBER ||
            !format_number_dp(ctx, args.items[value_idx].number, places, &payload)) {
            return fail(PB_EVAL_TYPE);
        }
    } else if (!coerce_payload(ctx, args.items[value_idx], &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    if (!publish_subject_valid(subject)) {
        return fail(PB_EVAL_INVALID_SUBJECT);
    }
    if (ctx->publish == NULL || !ctx->publish(ctx->publish_ctx, subject, payload)) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return nil();
}
