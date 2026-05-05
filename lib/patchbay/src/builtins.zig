//! Built-in operators for the patchbay DSL. Single dispatch entry is
//! `evalCall`; stateful ops go through `state.zig`'s slot helpers.

const std = @import("std");
const Allocator = std.mem.Allocator;
const subject_mod = @import("subject.zig");
const state = @import("state.zig");
const eval = @import("eval.zig");
const sexpr = @import("sexpr.zig");
const kernel = @import("kernel.zig");

const Value = state.Value;
const Context = eval.Context;
const EvalError = eval.EvalError;
const WindowKind = sexpr.WindowKind;
const WindowSpec = sexpr.WindowSpec;

const SpecialForm = enum { @"if", @"when", @"and", @"or", do, thread, transition };

const special_form_map = std.StaticStringMap(SpecialForm).initComptime(.{
    .{ "if", .@"if" },
    .{ "when", .@"when" },
    .{ "and", .@"and" },
    .{ "or", .@"or" },
    .{ "do", .do },
    .{ "->", .thread },
    .{ "transition", .transition },
});

const Op = enum {
    publish, publish_to, subject_append, str_concat,
    not, eq, gt, lt, ge, le,
    add, sub, mul, div,
    contains, starts_with, ends_with, subject_token, subject_with,
    now,
    round, quantize, clamp, min, max, abs, sign,
    squelch, deadband, changed, delta, hold_off,
    moving_avg, moving_sum, moving_max, moving_min,
    rate, percentile, median, stddev, variance, throttle,
    rising_edge, falling_edge,
    json_get, json_demux,
    bar,
    count,
    print,
};

const op_map = std.StaticStringMap(Op).initComptime(.{
    // `publish!` is the canonical terminal-emit form. `publish-to!` is a
    // deprecated alias kept only because args used to differ; semantics are
    // identical now (lenient: nil-skip + numeric coercion). Un-banged
    // spellings are TODO-removable aliases for in-tree patchbays.
    .{ "publish!", .publish_to },
    // TODO: remove `publish` alias once existing patchbays migrate to `publish!`.
    .{ "publish", .publish_to },
    // TODO: remove `publish-to!` alias; it predates the publish/publish-to merge.
    .{ "publish-to!", .publish_to },
    // TODO: remove `publish-to` alias once existing patchbays migrate to `publish!`.
    .{ "publish-to", .publish_to },
    .{ "subject-append", .subject_append },
    .{ "str-concat", .str_concat },
    .{ "not", .not },
    .{ "=", .eq },
    .{ ">", .gt },
    .{ "<", .lt },
    .{ ">=", .ge },
    .{ "<=", .le },
    .{ "+", .add },
    .{ "-", .sub },
    .{ "*", .mul },
    .{ "/", .div },
    .{ "contains?", .contains },
    .{ "starts-with?", .starts_with },
    .{ "ends-with?", .ends_with },
    .{ "subject-token", .subject_token },
    .{ "subject-with", .subject_with },
    .{ "now", .now },
    .{ "round", .round },
    .{ "quantize", .quantize },
    .{ "clamp", .clamp },
    .{ "min", .min },
    .{ "max", .max },
    .{ "abs", .abs },
    .{ "sign", .sign },
    .{ "squelch", .squelch },
    .{ "deadband", .deadband },
    .{ "changed?", .changed },
    .{ "delta", .delta },
    .{ "hold-off", .hold_off },
    .{ "rate", .rate },
    .{ "percentile", .percentile },
    .{ "median", .median },
    .{ "stddev", .stddev },
    .{ "variance", .variance },
    .{ "throttle", .throttle },
    .{ "moving-avg", .moving_avg },
    .{ "moving-sum", .moving_sum },
    .{ "moving-max", .moving_max },
    .{ "moving-min", .moving_min },
    .{ "rising-edge", .rising_edge },
    .{ "falling-edge", .falling_edge },
    .{ "json-get", .json_get },
    .{ "json-demux!", .json_demux },
    // TODO: remove `json-demux` alias once existing patchbays migrate to `json-demux!`.
    .{ "json-demux", .json_demux },
    .{ "bar!", .bar },
    // TODO: remove `bar` alias once existing patchbays migrate to `bar!`.
    .{ "bar", .bar },
    .{ "count!", .count },
    // TODO: remove `count` alias once existing patchbays migrate to `count!`.
    .{ "count", .count },
    // `print!` is a debug aid; the loader counts these and warns at
    // startup so a left-in `print!` is visible in the boot log.
    .{ "print!", .print },
});

pub fn evalCall(ctx: *Context, items: []const Value) EvalError!Value {
    if (items.len == 0) return .nil;
    if (items[0] != .symbol) return error.TypeMismatch;
    const op = items[0].symbol;
    const args = items[1..];

    if (special_form_map.get(op)) |sf| return switch (sf) {
        .@"if" => evalIf(ctx, args),
        .@"when" => evalWhen(ctx, args),
        .@"and" => evalAnd(ctx, args),
        .@"or" => evalOr(ctx, args),
        .do => evalDo(ctx, args),
        .thread => evalThread(ctx, args),
        .transition => evalTransition(ctx, args),
    };

    const tag = op_map.get(op) orelse return error.UnknownSymbol;

    // <= 8 args is the common case; arena fallback for wide forms.
    var stack_buf: [8]Value = undefined;
    const evaled = if (args.len <= stack_buf.len)
        stack_buf[0..args.len]
    else
        try ctx.arena.alloc(Value, args.len);
    for (args, 0..) |a, i| evaled[i] = try ctx.eval_fn(ctx, a);

    return switch (tag) {
        .publish => callPublish(ctx, evaled),
        .publish_to => callPublishTo(ctx, evaled),
        .subject_append => callSubjectAppend(ctx, evaled),
        .str_concat => callStrConcat(ctx, evaled),
        .not => callNot(evaled),
        .eq => callEq(evaled),
        .gt => callCmp(evaled, .gt),
        .lt => callCmp(evaled, .lt),
        .ge => callCmp(evaled, .ge),
        .le => callCmp(evaled, .le),
        .add => callArith(evaled, .add),
        .sub => callArith(evaled, .sub),
        .mul => callArith(evaled, .mul),
        .div => callArith(evaled, .div),
        .contains => callContains(evaled),
        .starts_with => callStartsEnds(evaled, .starts),
        .ends_with => callStartsEnds(evaled, .ends),
        .subject_token => callSubjectToken(ctx, evaled),
        .subject_with => callSubjectWith(ctx, evaled),
        .now => callNow(ctx, evaled),
        .round => callRound(evaled),
        .quantize => callQuantize(evaled),
        .clamp => callClamp(evaled),
        .min => callMinMax(evaled, .min),
        .max => callMinMax(evaled, .max),
        .abs => callAbs(evaled),
        .sign => callSign(evaled),
        .squelch => callSquelch(ctx, evaled),
        .deadband => callDeadband(ctx, evaled),
        .changed => callChanged(ctx, evaled),
        .delta => callDelta(ctx, evaled),
        .hold_off => callHoldOff(ctx, evaled),
        .moving_avg => callMoving(ctx, evaled, .avg),
        .moving_sum => callMoving(ctx, evaled, .sum),
        .moving_max => callMoving(ctx, evaled, .max),
        .moving_min => callMoving(ctx, evaled, .min),
        .rate => callRate(ctx, evaled),
        .percentile => callPercentile(ctx, evaled),
        .median => callMedian(ctx, evaled),
        .stddev => callStdVar(ctx, evaled, .stddev),
        .variance => callStdVar(ctx, evaled, .variance),
        .throttle => callThrottle(ctx, evaled),
        .rising_edge => callEdge(ctx, evaled, .rising),
        .falling_edge => callEdge(ctx, evaled, .falling),
        .json_get => callJsonGet(ctx, evaled),
        .json_demux => callJsonDemux(ctx, evaled),
        .bar => callBar(ctx, evaled),
        .count => callCount(ctx, evaled),
        .print => callPrint(ctx, evaled),
    };
}

// --- Special forms ------------------------------------------------------

fn evalIf(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 2 or args.len > 3) return error.ArityMismatch;
    const cond = try ctx.eval_fn(ctx, args[0]);
    if (cond.isTruthy()) return ctx.eval_fn(ctx, args[1]);
    if (args.len == 3) return ctx.eval_fn(ctx, args[2]);
    return .nil;
}

fn evalWhen(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    const cond = try ctx.eval_fn(ctx, args[0]);
    if (!cond.isTruthy()) return .nil;
    var last: Value = .nil;
    for (args[1..]) |a| last = try ctx.eval_fn(ctx, a);
    return last;
}

fn evalAnd(ctx: *Context, args: []const Value) EvalError!Value {
    var last: Value = .{ .boolean = true };
    for (args) |a| {
        last = try ctx.eval_fn(ctx, a);
        if (!last.isTruthy()) return last;
    }
    return last;
}

fn evalOr(ctx: *Context, args: []const Value) EvalError!Value {
    var last: Value = .{ .boolean = false };
    for (args) |a| {
        last = try ctx.eval_fn(ctx, a);
        if (last.isTruthy()) return last;
    }
    return last;
}

fn evalDo(ctx: *Context, args: []const Value) EvalError!Value {
    var last: Value = .nil;
    for (args) |a| last = try ctx.eval_fn(ctx, a);
    return last;
}

/// `(-> X f1 f2 ...)` threads X as the LAST argument of each form. A bare
/// symbol `f` is treated as `(f)`.
fn evalThread(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len == 0) return error.ArityMismatch;
    var acc = try ctx.eval_fn(ctx, args[0]);
    for (args[1..]) |form| {
        const call_items = switch (form) {
            .list => |items| items,
            .symbol => &[_]Value{form},
            else => return error.TypeMismatch,
        };
        if (call_items.len == 0 or call_items[0] != .symbol) return error.TypeMismatch;
        const rebuilt = try ctx.arena.alloc(Value, call_items.len + 1);
        @memcpy(rebuilt[0..call_items.len], call_items);
        rebuilt[call_items.len] = acc;
        acc = try ctx.eval_fn(ctx, .{ .list = rebuilt });
    }
    return acc;
}

/// `(transition BOOL RISING-BRANCH FALLING-BRANCH)`. Evaluates one branch
/// on the matching edge; nil on first sight or no-change. Distinct state
/// key from rising-edge/falling-edge.
fn evalTransition(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 3) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const now = (try ctx.eval_fn(ctx, args[0])).isTruthy();

    const key = try state.keyForOp(ctx.arena, "transition", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    const prev = slot.found_existing and slot.value_ptr.number != 0;
    var k: kernel.Transition = .{ .last = prev, .seen = slot.found_existing };
    const edge = k.update(now);
    slot.value_ptr.* = .{ .number = if (k.last) 1 else 0 };
    switch (edge) {
        .rising => return ctx.eval_fn(ctx, args[1]),
        .falling => return ctx.eval_fn(ctx, args[2]),
        .none => {
            rule.publishes_suppressed += 1;
            return .nil;
        },
    }
}

// --- Side-effecting publish ops -----------------------------------------

// TODO: dead since the publish/publish-to merge (every spelling now routes
// to callPublishTo). Delete once we're sure no out-of-tree caller imports it.
fn callPublish(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const subj = try state.asString(args[0]);
    const payload = try state.asString(args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    try ctx.emit(subj, payload);
    return .nil;
}

/// `(publish! SUBJECT VALUE)` (also `publish`, `publish-to!`, `publish-to`).
/// Validates SUBJECT, coerces VALUE to a payload string (numbers stringified
/// canonically, booleans → "true"/"false"), enqueues. No-op on nil VALUE so
/// an upstream gate suppression self-terminates the chain.
fn callPublishTo(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    if (args[1] == .nil) return .nil;
    const subj = try state.asString(args[0]);
    const payload = try coercePayload(ctx.arena, args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    try ctx.emit(subj, payload);
    return .nil;
}

fn coercePayload(arena: Allocator, v: Value) EvalError![]const u8 {
    return switch (v) {
        .string, .symbol => |s| s,
        .number => |n| try std.fmt.allocPrint(arena, "{d}", .{n}),
        .boolean => |b| if (b) "true" else "false",
        .nil => "",
        .list, .vector, .keyword => error.TypeMismatch,
    };
}

// --- String / subject ops -----------------------------------------------

fn callSubjectAppend(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const suffix = try state.asString(args[0]);
    const joined = try std.fmt.allocPrint(ctx.arena, "{s}.{s}", .{ ctx.subject, suffix });
    return .{ .string = joined };
}

fn callStrConcat(ctx: *Context, args: []const Value) EvalError!Value {
    var total: usize = 0;
    for (args) |a| total += (try state.asString(a)).len;
    const buf = try ctx.arena.alloc(u8, total);
    var i: usize = 0;
    for (args) |a| {
        const s = try state.asString(a);
        @memcpy(buf[i..][0..s.len], s);
        i += s.len;
    }
    return .{ .string = buf };
}

fn callContains(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    // `(contains? COLL ITEM)`: substring on strings, membership on vectors.
    // Order matches `->` last-arg threading: the threaded value lands as
    // ITEM in `(-> x (contains? coll))`.
    if (args[0] == .vector) {
        for (args[0].vector) |item| {
            if (state.valueEql(item, args[1])) return .{ .boolean = true };
        }
        return .{ .boolean = false };
    }
    const hay = try state.asString(args[0]);
    const needle = try state.asString(args[1]);
    return .{ .boolean = std.mem.indexOf(u8, hay, needle) != null };
}

const AffixKind = enum { starts, ends };

fn callStartsEnds(args: []const Value, kind: AffixKind) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const hay = try state.asString(args[0]);
    const needle = try state.asString(args[1]);
    return .{ .boolean = switch (kind) {
        .starts => std.mem.startsWith(u8, hay, needle),
        .ends => std.mem.endsWith(u8, hay, needle),
    } };
}

/// `(subject-with TOK ...)` / `(subject-with [TOK ...])`. Joins tokens
/// with `.` to form a publishable subject. Tokens are coerced
/// (numbers stringified canonically, booleans to `"true"`/`"false"`);
/// empty tokens and the result are validated against publish rules.
fn callSubjectWith(ctx: *Context, args: []const Value) EvalError!Value {
    // Vector form: `(subject-with [a b c])` is sugar for `(subject-with a b c)`.
    const toks: []const Value = if (args.len == 1 and args[0] == .vector)
        args[0].vector
    else
        args;
    if (toks.len == 0) return error.ArityMismatch;

    var total: usize = toks.len - 1; // dots between tokens
    var encoded = try ctx.arena.alloc([]const u8, toks.len);
    for (toks, 0..) |t, i| {
        const s = try coercePayload(ctx.arena, t);
        if (s.len == 0) return error.InvalidSubject;
        encoded[i] = s;
        total += s.len;
    }

    const buf = try ctx.arena.alloc(u8, total);
    var off: usize = 0;
    for (encoded, 0..) |s, i| {
        if (i > 0) {
            buf[off] = '.';
            off += 1;
        }
        @memcpy(buf[off..][0..s.len], s);
        off += s.len;
    }
    subject_mod.validatePublish(buf) catch return error.InvalidSubject;
    return .{ .string = buf };
}

/// `(now :date)` / `(now :hour)` / `(now :minute)` returns a UTC
/// wall-clock string built from `ctx.wall_ms`:
///
///   :date    -> "YYYY-MM-DD"        (10 chars)
///   :hour    -> "YYYY-MM-DDTHH"     (13 chars, RFC 3339 T separator)
///   :minute  -> "YYYY-MM-DDTHHMM"   (15 chars, RFC 3339 basic form)
///
/// Subject-token-safe: no `:` (the RFC extended form would include one),
/// just letters / digits / dashes. Useful for date/hour bucketing:
/// `(publish! (subject-with "logs" (now :hour) "errors") payload)`.
///
/// The three granularities share one threadlocal buffer keyed by minute;
/// recompute on a minute boundary, slice on every call. Cost on a hit
/// is one integer compare and a slice. Single-threaded server, no
/// locking. The returned slice is valid until the next call that
/// crosses a minute boundary; rules consume it immediately so this
/// isn't a real-world hazard.
///
/// `:minute` granularity is high cardinality (525,600 unique values per
/// year per topic) when used as a subject token. Prefer `:hour` or
/// `:date` unless you really mean it.
fn callNow(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    if (args[0] != .keyword) return error.TypeMismatch;
    const flag = args[0].keyword;

    const len: usize = if (std.mem.eql(u8, flag, "date"))
        10
    else if (std.mem.eql(u8, flag, "hour"))
        13
    else if (std.mem.eql(u8, flag, "minute"))
        15
    else
        return error.UnknownSymbol;

    const buf = formatCachedMinute(ctx.wall_ms);
    return .{ .string = buf[0..len] };
}

const ms_per_day: i64 = 86_400_000;
const ms_per_minute: i64 = 60_000;

const minute_sentinel: i64 = std.math.minInt(i64);

// Single shared buffer for all three granularities. Minute strictly
// contains hour strictly contains date, so :date is buf[0..10], :hour
// is buf[0..13], :minute is buf[0..15]. Recompute only on minute
// boundary. Buffer carries a couple of bytes of slack: bufPrint under
// Zig 0.16 reports NoSpaceLeft on an exact-fit write.
threadlocal var minute_cache: struct {
    minute: i64 = minute_sentinel,
    buf: [20]u8 = undefined,
} = .{};

fn formatCachedMinute(wall_ms: i64) []const u8 {
    const minute_idx = @divFloor(wall_ms, ms_per_minute);
    if (minute_cache.minute != minute_idx) {
        const day = @divFloor(wall_ms, ms_per_day);
        const ymd = civilFromDays(day);
        // Cast year to u32 to avoid Zig 0.16's signed-print leading `+`.
        // Years before 1 AD wrap, but we don't support those (subjects
        // are for live data, not historical dating).
        const year: u32 = @intCast(ymd.y);
        const minute_of_day: u32 = @intCast(@mod(minute_idx, 24 * 60));
        const hour: u8 = @intCast(minute_of_day / 60);
        const minute: u8 = @intCast(minute_of_day % 60);
        _ = std.fmt.bufPrint(
            &minute_cache.buf,
            "{d:0>4}-{d:0>2}-{d:0>2}T{d:0>2}{d:0>2}",
            .{ year, ymd.m, ymd.d, hour, minute },
        ) catch unreachable;
        minute_cache.minute = minute_idx;
    }
    return minute_cache.buf[0..];
}

/// Howard Hinnant's `civil_from_days`: integer-only conversion from
/// "days since 1970-01-01 (UTC)" to (year, month, day). Proleptic
/// Gregorian, no DST or timezone awareness.
const Ymd = struct { y: i32, m: u8, d: u8 };

fn civilFromDays(z_in: i64) Ymd {
    const z = z_in + 719468;
    const era: i64 = if (z >= 0) @divFloor(z, 146097) else @divFloor(z - 146096, 146097);
    const doe: u32 = @intCast(z - era * 146097); // [0, 146096]
    const yoe: u32 = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const y: i64 = @as(i64, yoe) + era * 400;
    const doy: u32 = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const mp: u32 = (5 * doy + 2) / 153; // [0, 11]
    const d: u32 = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
    const m: u32 = if (mp < 10) mp + 3 else mp - 9; // [1, 12]
    const year: i64 = if (m <= 2) y + 1 else y;
    return .{ .y = @intCast(year), .m = @intCast(m), .d = @intCast(d) };
}

/// `(subject-token N)` / `(subject-token N S)`. 0-indexed; nil if out of range.
fn callSubjectToken(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 1 or args.len > 2) return error.ArityMismatch;
    const n_f = try state.asNumber(args[0]);
    if (n_f < 0) return error.TypeMismatch;
    const n: usize = @intFromFloat(n_f);
    const s = if (args.len == 2) try state.asString(args[1]) else ctx.subject;
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |tok| : (i += 1) {
        if (i == n) return .{ .string = tok };
    }
    return .nil;
}

// --- Boolean / comparison / arithmetic ----------------------------------

fn callNot(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    return .{ .boolean = !args[0].isTruthy() };
}

fn callEq(args: []const Value) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    const first = args[0];
    for (args[1..]) |a| if (!state.valueEql(first, a)) return .{ .boolean = false };
    return .{ .boolean = true };
}

const Cmp = enum { gt, lt, ge, le };

fn callCmp(args: []const Value, cmp: Cmp) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    var i: usize = 0;
    while (i + 1 < args.len) : (i += 1) {
        const a = try state.asNumber(args[i]);
        const b = try state.asNumber(args[i + 1]);
        const ok = switch (cmp) {
            .gt => a > b,
            .lt => a < b,
            .ge => a >= b,
            .le => a <= b,
        };
        if (!ok) return .{ .boolean = false };
    }
    return .{ .boolean = true };
}

const Arith = enum { add, sub, mul, div };

fn callArith(args: []const Value, op: Arith) EvalError!Value {
    if (args.len == 0) return error.ArityMismatch;
    var acc = try state.asNumber(args[0]);
    if (args.len == 1) {
        return .{ .number = switch (op) {
            .add, .mul => acc,
            .sub => -acc,
            .div => 1.0 / acc,
        } };
    }
    for (args[1..]) |a| {
        const x = try state.asNumber(a);
        acc = switch (op) {
            .add => acc + x,
            .sub => acc - x,
            .mul => acc * x,
            .div => acc / x,
        };
    }
    return .{ .number = acc };
}

// --- Numeric transforms -------------------------------------------------

/// `(round N X)`, X rounded to N decimal places.
fn callRound(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const decimals = try state.asNumber(args[0]);
    const x = try state.asNumber(args[1]);
    if (decimals < 0 or decimals > 15) return error.TypeMismatch;
    const scale = std.math.pow(f64, 10.0, decimals);
    return .{ .number = @round(x * scale) / scale };
}

/// `(quantize STEP X)`, X snapped to the nearest multiple of STEP.
fn callQuantize(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const step = try state.asNumber(args[0]);
    const x = try state.asNumber(args[1]);
    if (step == 0) return error.TypeMismatch;
    return .{ .number = kernel.quantize(step, x) };
}

/// `(clamp LO HI X)`, X clipped to [LO, HI]. Value-last for threading.
fn callClamp(args: []const Value) EvalError!Value {
    if (args.len != 3) return error.ArityMismatch;
    const lo = try state.asNumber(args[0]);
    const hi = try state.asNumber(args[1]);
    const x = try state.asNumber(args[2]);
    if (lo > hi) return error.TypeMismatch;
    return .{ .number = kernel.clamp(lo, hi, x) };
}

const MinMaxKind = enum { min, max };

fn callMinMax(args: []const Value, kind: MinMaxKind) EvalError!Value {
    if (args.len == 0) return error.ArityMismatch;
    var acc = try state.asNumber(args[0]);
    for (args[1..]) |a| {
        const x = try state.asNumber(a);
        acc = switch (kind) {
            .min => @min(acc, x),
            .max => @max(acc, x),
        };
    }
    return .{ .number = acc };
}

fn callAbs(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    return .{ .number = @abs(try state.asNumber(args[0])) };
}

fn callSign(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const x = try state.asNumber(args[0]);
    return .{ .number = if (x > 0) 1.0 else if (x < 0) -1.0 else 0.0 };
}

// --- Stateful gates -----------------------------------------------------

/// `(squelch X)`. Passes X through iff it differs from the last X seen.
/// First sight passes. Returns nil on suppress.
fn callSquelch(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const changed = try squelchChanged(ctx, rule, "squelch", args[0]);
    if (!changed) rule.publishes_suppressed += 1;
    return if (changed) args[0] else .nil;
}

/// `(deadband DELTA X)`. Passes X through iff it differs from the last
/// accepted X by >= DELTA. Suppresses sub-DELTA noise.
fn callDeadband(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const delta = try state.asNumber(args[0]);
    const x = try state.asNumber(args[1]);
    if (delta < 0) return error.TypeMismatch;

    const key = try state.keyForOp(ctx.arena, "deadband", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    var k: kernel.Deadband = .{
        .threshold = delta,
        .last = if (slot.found_existing) slot.value_ptr.number else 0,
        .seen = slot.found_existing,
    };
    const out = k.update(x);
    slot.value_ptr.* = .{ .number = k.last };
    if (out == null) {
        rule.publishes_suppressed += 1;
        return .nil;
    }
    return .{ .number = out.? };
}

/// `(changed? X)`. Boolean version of `squelch`; first sight returns true.
fn callChanged(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const changed = try squelchChanged(ctx, rule, "changed?", args[0]);
    if (!changed) rule.publishes_suppressed += 1;
    return .{ .boolean = changed };
}

/// Shared transition for `squelch` / `changed?`. Numeric input + numeric (or
/// fresh) slot uses the kernel f64 fast path stored in `.number`. Anything
/// else (non-numeric input, or slot already in `.bytes` from a prior
/// non-numeric call) falls back to the canonical-bytes path so `1 == 1.0`
/// across types. A type flip flushes the slot to the new variant.
fn squelchChanged(
    ctx: *Context,
    rule: *state.Rule,
    op_name: []const u8,
    v: Value,
) EvalError!bool {
    const key = try state.keyForOp(ctx.arena, op_name, ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);

    if (v == .number) {
        const fresh = !slot.found_existing or slot.value_ptr.* == .empty;
        const numeric_slot = fresh or slot.value_ptr.* == .number;
        if (numeric_slot) {
            var k: kernel.Squelch = .{
                .last = if (fresh) 0 else slot.value_ptr.number,
                .seen = !fresh,
            };
            const out = k.update(v.number);
            slot.value_ptr.* = .{ .number = k.last };
            return out != null;
        }
        // Slot is .bytes from a prior non-numeric call; flush to .number
        // and treat the type flip as a change.
        slot.value_ptr.deinit(ctx.gpa);
        slot.value_ptr.* = .{ .number = v.number };
        return true;
    }

    const encoded = try state.encodeForState(ctx.arena, v);
    if (!slot.found_existing or slot.value_ptr.* == .empty) {
        var buf: std.ArrayList(u8) = .empty;
        try buf.appendSlice(ctx.gpa, encoded);
        slot.value_ptr.* = .{ .bytes = buf };
        return true;
    }
    if (slot.value_ptr.* != .bytes) {
        // Slot was numeric; non-numeric input is a change. Flip variant.
        slot.value_ptr.* = .empty;
        var buf: std.ArrayList(u8) = .empty;
        try buf.appendSlice(ctx.gpa, encoded);
        slot.value_ptr.* = .{ .bytes = buf };
        return true;
    }
    const prev = &slot.value_ptr.bytes;
    if (std.mem.eql(u8, prev.items, encoded)) return false;
    prev.clearRetainingCapacity();
    try prev.appendSlice(ctx.gpa, encoded);
    return true;
}

/// `(delta X)`. X minus the last X. First sight returns 0.
fn callDelta(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const x = try state.asNumber(args[0]);
    const key = try state.keyForOp(ctx.arena, "delta", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    var k: kernel.Delta = .{
        .last = if (slot.found_existing) slot.value_ptr.number else 0,
        .seen = slot.found_existing,
    };
    const out = k.update(x);
    slot.value_ptr.* = .{ .number = k.last };
    return .{ .number = out };
}

/// `(hold-off MS X)`. Passes X on first sight and on any call arriving
/// >= MS ms after the previous pass; nil otherwise. Time source is
/// `ctx.now_ms` (stamped once per ingress).
fn callHoldOff(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const ms = try state.asNumber(args[0]);
    if (ms < 0) return error.TypeMismatch;
    const now: f64 = @floatFromInt(ctx.now_ms);

    const key = try state.keyForOp(ctx.arena, "hold-off", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    var k: kernel.HoldOff = .{
        .interval_ms = ms,
        .last_ms = if (slot.found_existing) slot.value_ptr.number else 0,
        .seen = slot.found_existing,
    };
    const passed = k.update(now);
    slot.value_ptr.* = .{ .number = k.last_ms };
    if (!passed) {
        rule.publishes_suppressed += 1;
        return .nil;
    }
    return args[1];
}

const EdgeKind = enum { rising, falling };

/// `(rising-edge X)` / `(falling-edge X)`. X on the matching boolean
/// transition, nil otherwise; first sight is nil.
fn callEdge(ctx: *Context, args: []const Value, kind: EdgeKind) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const now = args[0].isTruthy();

    const op_name = switch (kind) {
        .rising => "rising-edge",
        .falling => "falling-edge",
    };
    const key = try state.keyForOp(ctx.arena, op_name, ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    const prev = slot.found_existing and slot.value_ptr.number != 0;
    const fired = switch (kind) {
        .rising => blk: {
            var k: kernel.RisingEdge = .{ .last = prev, .seen = slot.found_existing };
            const f = k.update(now);
            slot.value_ptr.* = .{ .number = if (k.last) 1 else 0 };
            break :blk f;
        },
        .falling => blk: {
            var k: kernel.FallingEdge = .{ .last = prev, .seen = slot.found_existing };
            const f = k.update(now);
            slot.value_ptr.* = .{ .number = if (k.last) 1 else 0 };
            break :blk f;
        },
    };
    if (!fired) rule.publishes_suppressed += 1;
    return if (fired) args[0] else .nil;
}

// --- Window descriptors -------------------------------------------------

/// Parse a window argument starting at `args[idx]`. Two source shapes:
///   - bare number  → ticks (e.g. `(moving-avg 10 X)`)
///   - `:ms N`      → time, N milliseconds (e.g. `(moving-avg :ms 5000 X)`)
/// Returns the parsed spec plus the number of arg slots consumed.
fn takeWindow(args: []const Value, idx: usize) EvalError!struct {
    spec: WindowSpec,
    consumed: usize,
} {
    if (idx >= args.len) return error.ArityMismatch;
    const head = args[idx];
    if (head == .keyword) {
        if (!std.mem.eql(u8, head.keyword, "ms")) return error.TypeMismatch;
        if (idx + 1 >= args.len) return error.ArityMismatch;
        const n_f = try state.asNumber(args[idx + 1]);
        if (n_f < 1) return error.TypeMismatch;
        return .{
            .spec = .{ .kind = .time_ms, .n = @intFromFloat(n_f) },
            .consumed = 2,
        };
    }
    const n_f = try state.asNumber(head);
    if (n_f < 1) return error.TypeMismatch;
    return .{
        .spec = .{ .kind = .ticks, .n = @intFromFloat(n_f) },
        .consumed = 1,
    };
}

// --- Windowed aggregates ------------------------------------------------

const MovingKind = enum { avg, sum, max, min };

/// `(moving-{avg,sum,max,min} N X)`: fixed-cap ring over the last N
/// samples. `(moving-{avg,sum,max,min} :ms N X)`: time-evicted ring over
/// the last N ms. The ring allocates on first sight and is keyed per
/// (rule, op, subject, window-kind), so a rule that uses both forms gets
/// distinct slots. First call's aggregate is over that one sample.
fn callMoving(ctx: *Context, args: []const Value, kind: MovingKind) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 1) return error.ArityMismatch;
    const x = try state.asNumber(args[win.consumed]);

    const op_name = switch (kind) {
        .avg => "moving-avg",
        .sum => "moving-sum",
        .max => "moving-max",
        .min => "moving-min",
    };
    const key = try state.keyForWindow(ctx.arena, op_name, mapWinKind(win.spec.kind), ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);

    switch (win.spec.kind) {
        .ticks => {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .ring = try state.Ring.init(ctx.gpa, win.spec.n) };
            }
            const ring = &slot.value_ptr.ring;
            try ring.push(ctx.gpa, x);
            return .{ .number = switch (kind) {
                .avg => ring.mean(),
                .sum => ring.sum,
                .max => ring.max(),
                .min => ring.min(),
            } };
        },
        .time_ms => {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .time_ring = state.TimeRing.init(win.spec.n) };
            }
            const ring = &slot.value_ptr.time_ring;
            try ring.push(ctx.gpa, x, ctx.now_ms);
            ctx.notifyClockSlot(rule, slot.key_ptr);
            return .{ .number = switch (kind) {
                .avg => ring.mean(),
                .sum => ring.sum(),
                .max => ring.max(),
                .min => ring.min(),
            } };
        },
    }
}

// --- Window helpers (shared by rate / percentile / stddev / throttle) ---

/// Map the eval-side WindowKind onto state's KeyKind. The two enums have
/// the same shape but live in different modules.
fn mapWinKind(k: WindowKind) state.KeyKind {
    return switch (k) {
        .ticks => .ticks,
        .time_ms => .time_ms,
    };
}

/// Snapshot the live window's f64 values into an arena-owned slice.
/// Caller owns nothing extra; both slot variants are visited.
fn collectWindow(arena: Allocator, slot_value: state.StateEntry) ![]f64 {
    return switch (slot_value) {
        .ring => |r| blk: {
            const n = r.len();
            const out = try arena.alloc(f64, n);
            const cap = r.buf.len;
            // Logical samples are buf[(counter - n + i) % cap].
            const start = r.counter - @as(u64, n);
            var i: usize = 0;
            while (i < n) : (i += 1) {
                out[i] = r.buf[@intCast((start + @as(u64, i)) % cap)];
            }
            break :blk out;
        },
        .time_ring => |r| blk: {
            const out = try arena.alloc(f64, r.samples.items.len);
            for (r.samples.items, 0..) |s, i| out[i] = s.value;
            break :blk out;
        },
        else => &[_]f64{},
    };
}

/// Push X into a (rule, op, subject, window-kind) ring, allocating the
/// slot if needed. Returns a pointer to the slot's StateEntry so callers
/// can read aggregates back.
fn pushIntoWindow(
    ctx: *Context,
    rule: *state.Rule,
    op_name: []const u8,
    window: WindowSpec,
    x: f64,
) !*state.StateEntry {
    const key = try state.keyForWindow(ctx.arena, op_name, mapWinKind(window.kind), ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    switch (window.kind) {
        .ticks => {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .ring = try state.Ring.init(ctx.gpa, window.n) };
            }
            try slot.value_ptr.ring.push(ctx.gpa, x);
        },
        .time_ms => {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .time_ring = state.TimeRing.init(window.n) };
            }
            try slot.value_ptr.time_ring.push(ctx.gpa, x, ctx.now_ms);
            ctx.notifyClockSlot(rule, slot.key_ptr);
        },
    }
    return slot.value_ptr;
}

// --- Rate ---------------------------------------------------------------

/// `(rate :ms N X)`. Events per second over the last N ms. Counts pushes;
/// X's value is ignored but evaluated (so it threads through `->`).
/// Tick-window form is rejected: "rate over the last N samples" has no
/// time unit.
fn callRate(ctx: *Context, args: []const Value) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 1) return error.ArityMismatch;
    if (win.spec.kind != .time_ms) return error.TypeMismatch;
    // Push a placeholder so the time_ring's eviction reflects our event.
    const slot_value = try pushIntoWindow(ctx, rule, "rate", win.spec, 0);
    const n = slot_value.time_ring.len();
    const seconds = @as(f64, @floatFromInt(win.spec.n)) / 1000.0;
    return .{ .number = @as(f64, @floatFromInt(n)) / seconds };
}

// --- Percentile / median ------------------------------------------------

/// `(percentile WINDOW P X)`. WINDOW is `N` (ticks) or `:ms N` (time).
/// P in [0, 1]. Linear interpolation between neighbours. O(n log n) per
/// call (sort copy).
fn callPercentile(ctx: *Context, args: []const Value) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 2) return error.ArityMismatch;
    const p = try state.asNumber(args[win.consumed]);
    if (p < 0 or p > 1) return error.TypeMismatch;
    const x = try state.asNumber(args[win.consumed + 1]);
    const slot_value = try pushIntoWindow(ctx, rule, "percentile", win.spec, x);
    return .{ .number = try percentileOf(ctx.arena, slot_value.*, p) };
}

/// `(median WINDOW X)`. Sugar for `(percentile WINDOW 0.5 X)`.
fn callMedian(ctx: *Context, args: []const Value) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 1) return error.ArityMismatch;
    const x = try state.asNumber(args[win.consumed]);
    const slot_value = try pushIntoWindow(ctx, rule, "median", win.spec, x);
    return .{ .number = try percentileOf(ctx.arena, slot_value.*, 0.5) };
}

fn percentileOf(arena: Allocator, slot_value: state.StateEntry, p: f64) EvalError!f64 {
    const buf = try collectWindow(arena, slot_value);
    if (buf.len == 0) return 0;
    std.mem.sort(f64, buf, {}, comptime std.sort.asc(f64));
    if (buf.len == 1) return buf[0];
    const idx = p * @as(f64, @floatFromInt(buf.len - 1));
    const lo: usize = @intFromFloat(@floor(idx));
    const hi: usize = @intFromFloat(@ceil(idx));
    if (lo == hi) return buf[lo];
    const frac = idx - @floor(idx);
    return buf[lo] + (buf[hi] - buf[lo]) * frac;
}

// --- Stddev / variance --------------------------------------------------

const StdVarKind = enum { stddev, variance };

/// `(stddev WINDOW X)` / `(variance WINDOW X)`. WINDOW is `N` (ticks) or
/// `:ms N` (time). Population variance (divides by n, not n-1). Two-pass
/// over the live window.
fn callStdVar(ctx: *Context, args: []const Value, kind: StdVarKind) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 1) return error.ArityMismatch;
    const x = try state.asNumber(args[win.consumed]);
    const op_name: []const u8 = switch (kind) {
        .stddev => "stddev",
        .variance => "variance",
    };
    const key = try state.keyForWindow(ctx.arena, op_name, mapWinKind(win.spec.kind), ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    switch (win.spec.kind) {
        .ticks => {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .ring = try state.Ring.init(ctx.gpa, win.spec.n) };
            }
            try slot.value_ptr.ring.push(ctx.gpa, x);
        },
        .time_ms => {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .time_ring = state.TimeRing.init(win.spec.n) };
            }
            try slot.value_ptr.time_ring.push(ctx.gpa, x, ctx.now_ms);
            ctx.notifyClockSlot(rule, slot.key_ptr);
        },
    }
    const buf = try collectWindow(ctx.arena, slot.value_ptr.*);
    if (buf.len == 0) return .{ .number = 0 };
    var mean: f64 = 0;
    for (buf) |v| mean += v;
    mean /= @as(f64, @floatFromInt(buf.len));
    var sumsq: f64 = 0;
    for (buf) |v| {
        const d = v - mean;
        sumsq += d * d;
    }
    const variance = sumsq / @as(f64, @floatFromInt(buf.len));
    return .{ .number = switch (kind) {
        .variance => variance,
        .stddev => @sqrt(variance),
    } };
}

// --- Throttle -----------------------------------------------------------

/// `(throttle WINDOW MAX X)`. WINDOW is `N` (ticks) or `:ms N` (time).
/// Pass X through iff fewer than MAX events have already passed within
/// WINDOW (inclusive of this evaluation). Tracks pass timestamps in the
/// same `time_ring` machinery time-windowed `moving-*` uses. Differs from
/// `hold-off`: `hold-off` is "min interval between passes" (one timer);
/// `throttle` is "max count per window" (a sliding bucket of timestamps).
fn callThrottle(ctx: *Context, args: []const Value) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 2) return error.ArityMismatch;
    const max_f = try state.asNumber(args[win.consumed]);
    if (max_f < 1) return error.TypeMismatch;
    const max: usize = @intFromFloat(max_f);
    const x = args[win.consumed + 1];

    const key = try state.keyForWindow(ctx.arena, "throttle", mapWinKind(win.spec.kind), ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);

    const passed: bool = switch (win.spec.kind) {
        .ticks => blk: {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .ring = try state.Ring.init(ctx.gpa, win.spec.n) };
            }
            const ring = &slot.value_ptr.ring;
            // Tick form: count the 1.0 entries among the live samples.
            // Push 1.0 on pass, 0.0 on suppress so eviction is implicit
            // when the ring rolls.
            const fill = ring.len();
            const cap = ring.buf.len;
            const start = ring.counter - @as(u64, fill);
            var passes_in_window: usize = 0;
            var i: usize = 0;
            while (i < fill) : (i += 1) {
                if (ring.buf[@intCast((start + @as(u64, i)) % cap)] == 1.0) {
                    passes_in_window += 1;
                }
            }
            const now_passing = passes_in_window < max;
            try ring.push(ctx.gpa, if (now_passing) 1.0 else 0.0);
            break :blk now_passing;
        },
        .time_ms => blk: {
            if (!slot.found_existing) {
                slot.value_ptr.* = .{ .time_ring = state.TimeRing.init(win.spec.n) };
            }
            const ring = &slot.value_ptr.time_ring;
            ring.evict(ctx.now_ms);
            const now_passing = ring.len() < max;
            if (now_passing) {
                try ring.push(ctx.gpa, 1.0, ctx.now_ms);
            }
            ctx.notifyClockSlot(rule, slot.key_ptr);
            break :blk now_passing;
        },
    };

    if (!passed) {
        rule.publishes_suppressed += 1;
        return .nil;
    }
    return x;
}

// --- OHLC bars ----------------------------------------------------------

/// `(bar WINDOW PAYLOAD)`. WINDOW is `N` (close every N samples) or
/// `:ms N` (close every N ms of wall-clock time, aligned to
/// `floor(now/N)*N`). Emits `<subject>.bar.{open,high,low,close}` on
/// close. Returns nil. Time bars also close from the server-side walker
/// when the window elapses without a new tick (see `tickClocks`).
fn callBar(ctx: *Context, args: []const Value) EvalError!Value {
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const win = try takeWindow(args, 0);
    if (args.len != win.consumed + 1) return error.ArityMismatch;
    const x = try state.asNumber(args[win.consumed]);

    const key = try state.keyForWindow(ctx.arena, "bar", mapWinKind(win.spec.kind), ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);

    switch (win.spec.kind) {
        .ticks => return updateTickBar(ctx, rule, slot, @intCast(win.spec.n), x),
        .time_ms => {
            const out = try updateTimeBar(ctx, rule, slot, win.spec.n, x);
            ctx.notifyClockSlot(rule, slot.key_ptr);
            return out;
        },
    }
}

fn updateTickBar(
    ctx: *Context,
    rule: *state.Rule,
    slot: state.StateSlot,
    cap: u32,
    x: f64,
) EvalError!Value {
    if (!slot.found_existing or slot.value_ptr.* == .empty) {
        slot.value_ptr.* = .{ .ohlc = .{ .cap = cap } };
    }
    const bar = &slot.value_ptr.ohlc;
    if (bar.tickUpdate(x)) |c| {
        try emitBarFields(ctx, c.open, c.high, c.low, c.close);
    } else {
        rule.publishes_suppressed += 1;
    }
    return .nil;
}

fn updateTimeBar(
    ctx: *Context,
    rule: *state.Rule,
    slot: state.StateSlot,
    window_ms: u64,
    x: f64,
) EvalError!Value {
    if (!slot.found_existing or slot.value_ptr.* == .empty) {
        slot.value_ptr.* = .{ .ohlc = .{ .window_ms = window_ms } };
    }
    const bar = &slot.value_ptr.ohlc;
    if (bar.timeUpdate(ctx.now_ms, x)) |c| {
        try emitBarFields(ctx, c.open, c.high, c.low, c.close);
    } else {
        rule.publishes_suppressed += 1;
    }
    return .nil;
}

fn emitBarFields(ctx: *Context, open: f64, high: f64, low: f64, close: f64) EvalError!void {
    const fields = [_]struct { name: []const u8, val: f64 }{
        .{ .name = "open", .val = open },
        .{ .name = "high", .val = high },
        .{ .name = "low", .val = low },
        .{ .name = "close", .val = close },
    };
    for (fields) |f| {
        const subj = try std.fmt.allocPrint(ctx.arena, "{s}.bar.{s}", .{ ctx.subject, f.name });
        const out = try std.fmt.allocPrint(ctx.arena, "{d}", .{f.val});
        subject_mod.validatePublish(subj) catch return error.InvalidSubject;
        try ctx.emit(subj, out);
    }
}

// --- Counters -----------------------------------------------------------

/// `(count)` / `(count COND)`. Running counter; with COND, increments
/// only when truthy. Each increment publishes the new total to
/// `<subject>.count`. Returns nil.
fn callCount(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len > 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const inc = args.len == 0 or args[0].isTruthy();

    const key = try state.keyForOp(ctx.arena, "count", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    const prev: u64 = if (!slot.found_existing or slot.value_ptr.* == .empty)
        0
    else
        @intFromFloat(slot.value_ptr.number);
    var k: kernel.Count = .{ .n = prev };
    const next = k.update(inc) orelse {
        rule.publishes_suppressed += 1;
        return .nil;
    };
    slot.value_ptr.* = .{ .number = @floatFromInt(next) };

    const subj = try std.fmt.allocPrint(ctx.arena, "{s}.count", .{ctx.subject});
    const out = try std.fmt.allocPrint(ctx.arena, "{d}", .{next});
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    try ctx.emit(subj, out);
    return .nil;
}

// --- Debug --------------------------------------------------------------

/// `(print! X)` / `(print! LABEL X)`. Logs one line via `std.log.info`
/// (scope `print`) and returns X unchanged so it sits cleanly in `->`
/// chains: `(-> payload-float (print! "raw") (round 1) (publish! ...))`.
/// Not a publish; counters are not bumped. The loader counts these and
/// the server warns at startup, so a left-in `print!` is not silent.
fn callPrint(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 1 or args.len > 2) return error.ArityMismatch;
    const label: ?[]const u8 = if (args.len == 2) try state.asString(args[0]) else null;
    const value = args[args.len - 1];

    var buf: std.ArrayList(u8) = .empty;
    if (label) |l| {
        try buf.print(ctx.arena, "[{s}] {s} = ", .{ ctx.subject, l });
    } else {
        try buf.print(ctx.arena, "[{s}] = ", .{ctx.subject});
    }
    try formatValue(&buf, ctx.arena, value);

    std.log.scoped(.print).info("{s}", .{buf.items});

    return value;
}

fn formatValue(buf: *std.ArrayList(u8), gpa: Allocator, v: Value) !void {
    switch (v) {
        .nil => try buf.appendSlice(gpa, "nil"),
        .boolean => |b| try buf.appendSlice(gpa, if (b) "true" else "false"),
        .number => |n| try buf.print(gpa, "{d}", .{n}),
        .symbol => |s| try buf.appendSlice(gpa, s),
        .keyword => |s| try buf.print(gpa, ":{s}", .{s}),
        .string => |s| try buf.print(gpa, "\"{s}\"", .{s}),
        .list => |items| {
            try buf.append(gpa, '(');
            for (items, 0..) |it, i| {
                if (i > 0) try buf.append(gpa, ' ');
                try formatValue(buf, gpa, it);
            }
            try buf.append(gpa, ')');
        },
        .vector => |items| {
            try buf.append(gpa, '[');
            for (items, 0..) |it, i| {
                if (i > 0) try buf.append(gpa, ' ');
                try formatValue(buf, gpa, it);
            }
            try buf.append(gpa, ']');
        },
    }
}

// --- JSON ops -----------------------------------------------------------

/// `(json-get KEY PAYLOAD)`. Top-level object lookup; returns number /
/// string / bool, or nil for missing / null / nested.
fn callJsonGet(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const key = try state.asString(args[0]);
    const payload = try state.asString(args[1]);
    return jsonLookup(ctx.arena, payload, key) catch |e| switch (e) {
        error.OutOfMemory => error.OutOfMemory,
        else => .nil,
    };
}

/// `(json-demux KEY ... PAYLOAD)`. For each KEY in the top-level object,
/// publishes its value to `<subject>.<key>`. Skips missing / null / nested.
fn callJsonDemux(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    const payload = try state.asString(args[args.len - 1]);
    for (args[0 .. args.len - 1]) |a| {
        const key = try state.asString(a);
        const v = jsonLookup(ctx.arena, payload, key) catch |e| switch (e) {
            error.OutOfMemory => return error.OutOfMemory,
            else => continue,
        };
        if (v == .nil) continue;
        const subj = try std.fmt.allocPrint(ctx.arena, "{s}.{s}", .{ ctx.subject, key });
        const out = try coercePayload(ctx.arena, v);
        subject_mod.validatePublish(subj) catch return error.InvalidSubject;
        try ctx.emit(subj, out);
    }
    return .nil;
}

const JsonLookupError = error{ Malformed, NotObject, KeyMissing, NestedValue } || Allocator.Error;

/// Find KEY in a top-level JSON object. Errors on malformed / non-object /
/// nested values. Backed by `std.json.Scanner`.
fn jsonLookup(arena: Allocator, payload: []const u8, key: []const u8) JsonLookupError!Value {
    var scanner = std.json.Scanner.initCompleteInput(arena, payload);
    defer scanner.deinit();

    const first = scanner.next() catch return error.Malformed;
    if (first != .object_begin) return error.NotObject;

    while (true) {
        const tok = scanner.nextAlloc(arena, .alloc_if_needed) catch return error.Malformed;
        const this_key = switch (tok) {
            .string => |s| s,
            .allocated_string => |s| s,
            .object_end => return error.KeyMissing,
            else => return error.Malformed,
        };
        if (!std.mem.eql(u8, this_key, key)) {
            scanner.skipValue() catch return error.Malformed;
            continue;
        }
        const v = scanner.nextAlloc(arena, .alloc_if_needed) catch return error.Malformed;
        return switch (v) {
            .string, .allocated_string => |s| .{ .string = s },
            .number, .allocated_number => |s| .{
                .number = std.fmt.parseFloat(f64, s) catch return error.Malformed,
            },
            .true => .{ .boolean = true },
            .false => .{ .boolean = false },
            .null => .nil,
            .object_begin, .array_begin => error.NestedValue,
            else => error.Malformed,
        };
    }
}
