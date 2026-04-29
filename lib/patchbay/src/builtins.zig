//! Built-in operators for the patchbay DSL. Single dispatch entry is
//! `evalCall`; stateful ops go through `state.zig`'s slot helpers.

const std = @import("std");
const Allocator = std.mem.Allocator;
const subject_mod = @import("subject.zig");
const state = @import("state.zig");
const eval = @import("eval.zig");

const Value = state.Value;
const Context = eval.Context;
const EvalError = eval.EvalError;

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
    contains, starts_with, ends_with, subject_token,
    round, quantize, clamp, min, max, abs, sign,
    squelch, deadband, changed, delta, hold_off,
    moving_avg, moving_sum, moving_max, moving_min,
    rising_edge, falling_edge,
    json_get, json_demux,
    bar,
    count,
};

const op_map = std.StaticStringMap(Op).initComptime(.{
    .{ "publish", .publish },
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
    .{ "moving-avg", .moving_avg },
    .{ "moving-sum", .moving_sum },
    .{ "moving-max", .moving_max },
    .{ "moving-min", .moving_min },
    .{ "rising-edge", .rising_edge },
    .{ "falling-edge", .falling_edge },
    .{ "json-get", .json_get },
    .{ "json-demux", .json_demux },
    .{ "bar", .bar },
    .{ "count", .count },
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
        .rising_edge => callEdge(ctx, evaled, .rising),
        .falling_edge => callEdge(ctx, evaled, .falling),
        .json_get => callJsonGet(ctx, evaled),
        .json_demux => callJsonDemux(ctx, evaled),
        .bar => callBar(ctx, evaled),
        .count => callCount(ctx, evaled),
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

    const key = try state.stateKey(ctx.arena, "transition", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = if (now) 1 else 0 };
        rule.publishes_suppressed += 1;
        return .nil;
    }
    const prev = slot.value_ptr.number != 0;
    slot.value_ptr.* = .{ .number = if (now) 1 else 0 };
    if (!prev and now) return ctx.eval_fn(ctx, args[1]);
    if (prev and !now) return ctx.eval_fn(ctx, args[2]);
    rule.publishes_suppressed += 1;
    return .nil;
}

// --- Side-effecting publish ops -----------------------------------------

fn callPublish(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const subj = try state.asString(args[0]);
    const payload = try state.asString(args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    try ctx.emit(subj, payload);
    return .nil;
}

/// `(publish-to SUBJECT VALUE)`. Args flipped so it threads through `->`.
/// No-op on nil VALUE (suppressed upstream gate).
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
        .list, .keyword => error.TypeMismatch,
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
    return .{ .number = @round(x / step) * step };
}

/// `(clamp LO HI X)`, X clipped to [LO, HI]. Value-last for threading.
fn callClamp(args: []const Value) EvalError!Value {
    if (args.len != 3) return error.ArityMismatch;
    const lo = try state.asNumber(args[0]);
    const hi = try state.asNumber(args[1]);
    const x = try state.asNumber(args[2]);
    if (lo > hi) return error.TypeMismatch;
    return .{ .number = std.math.clamp(x, lo, hi) };
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
    const encoded = try state.encodeForState(ctx.arena, args[0]);
    const key = try state.stateKey(ctx.arena, "squelch", ctx.subject);
    const changed = try state.stateEqualsOrStore(ctx.gpa, rule, key, encoded);
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

    const key = try state.stateKey(ctx.arena, "deadband", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = x };
        return .{ .number = x };
    }
    const last = slot.value_ptr.number;
    if (@abs(x - last) < delta) {
        rule.publishes_suppressed += 1;
        return .nil;
    }
    slot.value_ptr.* = .{ .number = x };
    return .{ .number = x };
}

/// `(changed? X)`. Boolean version of `squelch`; first sight returns true.
fn callChanged(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const encoded = try state.encodeForState(ctx.arena, args[0]);
    const key = try state.stateKey(ctx.arena, "changed?", ctx.subject);
    const changed = try state.stateEqualsOrStore(ctx.gpa, rule, key, encoded);
    if (!changed) rule.publishes_suppressed += 1;
    return .{ .boolean = changed };
}

/// `(delta X)`. X minus the last X. First sight returns 0.
fn callDelta(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const x = try state.asNumber(args[0]);
    const key = try state.stateKey(ctx.arena, "delta", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = x };
        return .{ .number = 0 };
    }
    const last = slot.value_ptr.number;
    slot.value_ptr.* = .{ .number = x };
    return .{ .number = x - last };
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

    const key = try state.stateKey(ctx.arena, "hold-off", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = now };
        return args[1];
    }
    const last = slot.value_ptr.number;
    if (now - last < ms) {
        rule.publishes_suppressed += 1;
        return .nil;
    }
    slot.value_ptr.* = .{ .number = now };
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
    const key = try state.stateKey(ctx.arena, op_name, ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = if (now) 1 else 0 };
        rule.publishes_suppressed += 1;
        return .nil;
    }
    const prev = slot.value_ptr.number != 0;
    slot.value_ptr.* = .{ .number = if (now) 1 else 0 };
    const fired = switch (kind) {
        .rising => !prev and now,
        .falling => prev and !now,
    };
    if (!fired) rule.publishes_suppressed += 1;
    return if (fired) args[0] else .nil;
}

// --- Windowed aggregates ------------------------------------------------

const MovingKind = enum { avg, sum, max, min };

/// `(moving-{avg,sum,max,min} N X)`. Push X into an N-wide ring and
/// return the aggregate over the current window. Ring allocates on first
/// sight; first call's aggregate is over just that one sample.
fn callMoving(ctx: *Context, args: []const Value, kind: MovingKind) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const n_f = try state.asNumber(args[0]);
    if (n_f < 1) return error.TypeMismatch;
    const n: usize = @intFromFloat(n_f);
    const x = try state.asNumber(args[1]);

    const op_name = switch (kind) {
        .avg => "moving-avg",
        .sum => "moving-sum",
        .max => "moving-max",
        .min => "moving-min",
    };
    const key = try state.stateKey(ctx.arena, op_name, ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .ring = try state.Ring.init(ctx.gpa, n) };
    }
    const ring = &slot.value_ptr.ring;
    try ring.push(ctx.gpa, x);

    return .{ .number = switch (kind) {
        .avg => ring.mean(),
        .sum => ring.sum,
        .max => ring.max(),
        .min => ring.min(),
    } };
}

// --- OHLC bars ----------------------------------------------------------

/// `(bar N PAYLOAD)`. Tick-count bar accumulator; every Nth call closes a
/// bar and publishes `<subject>.bar.{open,high,low,close}`. Returns nil.
/// In-progress bar survives snapshot reload (saved `cap` wins until close).
fn callBar(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const n_f = try state.asNumber(args[0]);
    if (n_f < 1) return error.TypeMismatch;
    const n: u32 = @intFromFloat(n_f);
    const x = try state.asNumber(args[1]);

    const key = try state.stateKey(ctx.arena, "bar", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing or slot.value_ptr.* == .empty) {
        slot.value_ptr.* = .{ .ohlc = .{ .open = x, .high = x, .low = x, .count = 1, .cap = n } };
    } else {
        const bar = &slot.value_ptr.ohlc;
        if (bar.count == 0) {
            bar.open = x;
            bar.high = x;
            bar.low = x;
            bar.count = 1;
        } else {
            if (x > bar.high) bar.high = x;
            if (x < bar.low) bar.low = x;
            bar.count += 1;
        }
    }

    const bar = &slot.value_ptr.ohlc;
    if (bar.count < bar.cap) {
        rule.publishes_suppressed += 1;
        return .nil;
    }

    // Bar closes on this tick. Emit four sub-subjects, then reset.
    const fields = [_]struct { name: []const u8, val: f64 }{
        .{ .name = "open", .val = bar.open },
        .{ .name = "high", .val = bar.high },
        .{ .name = "low", .val = bar.low },
        .{ .name = "close", .val = x },
    };
    for (fields) |f| {
        const subj = try std.fmt.allocPrint(ctx.arena, "{s}.bar.{s}", .{ ctx.subject, f.name });
        const out = try std.fmt.allocPrint(ctx.arena, "{d}", .{f.val});
        subject_mod.validatePublish(subj) catch return error.InvalidSubject;
        try ctx.emit(subj, out);
    }
    bar.count = 0;
    return .nil;
}

// --- Counters -----------------------------------------------------------

/// `(count)` / `(count COND)`. Running counter; with COND, increments
/// only when truthy. Each increment publishes the new total to
/// `<subject>.count`. Returns nil.
fn callCount(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len > 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    if (args.len == 1 and !args[0].isTruthy()) {
        rule.publishes_suppressed += 1;
        return .nil;
    }

    const key = try state.stateKey(ctx.arena, "count", ctx.subject);
    const slot = try state.getOrPutStateSlot(ctx.gpa, rule, key);
    const next: f64 = if (!slot.found_existing or slot.value_ptr.* == .empty)
        1
    else
        slot.value_ptr.number + 1;
    slot.value_ptr.* = .{ .number = next };

    const subj = try std.fmt.allocPrint(ctx.arena, "{s}.count", .{ctx.subject});
    const out = try std.fmt.allocPrint(ctx.arena, "{d}", .{next});
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    try ctx.emit(subj, out);
    return .nil;
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
