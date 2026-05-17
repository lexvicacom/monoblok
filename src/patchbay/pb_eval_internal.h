#ifndef PB_EVAL_INTERNAL_H
#define PB_EVAL_INTERNAL_H

#include "pb_eval.h"

typedef enum pb_form {
    PB_FORM_DO,
    PB_FORM_IF,
    PB_FORM_WHEN,
    PB_FORM_AND,
    PB_FORM_OR,
    PB_FORM_THREAD,
    PB_FORM_TRANSITION,
    PB_FORM_ON_SILENCE,
    PB_FORM_DROPOUT,
    PB_FORM_NOW,
    PB_FORM_NOT,
    PB_FORM_EQ,
    PB_FORM_GT,
    PB_FORM_LT,
    PB_FORM_GE,
    PB_FORM_LE,
    PB_FORM_ADD,
    PB_FORM_SUB,
    PB_FORM_MUL,
    PB_FORM_DIV,
    PB_FORM_STR_CONCAT,
    PB_FORM_CONTAINS,
    PB_FORM_STARTS_WITH,
    PB_FORM_ENDS_WITH,
    PB_FORM_SUBJECT_APPEND,
    PB_FORM_SUBJECT_TOKEN,
    PB_FORM_SUBJECT_WITH,
    PB_FORM_PRINT,
    PB_FORM_PUBLISH,
    PB_FORM_JSON_GET,
    PB_FORM_JSON_DEMUX,
    PB_FORM_ROUND,
    PB_FORM_QUANTIZE,
    PB_FORM_CLAMP,
    PB_FORM_MIN,
    PB_FORM_MAX,
    PB_FORM_ABS,
    PB_FORM_SIGN,
    PB_FORM_SQUELCH,
    PB_FORM_DEADBAND,
    PB_FORM_CHANGED,
    PB_FORM_HOLD_OFF,
    PB_FORM_RISING_EDGE,
    PB_FORM_FALLING_EDGE,
    PB_FORM_DELTA,
    PB_FORM_COUNT,
    PB_FORM_MOVING_AVG,
    PB_FORM_MOVING_SUM,
    PB_FORM_MOVING_MAX,
    PB_FORM_MOVING_MIN,
    PB_FORM_MEDIAN,
    PB_FORM_PERCENTILE,
    PB_FORM_STDDEV,
    PB_FORM_VARIANCE,
    PB_FORM_RATE,
    PB_FORM_THROTTLE,
    PB_FORM_DEBOUNCE,
    PB_FORM_SAMPLE,
    PB_FORM_AGGREGATE,
    PB_FORM_BAR,
} pb_form;

pb_eval_result pb_eval_ok(pb_value v);
pb_eval_result pb_eval_fail(pb_eval_error err);
bool pb_eval_text_eq(pb_slice s, const char *lit);
bool pb_eval_truthy(pb_value v);
pb_value pb_eval_bool_value(bool b);
bool pb_eval_as_number(pb_value v, double *out);
bool pb_eval_as_string(pb_value v, pb_slice *out);
bool pb_eval_coerce_payload(pb_eval_ctx *ctx, pb_value v, pb_slice *out);
bool pb_eval_value_eq(pb_value a, pb_value b);
pb_eval_state_entry *pb_eval_state_slot(pb_eval_ctx *ctx, const char *op_lit);
bool pb_eval_state_set_bytes(pb_eval_state_entry *e, pb_slice bytes);
bool pb_eval_state_set_emit_subject(pb_eval_state_entry *e, pb_slice bytes);
void pb_eval_note_suppressed(pb_eval_ctx *ctx);
pb_eval_result pb_eval_call_form(pb_eval_ctx *ctx, pb_form form, pb_values args);
pb_eval_result pb_eval_call_on_silence(pb_eval_ctx *ctx, pb_values raw_args);
pb_eval_result pb_eval_call_dropout(pb_eval_ctx *ctx, pb_values raw_args);

static inline pb_eval_result pb_eval_nil(void) {
    return pb_eval_ok((pb_value){.kind = PB_NIL});
}

// Short aliases used as local sugar in evaluator form implementations.
#define ok pb_eval_ok
#define nil pb_eval_nil
#define fail pb_eval_fail
#define text_eq pb_eval_text_eq
#define truthy pb_eval_truthy
#define bool_value pb_eval_bool_value
#define as_number pb_eval_as_number
#define as_string pb_eval_as_string
#define coerce_payload pb_eval_coerce_payload
#define value_eq pb_eval_value_eq
#define state_slot pb_eval_state_slot
#define state_set_bytes pb_eval_state_set_bytes
#define state_set_emit_subject pb_eval_state_set_emit_subject
#define note_suppressed pb_eval_note_suppressed

#endif
