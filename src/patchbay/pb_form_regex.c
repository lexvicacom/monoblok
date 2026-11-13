// Forms: matches?, re-capture, re-replace.
//
// Experimental. The pattern is a PB_REGEX value compiled once at load by
// pb_resolve_rule; nothing compiles at publish time. Each call gets one fresh
// PB_REGEX_DEFAULT_BUDGET; running out fails the rule (PB_EVAL_REGEX_BUDGET) so
// a scanning rule fails closed. TEXT coercion matches contains?.

#include "pb_form_chunk.h"

static bool regex_args(pb_values args, size_t want, pb_regex **re, pb_slice *text, pb_eval_error *err) {
    if (args.len != want) {
        *err = PB_EVAL_ARITY;
        return false;
    }
    if (args.items[0].kind != PB_REGEX || !as_string(args.items[want - 1], text)) {
        *err = PB_EVAL_TYPE;
        return false;
    }
    *re = args.items[0].regex;
    return true;
}

// (matches? PATTERN TEXT): true when PATTERN matches anywhere in TEXT.
static pb_eval_result call_matches(pb_values args) {
    pb_regex *re = NULL;
    pb_slice text = {0};
    pb_eval_error err = PB_EVAL_OK;
    if (!regex_args(args, 2, &re, &text, &err)) {
        return fail(err);
    }
    size_t budget = PB_REGEX_DEFAULT_BUDGET;
    const pb_regex_result found = pb_regex_test(re, text, &budget);
    if (found == PB_REGEX_GAVE_UP) {
        return fail(PB_EVAL_REGEX_BUDGET);
    }
    return ok(bool_value(found == PB_REGEX_MATCH));
}

// (re-capture PATTERN TEXT): group 1 (whole match if no groups) of the leftmost
// match, borrowed from TEXT; nil on no match or a non-participating group.
static pb_eval_result call_re_capture(pb_values args) {
    pb_regex *re = NULL;
    pb_slice text = {0};
    pb_eval_error err = PB_EVAL_OK;
    if (!regex_args(args, 2, &re, &text, &err)) {
        return fail(err);
    }
    pb_regex_span spans[2] = {0};
    size_t budget = PB_REGEX_DEFAULT_BUDGET;
    const pb_regex_result found = pb_regex_search(re, text, 0, spans, 2, &budget);
    if (found == PB_REGEX_GAVE_UP) {
        return fail(PB_EVAL_REGEX_BUDGET);
    }
    if (found != PB_REGEX_MATCH) {
        return nil();
    }
    const pb_regex_span hit = pb_regex_group_count(re) > 0 ? spans[1] : spans[0];
    if (!hit.matched) {
        return nil();
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = text.ptr + hit.start, .len = hit.end - hit.start}});
}

// (re-replace PATTERN REPLACEMENT TEXT): replaces every non-overlapping match
// with the literal REPLACEMENT. Empty matches follow Go (b* on "abc" with "-"
// gives "-a-c-"). Spans are collected first, then output is allocated once.
static pb_eval_result call_re_replace(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice replacement = {0};
    pb_slice text = {0};
    if (args.items[0].kind != PB_REGEX || !as_string(args.items[1], &replacement) || !as_string(args.items[2], &text)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_regex *re = args.items[0].regex;

    pb_regex_span *hits = NULL;
    size_t hit_len = 0;
    size_t hit_cap = 0;
    size_t from = 0;
    size_t budget = PB_REGEX_DEFAULT_BUDGET;
    while (from <= text.len) {
        pb_regex_span span = {0};
        const pb_regex_result found = pb_regex_search(re, text, from, &span, 1, &budget);
        if (found == PB_REGEX_GAVE_UP) {
            return fail(PB_EVAL_REGEX_BUDGET);
        }
        if (found != PB_REGEX_MATCH) {
            break;
        }
        // Go convention: drop an empty match touching the previous match.
        if (span.end == span.start && hit_len != 0 && span.start == hits[hit_len - 1].end) {
            from = span.end + 1;
            continue;
        }
        if (hit_len == hit_cap) {
            if (hit_cap > SIZE_MAX / 2 / sizeof hits[0]) {
                return fail(PB_EVAL_OOM);
            }
            const size_t new_cap = hit_cap == 0 ? 8 : hit_cap * 2;
            pb_regex_span *grown = pb_arena_alloc(ctx->arena, new_cap * sizeof hits[0], alignof(pb_regex_span));
            if (grown == NULL) {
                return fail(PB_EVAL_OOM);
            }
            if (hit_len != 0) {
                memcpy(grown, hits, hit_len * sizeof hits[0]);
            }
            hits = grown;
            hit_cap = new_cap;
        }
        hits[hit_len] = span;
        hit_len += 1;
        from = span.end > span.start ? span.end : span.end + 1;
    }
    if (hit_len == 0) {
        return ok((pb_value){.kind = PB_STRING, .text = text});
    }

    size_t matched_bytes = 0;
    for (size_t i = 0; i < hit_len; i += 1) {
        matched_bytes += hits[i].end - hits[i].start;
    }
    if (replacement.len != 0 && hit_len > SIZE_MAX / replacement.len) {
        return fail(PB_EVAL_OOM);
    }
    const size_t added = hit_len * replacement.len;
    const size_t kept = text.len - matched_bytes;
    if (added > SIZE_MAX - kept) {
        return fail(PB_EVAL_OOM);
    }
    const size_t total = kept + added;
    char *out = pb_arena_alloc(ctx->arena, total == 0 ? 1 : total, 1);
    if (out == NULL) {
        return fail(PB_EVAL_OOM);
    }
    size_t off = 0;
    size_t pos = 0;
    for (size_t i = 0; i < hit_len; i += 1) {
        const size_t gap = hits[i].start - pos;
        memcpy(out + off, text.ptr + pos, gap);
        off += gap;
        memcpy(out + off, replacement.ptr, replacement.len);
        off += replacement.len;
        pos = hits[i].end;
    }
    memcpy(out + off, text.ptr + pos, text.len - pos);
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}
