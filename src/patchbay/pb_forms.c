#define _POSIX_C_SOURCE 200809L

#include "pb_form_chunk.h"

#define PB_FORM_JOINED 1

// Form chunks share one translation unit so helpers stay file-local without
// growing pb_eval_internal.h for organizational splits.
//
// To add a form: add the enum in pb_eval_internal.h, add its public name in
// pb_eval.c, put the implementation in the closest chunk below, then wire the
// dispatch switch in this file.
//
// Special forms with raw argument evaluation live in pb_eval.c:
// do, if, when, and, or, ->, transition, dropout.
#include "pb_form_numbers.c"
#include "pb_form_text.c"
#include "pb_form_state.c"
#include "pb_form_bar.c"
#if PB_ENABLE_JSON
#include "pb_form_json.c"
#endif
#include "pb_form_windows.c"
#include "pb_form_clocks.c"

pb_eval_result pb_eval_call_form(pb_eval_ctx *ctx, pb_form form, pb_values args) {
    switch (form) {
    // Numeric, boolean, comparison, arithmetic, and wall-clock forms.
    case PB_FORM_NOW: return call_now(ctx, args);
    case PB_FORM_NOT: return call_not(args);
    case PB_FORM_EQ: return call_eq(args);
    case PB_FORM_GT: return call_cmp(args, CMP_GT);
    case PB_FORM_LT: return call_cmp(args, CMP_LT);
    case PB_FORM_GE: return call_cmp(args, CMP_GE);
    case PB_FORM_LE: return call_cmp(args, CMP_LE);
    case PB_FORM_ADD: return call_arith(args, ARITH_ADD);
    case PB_FORM_SUB: return call_arith(args, ARITH_SUB);
    case PB_FORM_MUL: return call_arith(args, ARITH_MUL);
    case PB_FORM_DIV: return call_arith(args, ARITH_DIV);
    case PB_FORM_ROUND: return call_round(args);
    case PB_FORM_QUANTIZE: return call_quantize(args);
    case PB_FORM_CLAMP: return call_clamp(args);
    case PB_FORM_MIN: return call_minmax(args, MINMAX_MIN);
    case PB_FORM_MAX: return call_minmax(args, MINMAX_MAX);
    case PB_FORM_ABS: return call_abs(args);
    case PB_FORM_SIGN: return call_sign(args);

    // Text, subject, and publish forms.
    case PB_FORM_STR_CONCAT: return call_str_concat(ctx, args);
    case PB_FORM_CONTAINS: return call_contains(args);
    case PB_FORM_STARTS_WITH: return call_affix(args, AFFIX_STARTS);
    case PB_FORM_ENDS_WITH: return call_affix(args, AFFIX_ENDS);
    case PB_FORM_SUBJECT_APPEND: return call_subject_append(ctx, args);
    case PB_FORM_SUBJECT_TOKEN: return call_subject_token(ctx, args);
    case PB_FORM_SUBJECT_WITH: return call_subject_with(ctx, args);
    case PB_FORM_PRINT: return call_print(ctx, args);
    case PB_FORM_PUBLISH: return call_publish(ctx, args);

#if PB_ENABLE_JSON
    // JSON forms.
    case PB_FORM_JSON_GET: return call_json_get(ctx, args);
    case PB_FORM_JSON_DEMUX: return call_json_demux(ctx, args);
#else
    case PB_FORM_JSON_GET:
    case PB_FORM_JSON_DEMUX:
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
#endif

    // Per-rule state forms.
    case PB_FORM_SQUELCH: return call_squelch(ctx, args);
    case PB_FORM_DEADBAND: return call_deadband(ctx, args);
    case PB_FORM_CHANGED: return call_changed(ctx, args);
    case PB_FORM_HOLD_OFF: return call_hold_off(ctx, args);
    case PB_FORM_RISING_EDGE: return call_edge(ctx, args, true);
    case PB_FORM_FALLING_EDGE: return call_edge(ctx, args, false);
    case PB_FORM_DELTA: return call_delta(ctx, args);
    case PB_FORM_COUNT: return call_count(ctx, args);

    // Ring-window forms.
    case PB_FORM_MOVING_AVG:
    case PB_FORM_MOVING_SUM:
    case PB_FORM_MOVING_MAX:
    case PB_FORM_MOVING_MIN:
    case PB_FORM_MEDIAN:
    case PB_FORM_PERCENTILE:
    case PB_FORM_STDDEV:
    case PB_FORM_VARIANCE:
    case PB_FORM_RATE:
    case PB_FORM_THROTTLE:
        return call_window_form(ctx, form, args);

    // Clocked publish forms.
    case PB_FORM_DEBOUNCE:
    case PB_FORM_SAMPLE:
    case PB_FORM_AGGREGATE:
        return call_clock_form(ctx, form, args);

    // Bar/OHLC forms.
    case PB_FORM_BAR: return call_bar(ctx, args);

    // Special forms are handled before eager argument evaluation in pb_eval.c.
    case PB_FORM_DO:
    case PB_FORM_IF:
    case PB_FORM_WHEN:
    case PB_FORM_AND:
    case PB_FORM_OR:
    case PB_FORM_THREAD:
    case PB_FORM_TRANSITION:
    case PB_FORM_ON_SILENCE:
    case PB_FORM_DROPOUT:
        break;
    }
    return fail(PB_EVAL_UNKNOWN_SYMBOL);
}

pb_eval_result pb_eval_tick_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry) {
    if (entry->kind == PB_EVAL_STATE_CLOCK) {
        return tick_clock_state_entry(ctx, entry);
    }
    if (entry->kind == PB_EVAL_STATE_RING && entry->ring_time_window && entry->ring_len > 0) {
        const uint64_t cutoff = ctx->now_ms > entry->ring_window_ms ? ctx->now_ms - entry->ring_window_ms : 0;
        ring_evict_time(entry, cutoff);
    }
    bar_close close = {0};
    if (bar_time_tick(entry, ctx->now_ms, &close)) {
        return emit_bar_fields(ctx, close);
    }
    return nil();
}
