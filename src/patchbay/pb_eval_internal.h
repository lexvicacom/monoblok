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
    PB_FORM_REPLY,
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
    PB_FORM_HYSTERESIS,
    PB_FORM_EWMA,
    PB_FORM_LAST,
} pb_form;

// One row of the form table (FORMS[] in pb_eval.c), the single description of a
// form. Adding a form: enum id above, one row, one case in pb_eval_call_form
// (or pb_eval.c for special forms).
typedef struct pb_form_entry {
    const char *name;
    pb_form form;
    bool special;    // evaluates raw (unevaluated) arguments itself
    bool stateful;   // allocates a state slot keyed by call site
    bool clocked;    // always needs the clock timer
    bool clocked_ms; // needs the clock timer only when called with a leading :ms window
} pb_form_entry;

const pb_form_entry *pb_form_lookup(pb_slice name);
// Form at call-site id `site` inside a resolved rule body, or NULL.
const pb_form_entry *pb_form_at_site(pb_value body, uint32_t site);
// True when state op name `op` (e.g. "moving-avg/t", "count") belongs to `entry`'s form.
bool pb_form_owns_op(const pb_form_entry *entry, pb_slice op);

pb_eval_result pb_eval_ok(pb_value v);
pb_eval_result pb_eval_fail(pb_eval_error err);
bool pb_eval_text_eq(pb_slice s, const char *lit);
bool pb_eval_truthy(pb_value v);
pb_value pb_eval_bool_value(bool b);
bool pb_eval_as_number(pb_value v, double *out);
// Bound-symbol table shared with the YAML and JSON adapters: tag for NAME (no sigil), 0 if unknown.
uint32_t pb_binding_lookup(pb_slice name);
// Name of the INDEX-th binding, or NULL past the end; for error messages.
const char *pb_binding_name(size_t index);
bool pb_eval_as_string(pb_value v, pb_slice *out);
bool pb_eval_coerce_payload(pb_eval_ctx *ctx, pb_value v, pb_slice *out);
bool pb_eval_value_eq(pb_value a, pb_value b);
pb_eval_state_entry *pb_eval_state_slot(pb_eval_ctx *ctx, const char *op_lit);
bool pb_eval_state_set_bytes(pb_eval_state_entry *e, pb_slice bytes);
bool pb_eval_state_set_emit_subject(pb_eval_state_entry *e, pb_slice bytes);
void pb_eval_note_suppressed(pb_eval_ctx *ctx);
bool pb_eval_state_deadline(const pb_eval_state_entry *entry, uint64_t *out_ms);
pb_eval_result pb_eval_call_form(pb_eval_ctx *ctx, pb_form form, pb_values args);
pb_eval_result pb_eval_call_on_silence(pb_eval_ctx *ctx, pb_values raw_args);
pb_eval_result pb_eval_call_dropout(pb_eval_ctx *ctx, pb_values raw_args);

static inline pb_eval_result pb_eval_nil(void) {
    return pb_eval_ok((pb_value){.kind = PB_NIL});
}

#endif
