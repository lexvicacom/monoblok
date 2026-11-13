#ifndef PB_EVAL_H
#define PB_EVAL_H

#include "pb_sexpr.h"

#include <stdint.h>

#ifndef PB_ENABLE_JSON
#define PB_ENABLE_JSON 1
#endif

typedef enum pb_eval_error {
    PB_EVAL_OK,
    PB_EVAL_OOM,
    PB_EVAL_UNKNOWN_SYMBOL,
    PB_EVAL_TYPE,
    PB_EVAL_ARITY,
    PB_EVAL_INVALID_SUBJECT,
    PB_EVAL_PUBLISH_FAILED,
    PB_EVAL_REGEX_BUDGET, // a regex form hit its work budget; the rule fails closed
} pb_eval_error;

typedef struct pb_eval_ctx pb_eval_ctx;
typedef struct pb_eval_result pb_eval_result;
typedef bool (*pb_publish_fn)(void *ctx, pb_slice subject, pb_slice payload);
// Last-value lookup for `(last SUBJECT)`. On a hit sets *payload to a view that
// is only valid until the next publish; the caller copies it. False means none.
typedef bool (*pb_last_fn)(void *ctx, pb_slice subject, pb_slice *payload);
typedef bool (*pb_eval_symbol_fn)(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_eval_result *out);
typedef bool (*pb_eval_call_fn)(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_values args, pb_eval_result *out);

typedef enum pb_eval_state_kind {
    PB_EVAL_STATE_EMPTY,
    PB_EVAL_STATE_NUMBER,
    PB_EVAL_STATE_BYTES,
    PB_EVAL_STATE_RING,
    PB_EVAL_STATE_BAR,
    PB_EVAL_STATE_CLOCK,
} pb_eval_state_kind;

typedef enum pb_eval_clock_kind {
    PB_EVAL_CLOCK_NONE,
    PB_EVAL_CLOCK_ON_SILENCE,
    PB_EVAL_CLOCK_DROPOUT,
    PB_EVAL_CLOCK_DEBOUNCE,
    PB_EVAL_CLOCK_SAMPLE,
    PB_EVAL_CLOCK_AGGREGATE,
    PB_EVAL_CLOCK_EVERY,
} pb_eval_clock_kind;

typedef enum pb_eval_metric_kind {
    PB_EVAL_METRIC_NONE,
    PB_EVAL_METRIC_AVG,
    PB_EVAL_METRIC_SUM,
    PB_EVAL_METRIC_MIN,
    PB_EVAL_METRIC_MAX,
    PB_EVAL_METRIC_COUNT,
    PB_EVAL_METRIC_RATE,
} pb_eval_metric_kind;

// Long-lived per-(rule, call site, op, subject) slot for stateful forms. The
// subject is the publish subject that created the slot (empty under every
// rules); owned heap bytes, also bound as `subject` when a clock body fires.
typedef struct pb_eval_state_entry {
    size_t rule_id;
    uint32_t site;
    char *op;
    size_t op_len;
    char *subject;
    size_t subject_len;
    pb_eval_state_kind kind;
    double number;
    char *bytes;
    size_t bytes_len;
    size_t bytes_cap;
    double *ring_values;
    uint64_t *ring_times_ms;
    size_t ring_cap;
    size_t ring_len;
    size_t ring_start;
    double ring_sum;
    uint64_t ring_window_ms;
    bool ring_time_window;
    double bar_open;
    double bar_high;
    double bar_low;
    double bar_last_close;
    uint32_t bar_count;
    uint32_t bar_cap;
    uint64_t bar_window_ms;
    uint64_t bar_window_start_ms;
    bool bar_time_window;
    pb_eval_clock_kind clock_kind;
    pb_eval_metric_kind metric_kind;
    uint64_t clock_interval_ms;
    uint64_t clock_deadline_ms;
    bool clock_armed;
    bool dropout_lost;
    pb_value lost_form;
    pb_value found_form;
    pb_values clock_body;
    char *emit_subject;
    size_t emit_subject_len;
    size_t emit_subject_cap;
} pb_eval_state_entry;

// Hash index entry for long-lived evaluator state slots.
typedef struct pb_eval_state_index_entry {
    uint64_t hash;
    size_t index;
    bool occupied;
} pb_eval_state_index_entry;

// Stateful patchbay storage owned by the caller and reused across publishes.
typedef struct pb_eval_state {
    pb_eval_state_entry *items;
    pb_eval_state_index_entry *index;
    size_t len;
    size_t cap;
    size_t index_cap;
} pb_eval_state;

// Per-evaluation bindings and effect hooks; scratch allocations use arena.
struct pb_eval_ctx {
    pb_arena *arena;
    pb_eval_state *state;
    size_t rule_id;
    uint32_t site; // call-site id of the form being run; set by eval, 0 outside a call
    uint64_t now_ms;
    int64_t wall_ms;
    bool replaying;
    pb_slice subject;
    pb_slice payload;
    pb_slice reply_to; // reply subject of the triggering publish; empty means none
    pb_publish_fn publish;
    void *publish_ctx;
    pb_last_fn last; // optional `(last ...)` lookup; NULL makes `last` return nil
    void *last_ctx;
    uint64_t *publishes_emitted;
    uint64_t *publishes_suppressed;
    pb_eval_symbol_fn user_symbol;
    pb_eval_call_fn user_call;
    void *user_ctx;
};

// Evaluator return value; value is meaningful only when err is PB_EVAL_OK.
struct pb_eval_result {
    pb_eval_error err;
    pb_value value;
};

// Resolve pass run once per rule body on the AST (any source format): numbers
// call sites, tags form heads, rejects unknown forms/symbols (unless the
// matching user hook is installed) and `->` steps that cannot be threaded.
// On failure writes a short message into err and returns false.
bool pb_resolve_rule(pb_arena *arena, pb_value *body, bool allow_user_call, bool allow_user_symbol,
                     char *err, size_t err_cap);
pb_eval_result pb_eval(pb_eval_ctx *ctx, pb_value expr);
// Creates the clock slot of an (every ...) rule in `state`. `body` must be a
// resolved value that outlives the state (parse arena); the first tick
// schedules the slot at now + ms without firing it.
bool pb_eval_every_arm(pb_eval_state *state, size_t rule_id, uint64_t ms, pb_value *body);
pb_eval_result pb_eval_tick_state_entry(pb_eval_ctx *ctx, pb_eval_state_entry *entry);
void pb_eval_state_free(pb_eval_state *state);
const char *pb_eval_error_name(pb_eval_error err);

#endif
