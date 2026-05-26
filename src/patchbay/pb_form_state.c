// Forms: squelch, deadband, changed?, hold-off, rising-edge, falling-edge,
// delta, count!, count.
// Per-rule state forms that store numbers or bytes in pb_eval_state_entry.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

static bool encode_state_value(pb_eval_ctx *ctx, pb_value v, pb_slice *out) {
    if (v.kind == PB_NUMBER) {
        return coerce_payload(ctx, v, out);
    }
    if (v.kind == PB_STRING || v.kind == PB_SYMBOL || v.kind == PB_BOOL || v.kind == PB_NIL) {
        return as_string(v, out);
    }
    return false;
}

static pb_eval_result stateful_changed(pb_eval_ctx *ctx, const char *op, pb_value v, bool bool_mode) {
    pb_eval_state_entry *slot = state_slot(ctx, op);
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }

    bool changed = true;
    if (v.kind == PB_NUMBER) {
        changed = slot->kind != PB_EVAL_STATE_NUMBER || slot->number != v.number;
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = v.number;
    } else {
        pb_slice bytes = {0};
        if (!encode_state_value(ctx, v, &bytes)) {
            return fail(PB_EVAL_TYPE);
        }
        changed = slot->kind != PB_EVAL_STATE_BYTES ||
                  slot->bytes_len != bytes.len ||
                  memcmp(slot->bytes, bytes.ptr, bytes.len) != 0;
        if (changed && !state_set_bytes(slot, bytes)) {
            return fail(PB_EVAL_OOM);
        }
    }
    if (!changed) {
        note_suppressed(ctx);
    }
    return ok(bool_mode ? bool_value(changed) : (changed ? v : (pb_value){.kind = PB_NIL}));
}

static pb_eval_result call_squelch(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    return stateful_changed(ctx, "squelch", args.items[0], false);
}

static pb_eval_result call_changed(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    return stateful_changed(ctx, "changed?", args.items[0], true);
}

static pb_eval_result call_hold_off(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double window_ms = 0;
    if (!as_number(args.items[0], &window_ms) || window_ms < 0 || floor(window_ms) != window_ms) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "hold-off");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = (double)ctx->now_ms;
        return ok(args.items[1]);
    }
    if (slot->kind != PB_EVAL_STATE_NUMBER) {
        return fail(PB_EVAL_TYPE);
    }

    const double now_ms = (double)ctx->now_ms;
    if (now_ms < slot->number || now_ms - slot->number < window_ms) {
        note_suppressed(ctx);
        return nil();
    }
    slot->number = now_ms;
    return ok(args.items[1]);
}

static pb_eval_result call_edge(pb_eval_ctx *ctx, pb_values args, bool rising) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }

    const bool cur = truthy(args.items[0]);
    pb_eval_state_entry *slot = state_slot(ctx, rising ? "rising-edge" : "falling-edge");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = cur ? 1.0 : 0.0;
        note_suppressed(ctx);
        return nil();
    }
    if (slot->kind != PB_EVAL_STATE_NUMBER) {
        return fail(PB_EVAL_TYPE);
    }

    const bool prev = slot->number != 0.0;
    slot->number = cur ? 1.0 : 0.0;
    const bool fire = rising ? (!prev && cur) : (prev && !cur);
    if (!fire) {
        note_suppressed(ctx);
    }
    return ok(fire ? bool_value(true) : (pb_value){.kind = PB_NIL});
}

static pb_eval_result call_deadband(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double threshold = 0;
    double x = 0;
    if (!as_number(args.items[0], &threshold) || !as_number(args.items[1], &x) || threshold < 0) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "deadband");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    const bool pass = slot->kind != PB_EVAL_STATE_NUMBER || fabs(x - slot->number) >= threshold;
    if (!pass) {
        note_suppressed(ctx);
        return nil();
    }
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = x;
    return ok((pb_value){.kind = PB_NUMBER, .number = x});
}

static pb_eval_result call_delta(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[0], &x)) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "delta");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    const double prev = slot->kind == PB_EVAL_STATE_NUMBER ? slot->number : x;
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = x;
    return ok((pb_value){.kind = PB_NUMBER, .number = x - prev});
}

static pb_eval_result call_count(pb_eval_ctx *ctx, pb_values args) {
    if (args.len > 3) {
        return fail(PB_EVAL_ARITY);
    }

    bool has_cond = false;
    pb_value cond = {0};
    bool has_subject = false;
    pb_slice subject = {0};

    if (args.len == 1) {
        if (keyword_eq(args.items[0], "subject")) {
            return fail(PB_EVAL_ARITY);
        }
        has_cond = true;
        cond = args.items[0];
    } else if (args.len == 2) {
        if (!keyword_eq(args.items[0], "subject") || !as_string(args.items[1], &subject)) {
            return fail(PB_EVAL_TYPE);
        }
        has_subject = true;
    } else if (args.len == 3) {
        if (keyword_eq(args.items[0], "subject")) {
            if (!as_string(args.items[1], &subject)) {
                return fail(PB_EVAL_TYPE);
            }
            has_subject = true;
            has_cond = true;
            cond = args.items[2];
        } else if (keyword_eq(args.items[1], "subject")) {
            has_cond = true;
            cond = args.items[0];
            if (!as_string(args.items[2], &subject)) {
                return fail(PB_EVAL_TYPE);
            }
            has_subject = true;
        } else {
            return fail(PB_EVAL_TYPE);
        }
    }

    const bool inc = !has_cond || truthy(cond);
    if (!inc) {
        note_suppressed(ctx);
        return nil();
    }

    if (!has_subject) {
        const size_t subject_len = ctx->subject.len + 6;
        char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
        if (subject_ptr == NULL) {
            return fail(PB_EVAL_OOM);
        }
        memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
        memcpy(subject_ptr + ctx->subject.len, ".count", 6);
        subject = (pb_slice){.ptr = subject_ptr, .len = subject_len};
    }
    if (!publish_subject_valid(subject)) {
        return fail(PB_EVAL_INVALID_SUBJECT);
    }

    pb_eval_state_entry *slot = state_slot(ctx, "count");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    const double next = (slot->kind == PB_EVAL_STATE_NUMBER ? slot->number : 0) + 1;
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = next;

    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%.0f", next);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return fail(PB_EVAL_TYPE);
    }
    char *payload_ptr = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (payload_ptr == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (ctx->publish == NULL ||
        !ctx->publish(ctx->publish_ctx, subject, (pb_slice){.ptr = payload_ptr, .len = (size_t)n})) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return nil();
}
