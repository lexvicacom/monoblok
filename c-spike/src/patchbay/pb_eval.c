#include "pb_eval.h"

#include "yyjson.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pb_eval_result ok(pb_value v) {
    return (pb_eval_result){.err = PB_EVAL_OK, .value = v};
}

static pb_eval_result fail(pb_eval_error err) {
    return (pb_eval_result){.err = err};
}

static bool text_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool truthy(pb_value v) {
    return v.kind != PB_NIL && !(v.kind == PB_BOOL && !v.boolean);
}

static pb_value bool_value(bool b) {
    return (pb_value){.kind = PB_BOOL, .boolean = b};
}

static bool as_number(pb_value v, double *out) {
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
    const bool ok_num = errno == 0 && end == tmp + v.text.len;
    if (!ok_num) {
        return false;
    }
    *out = n;
    return true;
}

static bool as_string(pb_value v, pb_slice *out) {
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

static bool coerce_payload(pb_eval_ctx *ctx, pb_value v, pb_slice *out) {
    if (as_string(v, out)) {
        return true;
    }
    if (v.kind != PB_NUMBER) {
        return false;
    }
    char tmp[32];
    const int n = snprintf(tmp, sizeof tmp, "%.17g", v.number);
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

static bool value_eq(pb_value a, pb_value b) {
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
        return a.text.len == b.text.len && memcmp(a.text.ptr, b.text.ptr, a.text.len) == 0;
    case PB_LIST:
    case PB_VECTOR:
        return false;
    }
    return false;
}

static pb_eval_result eval_list(pb_eval_ctx *ctx, pb_values call);
static pb_eval_result eval_thread(pb_eval_ctx *ctx, pb_values args);

typedef enum pb_builtin {
    PB_BUILTIN_DO,
    PB_BUILTIN_IF,
    PB_BUILTIN_WHEN,
    PB_BUILTIN_AND,
    PB_BUILTIN_OR,
    PB_BUILTIN_THREAD,
    PB_BUILTIN_NOT,
    PB_BUILTIN_EQ,
    PB_BUILTIN_GT,
    PB_BUILTIN_LT,
    PB_BUILTIN_GE,
    PB_BUILTIN_LE,
    PB_BUILTIN_ADD,
    PB_BUILTIN_SUB,
    PB_BUILTIN_MUL,
    PB_BUILTIN_DIV,
    PB_BUILTIN_STR_CONCAT,
    PB_BUILTIN_CONTAINS,
    PB_BUILTIN_STARTS_WITH,
    PB_BUILTIN_ENDS_WITH,
    PB_BUILTIN_SUBJECT_APPEND,
    PB_BUILTIN_SUBJECT_TOKEN,
    PB_BUILTIN_SUBJECT_WITH,
    PB_BUILTIN_PUBLISH,
    PB_BUILTIN_JSON_GET,
    PB_BUILTIN_JSON_DEMUX,
    PB_BUILTIN_ROUND,
    PB_BUILTIN_CLAMP,
    PB_BUILTIN_MIN,
    PB_BUILTIN_MAX,
    PB_BUILTIN_ABS,
    PB_BUILTIN_SIGN,
    PB_BUILTIN_SQUELCH,
    PB_BUILTIN_DEADBAND,
    PB_BUILTIN_CHANGED,
    PB_BUILTIN_DELTA,
    PB_BUILTIN_COUNT,
    PB_BUILTIN_MOVING_AVG,
} pb_builtin;

typedef struct pb_builtin_entry {
    const char *name;
    pb_builtin builtin;
    bool special;
} pb_builtin_entry;

static const pb_builtin_entry BUILTINS[] = {
    {.name = "do", .builtin = PB_BUILTIN_DO, .special = true},
    {.name = "if", .builtin = PB_BUILTIN_IF, .special = true},
    {.name = "when", .builtin = PB_BUILTIN_WHEN, .special = true},
    {.name = "and", .builtin = PB_BUILTIN_AND, .special = true},
    {.name = "or", .builtin = PB_BUILTIN_OR, .special = true},
    {.name = "->", .builtin = PB_BUILTIN_THREAD, .special = true},
    {.name = "not", .builtin = PB_BUILTIN_NOT},
    {.name = "=", .builtin = PB_BUILTIN_EQ},
    {.name = ">", .builtin = PB_BUILTIN_GT},
    {.name = "<", .builtin = PB_BUILTIN_LT},
    {.name = ">=", .builtin = PB_BUILTIN_GE},
    {.name = "<=", .builtin = PB_BUILTIN_LE},
    {.name = "+", .builtin = PB_BUILTIN_ADD},
    {.name = "-", .builtin = PB_BUILTIN_SUB},
    {.name = "*", .builtin = PB_BUILTIN_MUL},
    {.name = "/", .builtin = PB_BUILTIN_DIV},
    {.name = "str-concat", .builtin = PB_BUILTIN_STR_CONCAT},
    {.name = "contains?", .builtin = PB_BUILTIN_CONTAINS},
    {.name = "starts-with?", .builtin = PB_BUILTIN_STARTS_WITH},
    {.name = "ends-with?", .builtin = PB_BUILTIN_ENDS_WITH},
    {.name = "subject-append", .builtin = PB_BUILTIN_SUBJECT_APPEND},
    {.name = "subject-token", .builtin = PB_BUILTIN_SUBJECT_TOKEN},
    {.name = "subject-with", .builtin = PB_BUILTIN_SUBJECT_WITH},
    {.name = "publish!", .builtin = PB_BUILTIN_PUBLISH},
    {.name = "publish", .builtin = PB_BUILTIN_PUBLISH},
    {.name = "json-get", .builtin = PB_BUILTIN_JSON_GET},
    {.name = "json-demux!", .builtin = PB_BUILTIN_JSON_DEMUX},
    {.name = "round", .builtin = PB_BUILTIN_ROUND},
    {.name = "clamp", .builtin = PB_BUILTIN_CLAMP},
    {.name = "min", .builtin = PB_BUILTIN_MIN},
    {.name = "max", .builtin = PB_BUILTIN_MAX},
    {.name = "abs", .builtin = PB_BUILTIN_ABS},
    {.name = "sign", .builtin = PB_BUILTIN_SIGN},
    {.name = "squelch", .builtin = PB_BUILTIN_SQUELCH},
    {.name = "deadband", .builtin = PB_BUILTIN_DEADBAND},
    {.name = "changed?", .builtin = PB_BUILTIN_CHANGED},
    {.name = "delta", .builtin = PB_BUILTIN_DELTA},
    {.name = "count!", .builtin = PB_BUILTIN_COUNT},
    {.name = "count", .builtin = PB_BUILTIN_COUNT},
    {.name = "moving-avg", .builtin = PB_BUILTIN_MOVING_AVG},
};

static const pb_builtin_entry *find_builtin(pb_slice name) {
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i += 1) {
        if (text_eq(name, BUILTINS[i].name)) {
            return &BUILTINS[i];
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
    *e = (pb_eval_state_entry){0};
}

void pb_eval_state_free(pb_eval_state *state) {
    for (size_t i = 0; i < state->len; i += 1) {
        state_entry_free(&state->items[i]);
    }
    free(state->items);
    *state = (pb_eval_state){0};
}

static bool state_key_eq(const pb_eval_state_entry *e, size_t rule_id, pb_slice op, pb_slice subject) {
    return e->rule_id == rule_id &&
           e->op_len == op.len &&
           e->subject_len == subject.len &&
           memcmp(e->op, op.ptr, op.len) == 0 &&
           memcmp(e->subject, subject.ptr, subject.len) == 0;
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

static pb_eval_state_entry *state_slot(pb_eval_ctx *ctx, const char *op_lit) {
    if (ctx->state == NULL) {
        return NULL;
    }
    const pb_slice op = {.ptr = op_lit, .len = strlen(op_lit)};
    for (size_t i = 0; i < ctx->state->len; i += 1) {
        if (state_key_eq(&ctx->state->items[i], ctx->rule_id, op, ctx->subject)) {
            return &ctx->state->items[i];
        }
    }
    if (ctx->state->len == ctx->state->cap) {
        const size_t next = ctx->state->cap == 0 ? 8 : ctx->state->cap * 2;
        pb_eval_state_entry *items = realloc(ctx->state->items, next * sizeof items[0]);
        if (items == NULL) {
            return NULL;
        }
        ctx->state->items = items;
        ctx->state->cap = next;
    }
    pb_eval_state_entry *e = &ctx->state->items[ctx->state->len];
    *e = (pb_eval_state_entry){.rule_id = ctx->rule_id};
    if (!heap_dup_slice(op, &e->op, &e->op_len) ||
        !heap_dup_slice(ctx->subject, &e->subject, &e->subject_len)) {
        state_entry_free(e);
        return NULL;
    }
    ctx->state->len += 1;
    return e;
}

static bool state_set_bytes(pb_eval_state_entry *e, pb_slice bytes) {
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
    return ok((pb_value){.kind = PB_NIL});
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
    return ok((pb_value){.kind = PB_NIL});
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
        return ok((pb_value){.kind = PB_NIL});
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
    return ok((pb_value){.kind = PB_NIL});
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
        cur = eval_call_with_threaded(ctx, args.items[i], cur.value);
        if (cur.err != PB_EVAL_OK) {
            return cur;
        }
    }
    return cur;
}

static pb_eval_result call_not(pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    return ok(bool_value(!truthy(args.items[0])));
}

static pb_eval_result call_eq(pb_values args) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    for (size_t i = 1; i < args.len; i += 1) {
        if (!value_eq(args.items[0], args.items[i])) {
            return ok(bool_value(false));
        }
    }
    return ok(bool_value(true));
}

typedef enum cmp_op { CMP_GT, CMP_LT, CMP_GE, CMP_LE } cmp_op;

static pb_eval_result call_cmp(pb_values args, cmp_op op) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    for (size_t i = 0; i + 1 < args.len; i += 1) {
        double a = 0;
        double b = 0;
        if (!as_number(args.items[i], &a) || !as_number(args.items[i + 1], &b)) {
            return fail(PB_EVAL_TYPE);
        }
        const bool pass =
            (op == CMP_GT && a > b) ||
            (op == CMP_LT && a < b) ||
            (op == CMP_GE && a >= b) ||
            (op == CMP_LE && a <= b);
        if (!pass) {
            return ok(bool_value(false));
        }
    }
    return ok(bool_value(true));
}

typedef enum arith_op { ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV } arith_op;

static pb_eval_result call_arith(pb_values args, arith_op op) {
    if (args.len == 0) {
        return fail(PB_EVAL_ARITY);
    }
    double acc = 0;
    if (!as_number(args.items[0], &acc)) {
        return fail(PB_EVAL_TYPE);
    }
    if (args.len == 1) {
        if (op == ARITH_SUB) acc = -acc;
        if (op == ARITH_DIV) acc = 1.0 / acc;
        return ok((pb_value){.kind = PB_NUMBER, .number = acc});
    }
    for (size_t i = 1; i < args.len; i += 1) {
        double x = 0;
        if (!as_number(args.items[i], &x)) {
            return fail(PB_EVAL_TYPE);
        }
        if (op == ARITH_ADD) acc += x;
        if (op == ARITH_SUB) acc -= x;
        if (op == ARITH_MUL) acc *= x;
        if (op == ARITH_DIV) acc /= x;
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = acc});
}

static pb_eval_result call_round(pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double places = 0;
    double x = 0;
    if (!as_number(args.items[0], &places) || !as_number(args.items[1], &x) || places < 0 || floor(places) != places) {
        return fail(PB_EVAL_TYPE);
    }
    const double factor = pow(10.0, places);
    return ok((pb_value){.kind = PB_NUMBER, .number = round(x * factor) / factor});
}

static pb_eval_result call_clamp(pb_values args) {
    if (args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }
    double lo = 0;
    double hi = 0;
    double x = 0;
    if (!as_number(args.items[0], &lo) || !as_number(args.items[1], &hi) || !as_number(args.items[2], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return ok((pb_value){.kind = PB_NUMBER, .number = x});
}

typedef enum minmax_kind { MINMAX_MIN, MINMAX_MAX } minmax_kind;

static pb_eval_result call_minmax(pb_values args, minmax_kind kind) {
    if (args.len == 0) {
        return fail(PB_EVAL_ARITY);
    }
    double acc = 0;
    if (!as_number(args.items[0], &acc)) {
        return fail(PB_EVAL_TYPE);
    }
    for (size_t i = 1; i < args.len; i += 1) {
        double x = 0;
        if (!as_number(args.items[i], &x)) {
            return fail(PB_EVAL_TYPE);
        }
        if (kind == MINMAX_MIN && x < acc) acc = x;
        if (kind == MINMAX_MAX && x > acc) acc = x;
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = acc});
}

static pb_eval_result call_abs(pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[0], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = fabs(x)});
}

static pb_eval_result call_sign(pb_values args) {
    if (args.len != 1) {
        return fail(PB_EVAL_ARITY);
    }
    double x = 0;
    if (!as_number(args.items[0], &x)) {
        return fail(PB_EVAL_TYPE);
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = (x > 0) - (x < 0)});
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
    if (args.len != 1 || args.items[0].kind != PB_NUMBER || args.items[0].number < 0) {
        return fail(PB_EVAL_ARITY);
    }
    const size_t want = (size_t)args.items[0].number;
    size_t tok = 0;
    size_t start = 0;
    for (size_t i = 0; i <= ctx->subject.len; i += 1) {
        if (i == ctx->subject.len || ctx->subject.ptr[i] == '.') {
            if (tok == want) {
                return ok((pb_value){.kind = PB_STRING, .text = {.ptr = ctx->subject.ptr + start, .len = i - start}});
            }
            tok += 1;
            start = i + 1;
        }
    }
    return ok((pb_value){.kind = PB_NIL});
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
        if (!as_string(toks.items[i], &parts[i]) || parts[i].len == 0) {
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
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = out, .len = total}});
}

static pb_eval_result call_publish(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    if (args.items[1].kind == PB_NIL) {
        return ok((pb_value){.kind = PB_NIL});
    }
    pb_slice subject = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &subject) || !coerce_payload(ctx, args.items[1], &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    if (ctx->publish == NULL || !ctx->publish(ctx->publish_ctx, subject, payload)) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

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
        return ok((pb_value){.kind = PB_NIL});
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
    if (args.len > 1) {
        return fail(PB_EVAL_ARITY);
    }
    const bool inc = args.len == 0 || truthy(args.items[0]);
    pb_eval_state_entry *slot = state_slot(ctx, "count");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (!inc) {
        return ok((pb_value){.kind = PB_NIL});
    }
    const double next = (slot->kind == PB_EVAL_STATE_NUMBER ? slot->number : 0) + 1;
    slot->kind = PB_EVAL_STATE_NUMBER;
    slot->number = next;

    const size_t subject_len = ctx->subject.len + 6;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return fail(PB_EVAL_OOM);
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    memcpy(subject_ptr + ctx->subject.len, ".count", 6);

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
        !ctx->publish(ctx->publish_ctx, (pb_slice){.ptr = subject_ptr, .len = subject_len},
                      (pb_slice){.ptr = payload_ptr, .len = (size_t)n})) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static uint64_t now_ms(void) {
    struct timespec ts = {0};
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static bool ring_reserve(pb_eval_state_entry *slot, size_t cap, bool with_times) {
    if (slot->ring_cap >= cap) {
        return true;
    }

    double *values = malloc(cap * sizeof values[0]);
    if (values == NULL) {
        return false;
    }
    uint64_t *times = NULL;
    if (with_times) {
        times = malloc(cap * sizeof times[0]);
        if (times == NULL) {
            free(values);
            return false;
        }
    }

    for (size_t i = 0; i < slot->ring_len; i += 1) {
        const size_t old_idx = slot->ring_cap == 0 ? 0 : (slot->ring_start + i) % slot->ring_cap;
        values[i] = slot->ring_values[old_idx];
        if (with_times && slot->ring_times_ms != NULL) {
            times[i] = slot->ring_times_ms[old_idx];
        }
    }

    free(slot->ring_values);
    free(slot->ring_times_ms);
    slot->ring_values = values;
    slot->ring_times_ms = times;
    slot->ring_cap = cap;
    slot->ring_start = 0;
    return true;
}

static void ring_push_count(pb_eval_state_entry *slot, double x) {
    if (slot->ring_len < slot->ring_cap) {
        const size_t idx = (slot->ring_start + slot->ring_len) % slot->ring_cap;
        slot->ring_values[idx] = x;
        slot->ring_len += 1;
        slot->ring_sum += x;
        return;
    }
    const size_t idx = slot->ring_start;
    slot->ring_sum -= slot->ring_values[idx];
    slot->ring_values[idx] = x;
    slot->ring_sum += x;
    slot->ring_start = (slot->ring_start + 1) % slot->ring_cap;
}

static void ring_evict_time(pb_eval_state_entry *slot, uint64_t cutoff_ms) {
    while (slot->ring_len > 0) {
        const size_t idx = slot->ring_start;
        if (slot->ring_times_ms[idx] >= cutoff_ms) {
            break;
        }
        slot->ring_sum -= slot->ring_values[idx];
        slot->ring_start = (slot->ring_start + 1) % slot->ring_cap;
        slot->ring_len -= 1;
    }
}

static bool ring_push_time(pb_eval_state_entry *slot, double x, uint64_t t_ms) {
    if (slot->ring_len == slot->ring_cap) {
        const size_t next = slot->ring_cap == 0 ? 16 : slot->ring_cap * 2;
        if (!ring_reserve(slot, next, true)) {
            return false;
        }
    }
    const size_t idx = (slot->ring_start + slot->ring_len) % slot->ring_cap;
    slot->ring_values[idx] = x;
    slot->ring_times_ms[idx] = t_ms;
    slot->ring_len += 1;
    slot->ring_sum += x;
    return true;
}

static pb_eval_result call_moving_avg(pb_eval_ctx *ctx, pb_values args) {
    bool time_window = false;
    size_t window_count = 0;
    uint64_t window_ms = 0;
    pb_value input = {0};

    if (args.len == 2) {
        double window = 0;
        if (!as_number(args.items[0], &window) || window < 1 || floor(window) != window) {
            return fail(PB_EVAL_TYPE);
        }
        window_count = (size_t)window;
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

    pb_eval_state_entry *slot = state_slot(ctx, time_window ? "moving-avg/m" : "moving-avg/t");
    if (slot == NULL) {
        return fail(PB_EVAL_OOM);
    }
    if (slot->kind == PB_EVAL_STATE_EMPTY) {
        slot->kind = PB_EVAL_STATE_RING;
        slot->ring_time_window = time_window;
        slot->ring_window_ms = window_ms;
        if (!time_window && !ring_reserve(slot, window_count, false)) {
            return fail(PB_EVAL_OOM);
        }
    }
    if (slot->kind != PB_EVAL_STATE_RING || slot->ring_time_window != time_window) {
        return fail(PB_EVAL_TYPE);
    }

    if (time_window) {
        const uint64_t configured = slot->ring_window_ms == 0 ? window_ms : slot->ring_window_ms;
        const uint64_t t = now_ms();
        const uint64_t cutoff = t > configured ? t - configured : 0;
        ring_evict_time(slot, cutoff);
        if (!ring_push_time(slot, x, t)) {
            return fail(PB_EVAL_OOM);
        }
        ring_evict_time(slot, cutoff);
    } else {
        ring_push_count(slot, x);
    }

    if (slot->ring_len == 0) {
        return ok((pb_value){.kind = PB_NIL});
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = slot->ring_sum / (double)slot->ring_len});
}

static bool path_token(pb_slice path, size_t *pos, pb_slice *out) {
    if (*pos >= path.len) {
        return false;
    }
    const size_t start = *pos;
    while (*pos < path.len && path.ptr[*pos] != '.') {
        *pos += 1;
    }
    if (*pos == start) {
        return false;
    }
    *out = (pb_slice){.ptr = path.ptr + start, .len = *pos - start};
    if (*pos < path.len && path.ptr[*pos] == '.') {
        *pos += 1;
    }
    return true;
}

static bool token_index(pb_slice s, size_t *out) {
    if (s.len == 0) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') {
            return false;
        }
        n = n * 10 + (size_t)(s.ptr[i] - '0');
    }
    *out = n;
    return true;
}

static yyjson_val *json_lookup(yyjson_val *root, pb_slice path) {
    yyjson_val *cur = root;
    size_t pos = 0;
    pb_slice tok = {0};
    while (path_token(path, &pos, &tok)) {
        if (yyjson_is_obj(cur)) {
            cur = yyjson_obj_getn(cur, tok.ptr, tok.len);
        } else if (yyjson_is_arr(cur)) {
            size_t idx = 0;
            cur = token_index(tok, &idx) ? yyjson_arr_get(cur, idx) : NULL;
        } else {
            return NULL;
        }
        if (cur == NULL) {
            return NULL;
        }
    }
    return pos == path.len ? cur : NULL;
}

static void *json_arena_malloc(void *ctx, size_t size) {
    return pb_arena_alloc(ctx, size == 0 ? 1 : size, _Alignof(max_align_t));
}

static void *json_arena_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    void *next = pb_arena_alloc(ctx, size == 0 ? 1 : size, _Alignof(max_align_t));
    if (next == NULL) {
        return NULL;
    }
    if (ptr != NULL) {
        memcpy(next, ptr, old_size < size ? old_size : size);
    }
    return next;
}

static void json_arena_free(void *ctx, void *ptr) {
    (void)ctx;
    (void)ptr;
}

static yyjson_doc *read_json_slice(pb_eval_ctx *ctx, pb_slice source) {
    char *owned = pb_arena_memdup(ctx->arena, source.ptr, source.len);
    if (owned == NULL) {
        return NULL;
    }
    yyjson_alc alc = {
        .malloc = json_arena_malloc,
        .realloc = json_arena_realloc,
        .free = json_arena_free,
        .ctx = ctx->arena,
    };
    return yyjson_read_opts(owned, source.len, YYJSON_READ_NOFLAG, &alc, NULL);
}

static pb_eval_result json_scalar_value(pb_eval_ctx *ctx, yyjson_val *v) {
    if (v == NULL || yyjson_is_null(v) || yyjson_is_obj(v) || yyjson_is_arr(v)) {
        return ok((pb_value){.kind = PB_NIL});
    }
    if (yyjson_is_bool(v)) {
        return ok((pb_value){.kind = PB_BOOL, .boolean = yyjson_get_bool(v)});
    }
    if (yyjson_is_num(v)) {
        return ok((pb_value){.kind = PB_NUMBER, .number = yyjson_get_num(v)});
    }
    if (yyjson_is_str(v)) {
        const size_t len = yyjson_get_len(v);
        char *owned = pb_arena_memdup(ctx->arena, yyjson_get_str(v), len);
        if (owned == NULL) {
            return fail(PB_EVAL_OOM);
        }
        return ok((pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = len}});
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result call_json_get(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice path = {0};
    pb_slice payload = {0};
    if (!as_string(args.items[0], &path) || !as_string(args.items[1], &payload) || path.len == 0) {
        return fail(PB_EVAL_TYPE);
    }

    yyjson_doc *doc = read_json_slice(ctx, payload);
    if (doc == NULL) {
        return ok((pb_value){.kind = PB_NIL});
    }
    yyjson_val *found = json_lookup(yyjson_doc_get_root(doc), path);
    const pb_eval_result r = json_scalar_value(ctx, found);
    yyjson_doc_free(doc);
    return r;
}

static pb_slice path_leaf(pb_slice path) {
    size_t start = 0;
    for (size_t i = 0; i < path.len; i += 1) {
        if (path.ptr[i] == '.') {
            start = i + 1;
        }
    }
    return (pb_slice){.ptr = path.ptr + start, .len = path.len - start};
}

static bool demux_spec(pb_value spec, pb_slice *path, pb_slice *suffix) {
    if (spec.kind == PB_STRING || spec.kind == PB_SYMBOL) {
        *path = spec.text;
        *suffix = path_leaf(spec.text);
        return path->len != 0 && suffix->len != 0;
    }
    if (spec.kind == PB_VECTOR && spec.seq.len == 2 &&
        (spec.seq.items[0].kind == PB_STRING || spec.seq.items[0].kind == PB_SYMBOL) &&
        (spec.seq.items[1].kind == PB_STRING || spec.seq.items[1].kind == PB_SYMBOL)) {
        *path = spec.seq.items[0].text;
        *suffix = spec.seq.items[1].text;
        return path->len != 0 && suffix->len != 0;
    }
    return false;
}

static bool publish_json_demux_one(pb_eval_ctx *ctx, yyjson_val *root, pb_value spec) {
    pb_slice path = {0};
    pb_slice suffix = {0};
    if (!demux_spec(spec, &path, &suffix)) {
        return false;
    }
    pb_eval_result scalar = json_scalar_value(ctx, json_lookup(root, path));
    if (scalar.err != PB_EVAL_OK) {
        return false;
    }
    if (scalar.value.kind == PB_NIL) {
        return true;
    }

    const size_t subject_len = ctx->subject.len + 1 + suffix.len;
    char *subject_ptr = pb_arena_alloc(ctx->arena, subject_len, 1);
    if (subject_ptr == NULL) {
        return false;
    }
    memcpy(subject_ptr, ctx->subject.ptr, ctx->subject.len);
    subject_ptr[ctx->subject.len] = '.';
    memcpy(subject_ptr + ctx->subject.len + 1, suffix.ptr, suffix.len);

    pb_slice payload = {0};
    if (!coerce_payload(ctx, scalar.value, &payload)) {
        return false;
    }
    return ctx->publish != NULL &&
           ctx->publish(ctx->publish_ctx, (pb_slice){.ptr = subject_ptr, .len = subject_len}, payload);
}

static pb_eval_result call_json_demux(pb_eval_ctx *ctx, pb_values args) {
    if (args.len < 2) {
        return fail(PB_EVAL_ARITY);
    }
    pb_slice payload = {0};
    if (!as_string(args.items[args.len - 1], &payload)) {
        return fail(PB_EVAL_TYPE);
    }
    yyjson_doc *doc = read_json_slice(ctx, payload);
    if (doc == NULL) {
        return ok((pb_value){.kind = PB_NIL});
    }

    bool ok_publish = true;
    if (args.len == 2 && args.items[0].kind == PB_VECTOR) {
        for (size_t i = 0; i < args.items[0].seq.len; i += 1) {
            if (!publish_json_demux_one(ctx, yyjson_doc_get_root(doc), args.items[0].seq.items[i])) {
                ok_publish = false;
                break;
            }
        }
    } else {
        for (size_t i = 0; i + 1 < args.len; i += 1) {
            if (!publish_json_demux_one(ctx, yyjson_doc_get_root(doc), args.items[i])) {
                ok_publish = false;
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    if (!ok_publish) {
        return fail(PB_EVAL_PUBLISH_FAILED);
    }
    return ok((pb_value){.kind = PB_NIL});
}

static pb_eval_result eval_list(pb_eval_ctx *ctx, pb_values call) {
    const pb_slice head = call.items[0].text;
    const pb_values raw_args = {.items = call.items + 1, .len = call.len - 1};
    const pb_builtin_entry *entry = find_builtin(head);
    if (entry == NULL) {
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }

    if (entry->special) {
        switch (entry->builtin) {
        case PB_BUILTIN_DO: return eval_do(ctx, raw_args);
        case PB_BUILTIN_IF: return eval_if(ctx, raw_args);
        case PB_BUILTIN_WHEN: return eval_when(ctx, raw_args);
        case PB_BUILTIN_AND: return eval_and(ctx, raw_args);
        case PB_BUILTIN_OR: return eval_or(ctx, raw_args);
        case PB_BUILTIN_THREAD: return eval_thread(ctx, raw_args);
        default: return fail(PB_EVAL_UNKNOWN_SYMBOL);
        }
    }

    pb_values args = {0};
    pb_eval_result er = eval_args(ctx, raw_args, &args);
    if (er.err != PB_EVAL_OK) {
        return er;
    }

    switch (entry->builtin) {
    case PB_BUILTIN_NOT: return call_not(args);
    case PB_BUILTIN_EQ: return call_eq(args);
    case PB_BUILTIN_GT: return call_cmp(args, CMP_GT);
    case PB_BUILTIN_LT: return call_cmp(args, CMP_LT);
    case PB_BUILTIN_GE: return call_cmp(args, CMP_GE);
    case PB_BUILTIN_LE: return call_cmp(args, CMP_LE);
    case PB_BUILTIN_ADD: return call_arith(args, ARITH_ADD);
    case PB_BUILTIN_SUB: return call_arith(args, ARITH_SUB);
    case PB_BUILTIN_MUL: return call_arith(args, ARITH_MUL);
    case PB_BUILTIN_DIV: return call_arith(args, ARITH_DIV);
    case PB_BUILTIN_STR_CONCAT: return call_str_concat(ctx, args);
    case PB_BUILTIN_CONTAINS: return call_contains(args);
    case PB_BUILTIN_STARTS_WITH: return call_affix(args, AFFIX_STARTS);
    case PB_BUILTIN_ENDS_WITH: return call_affix(args, AFFIX_ENDS);
    case PB_BUILTIN_SUBJECT_APPEND: return call_subject_append(ctx, args);
    case PB_BUILTIN_SUBJECT_TOKEN: return call_subject_token(ctx, args);
    case PB_BUILTIN_SUBJECT_WITH: return call_subject_with(ctx, args);
    case PB_BUILTIN_PUBLISH: return call_publish(ctx, args);
    case PB_BUILTIN_JSON_GET: return call_json_get(ctx, args);
    case PB_BUILTIN_JSON_DEMUX: return call_json_demux(ctx, args);
    case PB_BUILTIN_ROUND: return call_round(args);
    case PB_BUILTIN_CLAMP: return call_clamp(args);
    case PB_BUILTIN_MIN: return call_minmax(args, MINMAX_MIN);
    case PB_BUILTIN_MAX: return call_minmax(args, MINMAX_MAX);
    case PB_BUILTIN_ABS: return call_abs(args);
    case PB_BUILTIN_SIGN: return call_sign(args);
    case PB_BUILTIN_SQUELCH: return call_squelch(ctx, args);
    case PB_BUILTIN_DEADBAND: return call_deadband(ctx, args);
    case PB_BUILTIN_CHANGED: return call_changed(ctx, args);
    case PB_BUILTIN_DELTA: return call_delta(ctx, args);
    case PB_BUILTIN_COUNT: return call_count(ctx, args);
    case PB_BUILTIN_MOVING_AVG: return call_moving_avg(ctx, args);
    case PB_BUILTIN_DO:
    case PB_BUILTIN_IF:
    case PB_BUILTIN_WHEN:
    case PB_BUILTIN_AND:
    case PB_BUILTIN_OR:
    case PB_BUILTIN_THREAD:
        break;
    }
    return fail(PB_EVAL_UNKNOWN_SYMBOL);
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
