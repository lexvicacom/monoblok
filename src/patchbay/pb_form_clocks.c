// Forms: debounce!, sample!, aggregate!. Special form: dropout.
// Clocked publish forms and tick handling for delayed/time-bucketed effects.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

#ifndef PB_FORM_JOINED
// Give editors the shared ring helper definitions when this chunk is parsed
// directly. The real build includes both chunks from pb_forms.c.
#include "pb_form_windows.c"
#endif

static pb_eval_result clock_publish(pb_eval_ctx *ctx, pb_eval_state_entry *slot) {
    if (slot->bytes == NULL && slot->bytes_len != 0) {
        return fail(PB_EVAL_TYPE);
    }
    if (ctx->publish == NULL ||
        !ctx->publish(ctx->publish_ctx,
                      (pb_slice){.ptr = slot->emit_subject, .len = slot->emit_subject_len},
                      (pb_slice){.ptr = slot->bytes, .len = slot->bytes_len})) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return nil();
}

static pb_eval_result retain_publish(pb_eval_ctx *ctx, const char *op, pb_eval_clock_kind kind, uint64_t ms,
                                     pb_value subject_v, pb_value value_v, bool reset_deadline) {
    pb_slice subject = {0};
    pb_slice payload = {0};
    if (!as_string(subject_v, &subject) || !coerce_payload(ctx, value_v, &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = state_slot(ctx, op);
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    slot->clock_kind = kind;
    slot->clock_interval_ms = ms;
    slot->clock_armed = true;
    if (reset_deadline || slot->clock_deadline_ms == 0) {
        slot->clock_deadline_ms = ctx->now_ms + ms;
    }
    if (!state_set_emit_subject(slot, subject) || !state_set_bytes(slot, payload)) {
        return fail(PB_EVAL_OOM);
    }
    slot->kind = PB_EVAL_STATE_CLOCK;
    return nil();
}

static pb_eval_result call_debounce(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 4 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms")) {
        return fail(PB_EVAL_ARITY);
    }
    double ms = 0;
    if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
        return fail(PB_EVAL_TYPE);
    }
    return retain_publish(ctx, "debounce", PB_EVAL_CLOCK_DEBOUNCE, (uint64_t)ms, args.items[2], args.items[3], true);
}

static pb_eval_result call_sample(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 4 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms")) {
        return fail(PB_EVAL_ARITY);
    }
    double ms = 0;
    if (!as_number(args.items[1], &ms) || ms < 1 || floor(ms) != ms) {
        return fail(PB_EVAL_TYPE);
    }
    return retain_publish(ctx, "sample", PB_EVAL_CLOCK_SAMPLE, (uint64_t)ms, args.items[2], args.items[3], false);
}

static pb_eval_result call_aggregate(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 5 || args.items[0].kind != PB_KEYWORD || !text_eq(args.items[0].text, "ms") ||
        args.items[3].kind != PB_KEYWORD) {
        return fail(PB_EVAL_ARITY);
    }
    double ms_d = 0;
    double x = 0;
    if (!as_number(args.items[1], &ms_d) || ms_d < 1 || floor(ms_d) != ms_d || !as_number(args.items[4], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_metric_kind metric = PB_EVAL_METRIC_NONE;
    if (text_eq(args.items[3].text, "avg")) metric = PB_EVAL_METRIC_AVG;
    if (text_eq(args.items[3].text, "sum")) metric = PB_EVAL_METRIC_SUM;
    if (text_eq(args.items[3].text, "min")) metric = PB_EVAL_METRIC_MIN;
    if (text_eq(args.items[3].text, "max")) metric = PB_EVAL_METRIC_MAX;
    if (text_eq(args.items[3].text, "count")) metric = PB_EVAL_METRIC_COUNT;
    if (text_eq(args.items[3].text, "rate")) metric = PB_EVAL_METRIC_RATE;
    if (metric == PB_EVAL_METRIC_NONE) {
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }
    pb_slice subject = {0};
    if (!as_string(args.items[2], &subject)) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = state_slot(ctx, "aggregate");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->clock_kind = PB_EVAL_CLOCK_AGGREGATE;
        slot->clock_interval_ms = (uint64_t)ms_d;
        slot->clock_deadline_ms = ctx->now_ms + (uint64_t)ms_d;
        slot->clock_armed = true;
    }
    slot->metric_kind = metric;
    if (!state_set_emit_subject(slot, subject)) {
        return fail(PB_EVAL_OOM);
    }
    if (!ring_push_time(slot, x, ctx->now_ms)) {
        return fail(PB_EVAL_OOM);
    }
    slot->kind = PB_EVAL_STATE_CLOCK;
    return nil();
}

pb_eval_result pb_eval_call_dropout(pb_eval_ctx *ctx, pb_values raw_args) {
    if (raw_args.len != 6 || raw_args.items[0].kind != PB_KEYWORD || !text_eq(raw_args.items[0].text, "ms") ||
        raw_args.items[2].kind != PB_KEYWORD || !text_eq(raw_args.items[2].text, "lost") ||
        raw_args.items[4].kind != PB_KEYWORD || !text_eq(raw_args.items[4].text, "found")) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result ms_v = pb_eval(ctx, raw_args.items[1]);
    double ms = 0;
    if (ms_v.err != PB_EVAL_OK || !as_number(ms_v.value, &ms) || ms < 1 || floor(ms) != ms) {
        return fail(PB_EVAL_TYPE);
    }
    pb_eval_state_entry *slot = state_slot(ctx, "dropout");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_CLOCK;
        slot->clock_kind = PB_EVAL_CLOCK_DROPOUT;
        slot->clock_interval_ms = (uint64_t)ms;
        slot->lost_form = raw_args.items[3];
        slot->found_form = raw_args.items[5];
    } else if (slot->kind != PB_EVAL_STATE_CLOCK || slot->clock_kind != PB_EVAL_CLOCK_DROPOUT) {
        return fail(PB_EVAL_TYPE);
    }
    slot->clock_armed = true;
    slot->clock_deadline_ms = ctx->now_ms + (uint64_t)ms;
    if (slot->dropout_lost) {
        slot->dropout_lost = false;
        return pb_eval(ctx, slot->found_form);
    }
    return nil();
}

static pb_eval_result call_clock_form(pb_eval_ctx *ctx, pb_form form, pb_values args) {
    switch (form) {
    case PB_FORM_DEBOUNCE: return call_debounce(ctx, args);
    case PB_FORM_SAMPLE: return call_sample(ctx, args);
    case PB_FORM_AGGREGATE: return call_aggregate(ctx, args);
    default: return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }
}

static pb_eval_result tick_clock_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry) {
    if (!entry->clock_armed || ctx->now_ms < entry->clock_deadline_ms) {
        return nil();
    }
    switch (entry->clock_kind) {
    case PB_EVAL_CLOCK_DROPOUT:
        entry->dropout_lost = true;
        entry->clock_armed = false;
        return pb_eval(ctx, entry->lost_form);
    case PB_EVAL_CLOCK_DEBOUNCE:
        entry->clock_armed = false;
        return clock_publish(ctx, entry);
    case PB_EVAL_CLOCK_SAMPLE: {
        entry->clock_deadline_ms += entry->clock_interval_ms;
        return clock_publish(ctx, entry);
    }
    case PB_EVAL_CLOCK_AGGREGATE: {
        if (entry->ring_len == 0) {
            entry->clock_deadline_ms += entry->clock_interval_ms;
            return nil();
        }
        pb_eval_result r = nil();
        switch (entry->metric_kind) {
        case PB_EVAL_METRIC_AVG: r = calc_ring(entry, WINDOW_AVG); break;
        case PB_EVAL_METRIC_SUM: r = calc_ring(entry, WINDOW_SUM); break;
        case PB_EVAL_METRIC_MIN: r = calc_ring(entry, WINDOW_MIN); break;
        case PB_EVAL_METRIC_MAX: r = calc_ring(entry, WINDOW_MAX); break;
        case PB_EVAL_METRIC_COUNT:
            r = ok((pb_value){.kind = PB_NUMBER, .number = (double)entry->ring_len});
            break;
        case PB_EVAL_METRIC_RATE:
            r = ok((pb_value){.kind = PB_NUMBER, .number = (double)entry->ring_len * 1000.0 / (double)entry->clock_interval_ms});
            break;
        case PB_EVAL_METRIC_NONE:
            return fail(PB_EVAL_TYPE);
        }
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        pb_slice payload = {0};
        if (!coerce_payload(ctx, r.value, &payload) || !state_set_bytes(entry, payload)) {
            return fail(PB_EVAL_OOM);
        }
        entry->kind = PB_EVAL_STATE_CLOCK;
        entry->ring_len = 0;
        entry->ring_start = 0;
        entry->ring_sum = 0;
        entry->clock_deadline_ms += entry->clock_interval_ms;
        return clock_publish(ctx, entry);
    }
    case PB_EVAL_CLOCK_NONE:
        break;
    }
    return nil();
}
