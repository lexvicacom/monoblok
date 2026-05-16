// Forms: now, not, =, >, <, >=, <=, +, -, *, /, round, quantize, clamp, min, max, abs, sign.
// Numeric, boolean, comparison, arithmetic, and wall-clock forms.
// Included by pb_forms.c; keep helpers static and chunk-local.

#include "pb_form_chunk.h"

static pb_eval_result call_now(pb_eval_ctx *ctx, pb_values args) {
    if (args.len != 1 || args.items[0].kind != PB_KEYWORD) {
        return fail(PB_EVAL_ARITY);
    }

    time_t sec = (time_t)(ctx->wall_ms / 1000);
    struct tm tm_utc = {0};
    if (gmtime_r(&sec, &tm_utc) == NULL) {
        return fail(PB_EVAL_TYPE);
    }

    char tmp[18];
    int n = 0;
    if (text_eq(args.items[0].text, "date")) {
        n = snprintf(tmp, sizeof tmp, "%04d-%02d-%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
    } else if (text_eq(args.items[0].text, "hour")) {
        n = snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour);
    } else if (text_eq(args.items[0].text, "minute")) {
        n = snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d%02d",
                     tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                     tm_utc.tm_hour, tm_utc.tm_min);
    } else {
        return fail(PB_EVAL_UNKNOWN_SYMBOL);
    }
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return fail(PB_EVAL_TYPE);
    }

    char *owned = pb_arena_memdup(ctx->arena, tmp, (size_t)n);
    if (owned == NULL) {
        return fail(PB_EVAL_OOM);
    }
    return ok((pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = (size_t)n}});
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
    if (!as_number(args.items[0], &places) ||
        !as_number(args.items[1], &x) ||
        places < 0 ||
        floor(places) != places) {
        return fail(PB_EVAL_TYPE);
    }
    const double factor = pow(10.0, places);
    return ok((pb_value){.kind = PB_NUMBER, .number = round(x * factor) / factor});
}

static pb_eval_result call_quantize(pb_values args) {
    if (args.len != 2) {
        return fail(PB_EVAL_ARITY);
    }
    double step = 0;
    double x = 0;
    if (!as_number(args.items[0], &step) || !as_number(args.items[1], &x) || step <= 0) {
        return fail(PB_EVAL_TYPE);
    }
    return ok((pb_value){.kind = PB_NUMBER, .number = round(x / step) * step});
}

static pb_eval_result call_clamp(pb_values args) {
    if (args.len != 3) {
        return fail(PB_EVAL_ARITY);
    }
    double lo = 0;
    double hi = 0;
    double x = 0;
    if (!as_number(args.items[0], &lo) ||
        !as_number(args.items[1], &hi) ||
        !as_number(args.items[2], &x)) {
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
