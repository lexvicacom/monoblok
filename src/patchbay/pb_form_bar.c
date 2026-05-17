// Forms: bar!, bar.
// Bar/OHLC forms and time-based bar ticking.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

typedef struct bar_close {
    double open;
    double high;
    double low;
    double close;
} bar_close;

static bool emit_one_bar_field(pb_eval_ctx *ctx, const char *name, double value) {
    const size_t name_len = strlen(name);
    const size_t subject_len = ctx->subject.len + 5 + name_len;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return false;
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    memcpy(subject_ptr + ctx->subject.len, ".bar.", 5);
    memcpy(subject_ptr + ctx->subject.len + 5, name, name_len);

    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%.17g", value);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return false;
    }
    char *payload_ptr = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (payload_ptr == NULL) {
        return false;
    }
    return ctx->publish != NULL &&
           ctx->publish(ctx->publish_ctx,
                        (pb_slice){.ptr = subject_ptr, .len = subject_len},
                        (pb_slice){.ptr = payload_ptr, .len = (size_t)n});
}

static pb_eval_result emit_bar_fields(pb_eval_ctx *ctx, bar_close c) {
    if (!emit_one_bar_field(ctx, "open", c.open) ||
        !emit_one_bar_field(ctx, "high", c.high) ||
        !emit_one_bar_field(ctx, "low", c.low) ||
        !emit_one_bar_field(ctx, "close", c.close)) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return nil();
}

static bool bar_tick_update(pb_eval_state_entry *slot, double x, bar_close *out) {
    if (slot->bar_count == 0) {
        slot->bar_open = x;
        slot->bar_high = x;
        slot->bar_low = x;
        slot->bar_count = 1;
    } else {
        if (x > slot->bar_high) slot->bar_high = x;
        if (x < slot->bar_low) slot->bar_low = x;
        slot->bar_count += 1;
    }
    if (slot->bar_count < slot->bar_cap) {
        return false;
    }
    *out = (bar_close){.open = slot->bar_open, .high = slot->bar_high, .low = slot->bar_low, .close = x};
    slot->bar_count = 0;
    return true;
}

static bool bar_time_update(pb_eval_state_entry *slot, uint64_t now, double x, bar_close *out) {
    const uint64_t aligned = (now / slot->bar_window_ms) * slot->bar_window_ms;
    if (slot->bar_count == 0) {
        slot->bar_open = x;
        slot->bar_high = x;
        slot->bar_low = x;
        slot->bar_count = 1;
        slot->bar_window_start_ms = aligned;
        slot->bar_last_close = x;
        return false;
    }
    if (slot->bar_window_start_ms != aligned) {
        *out = (bar_close){
            .open = slot->bar_open,
            .high = slot->bar_high,
            .low = slot->bar_low,
            .close = slot->bar_last_close,
        };
        slot->bar_open = x;
        slot->bar_high = x;
        slot->bar_low = x;
        slot->bar_count = 1;
        slot->bar_window_start_ms = aligned;
        slot->bar_last_close = x;
        return true;
    }
    if (x > slot->bar_high) slot->bar_high = x;
    if (x < slot->bar_low) slot->bar_low = x;
    slot->bar_count += 1;
    slot->bar_last_close = x;
    return false;
}

static bool bar_time_tick(pb_eval_state_entry *slot, uint64_t now, bar_close *out) {
    if (slot->kind != PB_EVAL_STATE_BAR || !slot->bar_time_window || slot->bar_count == 0) {
        return false;
    }
    if (now - slot->bar_window_start_ms < slot->bar_window_ms) {
        return false;
    }
    *out = (bar_close){
        .open = slot->bar_open,
        .high = slot->bar_high,
        .low = slot->bar_low,
        .close = slot->bar_last_close,
    };
    slot->bar_count = 0;
    slot->bar_window_start_ms = 0;
    return true;
}

static pb_eval_result call_bar(pb_eval_ctx *ctx, pb_values args) {
    bool time_window = false;
    uint32_t count = 0;
    uint64_t window_ms = 0;
    pb_value input = {0};

    if (args.len == 2) {
        double n = 0;
        if (!as_number(args.items[0], &n) || n < 1 || floor(n) != n || n > UINT32_MAX) {
            return fail(PB_EVAL_TYPE);
        }
        count = (uint32_t)n;
        input = args.items[1];
    } else if (args.len == 3 &&
               args.items[0].kind == PB_KEYWORD &&
               text_eq(args.items[0].text, "ms")) {
        double ms = 0;
        if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
            return fail(PB_EVAL_TYPE);
        }
        time_window = true;
        window_ms = (uint64_t)ms;
        input = args.items[2];
    } else {
        return fail(PB_EVAL_ARITY);
    }

    double x = 0;
    if (!as_number(input, &x)) {
        return fail(PB_EVAL_TYPE);
    }

    pb_eval_state_entry *slot = state_slot(ctx, time_window ? "bar/m" : "bar/t");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_BAR;
        slot->bar_time_window = time_window;
        slot->bar_cap = count;
        slot->bar_window_ms = window_ms;
    }
    if (slot->kind != PB_EVAL_STATE_BAR || slot->bar_time_window != time_window) {
        return fail(PB_EVAL_TYPE);
    }

    bar_close close = {0};
    const bool closed = time_window ? bar_time_update(slot, ctx->now_ms, x, &close) : bar_tick_update(slot, x, &close);
    if (!closed) {
        note_suppressed(ctx);
        return nil();
    }
    return emit_bar_fields(ctx, close);
}
