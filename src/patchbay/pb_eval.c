#include "pb_eval_internal.h"

#include "array.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pb_eval_result ok(pb_value v) {
    return (pb_eval_result){.err = PB_EVAL_OK, .value = v};
}

pb_eval_result fail(pb_eval_error err) {
    return (pb_eval_result){.err = err};
}

bool text_eq(pb_slice s, const char *lit) {
    return pb_slice_eq_lit(s, lit);
}

bool truthy(pb_value v) {
    return v.kind != PB_NIL && !(v.kind == PB_BOOL && !v.boolean);
}

pb_value bool_value(bool b) {
    return (pb_value){.kind = PB_BOOL, .boolean = b};
}

bool as_number(pb_value v, double *out) {
    if (v.kind == PB_NUMBER) {
        *out = v.number;
        return true;
    }
    if (v.kind != PB_STRING && v.kind != PB_SYMBOL) {
        return false;
    }
    if (v.text.len >= 128) {
        return false;
    }

    char tmp[128];
    memcpy(tmp, v.text.ptr, v.text.len);
    tmp[v.text.len] = '\0';

    errno = 0;
    char *end = NULL;
    const double n = strtod(tmp, &end);
    if (errno != 0 || end != tmp + v.text.len) {
        return false;
    }
    *out = n;
    return true;
}

bool as_string(pb_value v, pb_slice *out) {
    if (v.kind == PB_STRING || v.kind == PB_SYMBOL) {
        *out = v.text;
        return true;
    }
    if (v.kind == PB_BOOL) {
        *out = v.boolean ? (pb_slice){.ptr = "true", .len = 4} : (pb_slice){.ptr = "false", .len = 5};
        return true;
    }
    if (v.kind == PB_NIL) {
        *out = (pb_slice){.ptr = "", .len = 0};
        return true;
    }
    return false;
}

bool coerce_payload(pb_eval_ctx *ctx, pb_value v, pb_slice *out) {
    if (as_string(v, out)) {
        return true;
    }
    if (v.kind != PB_NUMBER) {
        return false;
    }

    char tmp[32];
    int n = -1;
    for (int precision = 15; precision <= 17; precision += 1) {
        n = snprintf(tmp, sizeof tmp, "%.*g", precision, v.number);
        if (n < 0 || (size_t)n >= sizeof tmp) {
            return false;
        }
        errno = 0;
        char *end = NULL;
        const double roundtrip = strtod(tmp, &end);
        if (errno == 0 && end == tmp + n && roundtrip == v.number) {
            break;
        }
    }
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return false;
    }
    char *owned = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (owned == NULL) {
        return false;
    }
    *out = (pb_slice){.ptr = owned, .len = (size_t)n};
    return true;
}

bool value_eq(pb_value a, pb_value b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case PB_NIL: return true;
    case PB_BOOL: return a.boolean == b.boolean;
    case PB_NUMBER: return a.number == b.number;
    case PB_SYMBOL:
    case PB_KEYWORD:
    case PB_STRING:
        return pb_slice_eq(a.text, b.text);
    case PB_LIST:
    case PB_VECTOR:
        return false;
    }
    return false;
}

static pb_eval_result eval_list(pb_eval_ctx *ctx, pb_values call);

typedef struct pb_form_entry {
    const char *name;
    pb_form form;
    bool special;
} pb_form_entry;

static const pb_form_entry FORMS[] = {
    // Special forms evaluate raw arguments in pb_eval.c.
    {.name = "do", .form = PB_FORM_DO, .special = true},
    {.name = "if", .form = PB_FORM_IF, .special = true},
    {.name = "when", .form = PB_FORM_WHEN, .special = true},
    {.name = "and", .form = PB_FORM_AND, .special = true},
    {.name = "or", .form = PB_FORM_OR, .special = true},
    {.name = "->", .form = PB_FORM_THREAD, .special = true},
    {.name = "transition", .form = PB_FORM_TRANSITION, .special = true},
    {.name = "dropout", .form = PB_FORM_DROPOUT, .special = true},

    // Numeric, boolean, comparison, arithmetic, and wall-clock forms.
    {.name = "now", .form = PB_FORM_NOW},
    {.name = "not", .form = PB_FORM_NOT},
    {.name = "=", .form = PB_FORM_EQ},
    {.name = ">", .form = PB_FORM_GT},
    {.name = "<", .form = PB_FORM_LT},
    {.name = ">=", .form = PB_FORM_GE},
    {.name = "<=", .form = PB_FORM_LE},
    {.name = "+", .form = PB_FORM_ADD},
    {.name = "-", .form = PB_FORM_SUB},
    {.name = "*", .form = PB_FORM_MUL},
    {.name = "/", .form = PB_FORM_DIV},
    {.name = "round", .form = PB_FORM_ROUND},
    {.name = "quantize", .form = PB_FORM_QUANTIZE},
    {.name = "clamp", .form = PB_FORM_CLAMP},
    {.name = "min", .form = PB_FORM_MIN},
    {.name = "max", .form = PB_FORM_MAX},
    {.name = "abs", .form = PB_FORM_ABS},
    {.name = "sign", .form = PB_FORM_SIGN},

    // Text, subject, and publish forms.
    {.name = "str-concat", .form = PB_FORM_STR_CONCAT},
    {.name = "contains?", .form = PB_FORM_CONTAINS},
    {.name = "starts-with?", .form = PB_FORM_STARTS_WITH},
    {.name = "ends-with?", .form = PB_FORM_ENDS_WITH},
    {.name = "subject-append", .form = PB_FORM_SUBJECT_APPEND},
    {.name = "subject-token", .form = PB_FORM_SUBJECT_TOKEN},
    {.name = "subject-with", .form = PB_FORM_SUBJECT_WITH},
    {.name = "publish!", .form = PB_FORM_PUBLISH},
    {.name = "publish", .form = PB_FORM_PUBLISH},

    // JSON forms.
    {.name = "json-get", .form = PB_FORM_JSON_GET},
    {.name = "json-demux!", .form = PB_FORM_JSON_DEMUX},

    // Per-rule state forms.
    {.name = "squelch", .form = PB_FORM_SQUELCH},
    {.name = "deadband", .form = PB_FORM_DEADBAND},
    {.name = "changed?", .form = PB_FORM_CHANGED},
    {.name = "hold-off", .form = PB_FORM_HOLD_OFF},
    {.name = "rising-edge", .form = PB_FORM_RISING_EDGE},
    {.name = "falling-edge", .form = PB_FORM_FALLING_EDGE},
    {.name = "delta", .form = PB_FORM_DELTA},
    {.name = "count!", .form = PB_FORM_COUNT},
    {.name = "count", .form = PB_FORM_COUNT},

    // Ring-window forms.
    {.name = "moving-avg", .form = PB_FORM_MOVING_AVG},
    {.name = "moving-sum", .form = PB_FORM_MOVING_SUM},
    {.name = "moving-max", .form = PB_FORM_MOVING_MAX},
    {.name = "moving-min", .form = PB_FORM_MOVING_MIN},
    {.name = "median", .form = PB_FORM_MEDIAN},
    {.name = "percentile", .form = PB_FORM_PERCENTILE},
    {.name = "stddev", .form = PB_FORM_STDDEV},
    {.name = "variance", .form = PB_FORM_VARIANCE},
    {.name = "rate", .form = PB_FORM_RATE},
    {.name = "throttle", .form = PB_FORM_THROTTLE},

    // Clocked publish forms.
    {.name = "debounce!", .form = PB_FORM_DEBOUNCE},
    {.name = "sample!", .form = PB_FORM_SAMPLE},
    {.name = "aggregate!", .form = PB_FORM_AGGREGATE},

    // Bar/OHLC forms.
    {.name = "bar!", .form = PB_FORM_BAR},
    {.name = "bar", .form = PB_FORM_BAR},
};

static const pb_form_entry *find_form(pb_slice name) {
    for (size_t i = 0; i < sizeof FORMS / sizeof FORMS[0]; i += 1) {
        if (text_eq(name, FORMS[i].name)) {
            return &FORMS[i];
        }
    }
    return NULL;
}

static void state_entry_free(pb_eval_state_entry *e) {
    free(e->op);
    free(e->subject);
    free(e->bytes);
    free(e->ring_values);
    free(e->ring_times_ms);
    free(e->emit_subject);
    *e = (pb_eval_state_entry){0};
}

void pb_eval_state_free(pb_eval_state *state) {
    for (size_t i = 0; i < state->len; i += 1) {
        state_entry_free(&state->items[i]);
    }
    free(state->items);
    free(state->index);
    *state = (pb_eval_state){0};
}

static uint64_t state_hash_update(uint64_t h, const void *ptr, size_t len) {
    const unsigned char *bytes = ptr;
    for (size_t i = 0; i < len; i += 1) {
        h ^= (uint64_t)bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t state_key_hash(size_t rule_id, pb_slice op, pb_slice subject) {
    uint64_t h = 1469598103934665603ULL;
    h = state_hash_update(h, &rule_id, sizeof rule_id);
    h = state_hash_update(h, op.ptr, op.len);
    h ^= 0xff;
    h *= 1099511628211ULL;
    return state_hash_update(h, subject.ptr, subject.len);
}

static bool state_key_eq(const pb_eval_state_entry *e, size_t rule_id, pb_slice op, pb_slice subject) {
    return e->rule_id == rule_id &&
           pb_slice_eq((pb_slice){.ptr = e->op, .len = e->op_len}, op) &&
           pb_slice_eq((pb_slice){.ptr = e->subject, .len = e->subject_len}, subject);
}

static size_t state_index_cap_for(size_t needed) {
    if (needed > SIZE_MAX / 4) {
        return 0;
    }
    size_t cap = 16;
    const size_t target = needed == 0 ? 16 : needed * 4;
    while (cap < target) {
        if (cap > SIZE_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }
    return cap;
}

static void state_index_insert(pb_eval_state_index_entry *index, size_t cap, uint64_t hash, size_t item_index) {
    size_t slot = (size_t)(hash & (uint64_t)(cap - 1));
    while (index[slot].occupied) {
        slot = (slot + 1) & (cap - 1);
    }
    index[slot] = (pb_eval_state_index_entry){.hash = hash, .index = item_index, .occupied = true};
}

static bool state_index_rebuild(pb_eval_state *state, size_t needed) {
    if (state->index_cap != 0 && needed < state->index_cap / 2) {
        return true;
    }
    const size_t cap = state_index_cap_for(needed);
    if (cap == 0) {
        return false;
    }
    pb_eval_state_index_entry *index = calloc(cap, sizeof index[0]);
    if (index == NULL) {
        return false;
    }
    for (size_t i = 0; i < state->len; i += 1) {
        const pb_eval_state_entry *e = &state->items[i];
        const pb_slice op = {.ptr = e->op, .len = e->op_len};
        const pb_slice subject = {.ptr = e->subject, .len = e->subject_len};
        state_index_insert(index, cap, state_key_hash(e->rule_id, op, subject), i);
    }
    free(state->index);
    state->index = index;
    state->index_cap = cap;
    return true;
}

static pb_eval_state_entry *state_index_find(pb_eval_state *state, size_t rule_id, pb_slice op, pb_slice subject, uint64_t hash) {
    if (state->index_cap == 0) {
        return NULL;
    }
    size_t slot = (size_t)(hash & (uint64_t)(state->index_cap - 1));
    for (;;) {
        const pb_eval_state_index_entry *entry = &state->index[slot];
        if (!entry->occupied) {
            return NULL;
        }
        pb_eval_state_entry *candidate = &state->items[entry->index];
        if (entry->hash == hash && state_key_eq(candidate, rule_id, op, subject)) {
            return candidate;
        }
        slot = (slot + 1) & (state->index_cap - 1);
    }
}

static bool heap_dup_slice(pb_slice s, char **out, size_t *out_len) {
    char *ptr = malloc(s.len == 0 ? 1 : s.len);
    if (ptr == NULL) {
        return false;
    }
    memcpy(ptr, s.ptr, s.len);
    *out = ptr;
    *out_len = s.len;
    return true;
}

pb_eval_state_entry *state_slot(pb_eval_ctx *ctx, const char *op_lit) {
    if (ctx->state == NULL) {
        return NULL;
    }

    const pb_slice op = {.ptr = op_lit, .len = strlen(op_lit)};
    const uint64_t hash = state_key_hash(ctx->rule_id, op, ctx->subject);
    if (ctx->state->index_cap == 0 && ctx->state->len >= 8) {
        (void)state_index_rebuild(ctx->state, ctx->state->len);
    }
    pb_eval_state_entry *found = state_index_find(ctx->state, ctx->rule_id, op, ctx->subject, hash);
    if (found != NULL) {
        return found;
    }
    if (ctx->state->index_cap == 0) {
        for (size_t i = 0; i < ctx->state->len; i += 1) {
            if (state_key_eq(&ctx->state->items[i], ctx->rule_id, op, ctx->subject)) {
                return &ctx->state->items[i];
            }
        }
    }

    if ((ctx->state->index_cap != 0 || ctx->state->len >= 8) &&
        !state_index_rebuild(ctx->state, ctx->state->len + 1)) {
        return NULL;
    }
    if (!mb_array_reserve((void **)&ctx->state->items, &ctx->state->cap,
                          ctx->state->len + 1, sizeof ctx->state->items[0], 8)) {
        return NULL;
    }

    pb_eval_state_entry *e = &ctx->state->items[ctx->state->len];
    *e = (pb_eval_state_entry){.rule_id = ctx->rule_id};
    if (!heap_dup_slice(op, &e->op, &e->op_len) ||
        !heap_dup_slice(ctx->subject, &e->subject, &e->subject_len)) {
        state_entry_free(e);
        return NULL;
    }
    ctx->state->len += 1;
    if (ctx->state->index_cap != 0) {
        state_index_insert(ctx->state->index, ctx->state->index_cap, hash, ctx->state->len - 1);
    }
    return e;
}

bool state_set_bytes(pb_eval_state_entry *e, pb_slice bytes) {
    if (e->bytes_cap < bytes.len) {
        char *next = realloc(e->bytes, bytes.len == 0 ? 1 : bytes.len);
        if (next == NULL) {
            return false;
        }
        e->bytes = next;
        e->bytes_cap = bytes.len;
    }
    memcpy(e->bytes, bytes.ptr, bytes.len);
    e->bytes_len = bytes.len;
    e->kind = PB_EVAL_STATE_BYTES;
    return true;
}

bool state_set_emit_subject(pb_eval_state_entry *e, pb_slice bytes) {
    if (e->emit_subject_cap < bytes.len) {
        char *next = realloc(e->emit_subject, bytes.len == 0 ? 1 : bytes.len);
        if (next == NULL) {
            return false;
        }
        e->emit_subject = next;
        e->emit_subject_cap = bytes.len;
    }
    memcpy(e->emit_subject, bytes.ptr, bytes.len);
    e->emit_subject_len = bytes.len;
    return true;
}

pb_eval_result pb_eval(pb_eval_ctx *ctx, pb_value expr) {
    switch (expr.kind) {
    case PB_NIL:
    case PB_BOOL:
    case PB_NUMBER:
    case PB_STRING:
    case PB_KEYWORD:
        return ok(expr);
    case PB_SYMBOL:
        if (text_eq(expr.text, "subject")) {
            return ok((pb_value){.kind = PB_STRING, .text = ctx->subject});
        }
        if (text_eq(expr.text, "payload")) {
            return ok((pb_value){.kind = PB_STRING, .text = ctx->payload});
        }
        if (text_eq(expr.text, "payload-float") || text_eq(expr.text, "payload-int")) {
            pb_value payload = {.kind = PB_STRING, .text = ctx->payload};
            double n = 0;
            if (!as_number(payload, &n)) {
                return fail(PB_EVAL_TYPE);
            }
            if (text_eq(expr.text, "payload-int") && floor(n) != n) {
                return fail(PB_EVAL_TYPE);
            }
            return ok((pb_value){.kind = PB_NUMBER, .number = n});
        }
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    case PB_VECTOR: {
        pb_value *items = pb_arena_alloc(ctx->arena, expr.seq.len * sizeof items[0], _Alignof(pb_value));
        if (items == NULL && expr.seq.len != 0) {
            return fail(PB_EVAL_OOM);
        }
        for (size_t i = 0; i < expr.seq.len; i += 1) {
            pb_eval_result r = pb_eval(ctx, expr.seq.items[i]);
            if (r.err != PB_EVAL_OK) {
                return r;
            }
            items[i] = r.value;
        }
        return ok((pb_value){.kind = PB_VECTOR, .seq = {.items = items, .len = expr.seq.len}});
    }
    case PB_LIST:
        return eval_list(ctx, expr.seq);
    }
    return fail(PB_EVAL_TYPE);
}

static pb_eval_result eval_args(pb_eval_ctx *ctx, pb_values raw, pb_values *out) {
    pb_value *items = pb_arena_alloc(ctx->arena, raw.len * sizeof items[0], _Alignof(pb_value));
    if (items == NULL && raw.len != 0) {
        return fail(PB_EVAL_OOM);
    }
    for (size_t i = 0; i < raw.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, raw.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        items[i] = r.value;
    }
    *out = (pb_values){.items = items, .len = raw.len};
    return nil();
}

static pb_eval_result eval_do(pb_eval_ctx *ctx, pb_values args) {
    pb_value last = {.kind = PB_NIL};
    for (size_t i = 0; i < args.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, args.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        last = r.value;
    }
    return ok(last);
}

static pb_eval_result eval_if(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2 && args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result cond = pb_eval(ctx, args.items[0]);
    if (cond.err != PB_EVAL_OK) {
        return cond;
    }
    if (truthy(cond.value)) {
        return pb_eval(ctx, args.items[1]);
    }
    if (args.len == 3) {
        return pb_eval(ctx, args.items[2]);
    }
    return nil();
}

static pb_eval_result eval_when(pb_eval_ctx *ctx, pb_values args) {
    if (args.len < 1) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result cond = pb_eval(ctx, args.items[0]);
    if (cond.err != PB_EVAL_OK) {
        return cond;
    }
    if (!truthy(cond.value)) {
        return nil();
    }
    return eval_do(ctx, (pb_values){.items = args.items + 1, .len = args.len - 1});
}

static pb_eval_result eval_and(pb_eval_ctx *ctx, pb_values args) {
    pb_value last = {.kind = PB_BOOL, .boolean = true};
    for (size_t i = 0; i < args.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, args.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        if (!truthy(r.value)) {
            return ok(r.value);
        }
        last = r.value;
    }
    return ok(last);
}

static pb_eval_result eval_or(pb_eval_ctx *ctx, pb_values args) {
    for (size_t i = 0; i < args.len; i += 1) {
        pb_eval_result r = pb_eval(ctx, args.items[i]);
        if (r.err != PB_EVAL_OK) {
            return r;
        }
        if (truthy(r.value)) {
            return ok(r.value);
        }
    }
    return nil();
}

static pb_eval_result eval_call_with_threaded(pb_eval_ctx *ctx, pb_value form, pb_value threaded) {
    if (form.kind == PB_SYMBOL) {
        pb_value items[2] = {form, threaded};
        return eval_list(ctx, (pb_values){.items = items, .len = 2});
    }
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        return fail(PB_EVAL_TYPE);
    }

    pb_value *items = pb_arena_alloc(ctx->arena, (form.seq.len + 1) * sizeof items[0], _Alignof(pb_value));
    if (items == NULL) {
        return fail(PB_EVAL_OOM);
    }
    memcpy(items, form.seq.items, form.seq.len * sizeof items[0]);
    items[form.seq.len] = threaded;
    return eval_list(ctx, (pb_values){.items = items, .len = form.seq.len + 1});
}

static pb_eval_result eval_thread(pb_eval_ctx *ctx, pb_values args) {
    if (args.len == 0) {
        return fail(PB_EVAL_ARITY);
    }
    pb_eval_result cur = pb_eval(ctx, args.items[0]);
    if (cur.err != PB_EVAL_OK) {
        return cur;
    }
    for (size_t i = 1; i < args.len; i += 1) {
        if (cur.value.kind == PB_NIL) {
            return cur;
        }
        cur = eval_call_with_threaded(ctx, args.items[i], cur.value);
        if (cur.err != PB_EVAL_OK) {
            return cur;
        }
    }
    return cur;
}

static pb_eval_result eval_transition(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }

    pb_eval_result cond = pb_eval(ctx, args.items[0]);
    if (cond.err != PB_EVAL_OK) {
        return cond;
    }

    const bool cur = truthy(cond.value);
    pb_eval_state_entry *slot = state_slot(ctx, "transition");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }

    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_NUMBER;
        slot->number = cur ? 1.0 : 0.0;
        return nil();
    }

    if (slot->kind != PB_EVAL_STATE_NUMBER) {
        return fail(PB_EVAL_TYPE);
    }

    const bool prev = slot->number != 0.0;
    slot->number = cur ? 1.0 : 0.0;
    if (!prev && cur) {
        return pb_eval(ctx, args.items[1]);
    }
    if (prev && !cur) {
        return pb_eval(ctx, args.items[2]);
    }
    return nil();
}

static pb_eval_result eval_list(pb_eval_ctx *ctx, pb_values call) {
    const pb_slice head = call.items[0].text;
    const pb_values raw_args = {.items = call.items + 1, .len = call.len - 1};
    const pb_form_entry *entry = find_form(head);
    if (entry == NULL) {
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }

    if (entry->special) {
        switch (entry->form) {
        case PB_FORM_DO: return eval_do(ctx, raw_args);
        case PB_FORM_IF: return eval_if(ctx, raw_args);
        case PB_FORM_WHEN: return eval_when(ctx, raw_args);
        case PB_FORM_AND: return eval_and(ctx, raw_args);
        case PB_FORM_OR: return eval_or(ctx, raw_args);
        case PB_FORM_THREAD: return eval_thread(ctx, raw_args);
        case PB_FORM_TRANSITION: return eval_transition(ctx, raw_args);
        case PB_FORM_DROPOUT: return pb_eval_call_dropout(ctx, raw_args);
        default: return fail(PB_EVAL_UNKNOWN_SYMBOL);
        }
    }

    pb_values args = {0};
    pb_eval_result er = eval_args(ctx, raw_args, &args);
    if (er.err != PB_EVAL_OK) {
        return er;
    }
    return pb_eval_call_form(ctx, entry->form, args);
}

const char *pb_eval_error_name(pb_eval_error err) {
    switch (err) {
    case PB_EVAL_OK: return "ok";
    case PB_EVAL_OOM: return "out of memory";
    case PB_EVAL_UNKNOWN_SYMBOL: return "unknown symbol";
    case PB_EVAL_TYPE: return "type mismatch";
    case PB_EVAL_ARITY: return "arity mismatch";
    case PB_EVAL_INVALID_SUBJECT: return "invalid subject";
    case PB_EVAL_PUBLISH_FAILED: return "publish failed";
    }
    return "unknown eval error";
}
