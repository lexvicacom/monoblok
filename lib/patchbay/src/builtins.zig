//! Built-in operators for the patchbay DSL: arithmetic, comparisons, gates
//! (`squelch`, `deadband`, `hold-off`, edge detectors), windowed aggregates
//! (`moving-*`), JSON ops, OHLC bars, the `count` accumulator, the
//! threading macro `->`, and the special forms `if` / `when` / `and` / `or`
//! / `do` / `transition`.
//!
//! The single dispatch entry point is `evalCall`; `eval.evalNormal` delegates
//! to it for any list-form. Stateful ops read and write through the
//! `state.zig` slot helpers, so this module never touches the rule's state
//! map directly.

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

    // Most calls have <= 8 args (publish, gates, arithmetic). Stack-back the
    // evaluated-args slice for that range and only fall through to the arena
    // for the rare wide form (json-demux, str-concat, do, etc.).
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

/// `(-> X f1 f2 ...)`, thread X through each form as its LAST argument.
/// A bare symbol `f` is treated as the call `(f)`, so
/// `(-> payload-float (round 3) (squelch))` expands to
/// `(squelch (round 3 payload-float))`. Last-arg threading fits this
/// dialect: `round`, `moving-avg`, `deadband`, `publish-to` all take the
/// value as their final argument.
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
        // Append the already-evaluated threaded value as the last arg.
        // Safe because eval() on .nil/.boolean/.number/.string is identity,
        // and prior evals never yield a .symbol.
        rebuilt[call_items.len] = acc;
        acc = try ctx.eval_fn(ctx, .{ .list = rebuilt });
    }
    return acc;
}

/// `(transition BOOL RISING-BRANCH FALLING-BRANCH)`, one boolean edge
/// detector that dispatches both directions. Evaluates BOOL, compares to
/// the prior value stored per (rule, subject), and evaluates exactly one
/// branch: RISING-BRANCH on false→true, FALLING-BRANCH on true→false.
/// Returns nil (and leaves both branches unevaluated) on first sight or
/// no-change. State key is distinct from rising-edge/falling-edge so the
/// three can coexist without aliasing.
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

// --- Trace plumbing -----------------------------------------------------

/// Record an emission for trace output. No-op when not tracing. Strings are
/// expected to live in `ctx.arena` so they remain valid until the per-message
/// arena reset.
///
/// FIXME: not happy with this, every side-effecting op has to remember to
/// call this, which is exactly the kind of tight coupling a tracer should
/// avoid. The cleaner shape is to wrap `ctx.publisher` for the duration of
/// `run` so the spy lives in one place and ops stay oblivious. Left inline
/// for now to keep the diff small.
fn recordTraceEmit(ctx: *Context, subject: []const u8, payload: []const u8) void {
    if (!ctx.trace) return;
    ctx.trace_emissions.append(ctx.arena, .{ .subject = subject, .payload = payload }) catch {};
}

// --- Side-effecting publish ops -----------------------------------------

fn callPublish(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const subj = try state.asString(args[0]);
    const payload = try state.asString(args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    ctx.publisher.publish(subj, payload) catch return error.PublishFailed;
    ctx.rule_publishes += 1;
    if (ctx.current_rule) |r| r.publishes_emitted += 1;
    recordTraceEmit(ctx, subj, payload);
    return .nil;
}

/// `(publish-to SUBJECT VALUE)`, publish with args flipped so it slots
/// into a `->` pipeline: `(-> x (publish-to "foo"))` publishes x to foo.
/// Numbers are formatted canonically; strings pass through. If VALUE is
/// nil (a suppressed gate upstream), this is a no-op, that's what makes
/// `(-> ... (squelch) ... (publish-to ...))` read top-to-bottom.
fn callPublishTo(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    if (args[1] == .nil) return .nil;
    const subj = try state.asString(args[0]);
    const payload = try coercePayload(ctx.arena, args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    ctx.publisher.publish(subj, payload) catch return error.PublishFailed;
    ctx.rule_publishes += 1;
    if (ctx.current_rule) |r| r.publishes_emitted += 1;
    recordTraceEmit(ctx, subj, payload);
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

/// `(starts-with? HAY NEEDLE)` / `(ends-with? HAY NEEDLE)`, mirror `contains?`.
fn callStartsEnds(args: []const Value, kind: AffixKind) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const hay = try state.asString(args[0]);
    const needle = try state.asString(args[1]);
    return .{ .boolean = switch (kind) {
        .starts => std.mem.startsWith(u8, hay, needle),
        .ends => std.mem.endsWith(u8, hay, needle),
    } };
}

/// `(subject-token N)` / `(subject-token N S)` returns the Nth
/// dot-separated token (0-indexed) of the current subject, or of an
/// explicit string. Returns nil if N is out of range.
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

/// `(round N X)`, round X to N decimal places. Returns a number.
fn callRound(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const decimals = try state.asNumber(args[0]);
    const x = try state.asNumber(args[1]);
    if (decimals < 0 or decimals > 15) return error.TypeMismatch;
    const scale = std.math.pow(f64, 10.0, decimals);
    return .{ .number = @round(x * scale) / scale };
}

/// `(quantize STEP X)`, snap X to the nearest multiple of STEP. Useful
/// when you want 0.5-degree buckets, 100-ms buckets, etc.
fn callQuantize(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const step = try state.asNumber(args[0]);
    const x = try state.asNumber(args[1]);
    if (step == 0) return error.TypeMismatch;
    return .{ .number = @round(x / step) * step };
}

/// `(clamp LO HI X)`, clip X to the inclusive range [LO, HI]. Value
/// last so it threads: `(-> payload-float (clamp 0 100))`.
fn callClamp(args: []const Value) EvalError!Value {
    if (args.len != 3) return error.ArityMismatch;
    const lo = try state.asNumber(args[0]);
    const hi = try state.asNumber(args[1]);
    const x = try state.asNumber(args[2]);
    if (lo > hi) return error.TypeMismatch;
    return .{ .number = std.math.clamp(x, lo, hi) };
}

const MinMaxKind = enum { min, max };

/// `(min A B ...)` / `(max A B ...)`, variadic over numbers; at least one arg.
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

/// `(sign X)`, returns -1, 0, or 1.
fn callSign(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const x = try state.asNumber(args[0]);
    return .{ .number = if (x > 0) 1.0 else if (x < 0) -1.0 else 0.0 };
}

// --- Stateful gates -----------------------------------------------------

/// `(squelch X)`, passes X through iff it differs from the last X seen
/// on this subject by this rule, otherwise returns nil. On first sight
/// for a subject, passes. Truthy-on-pass / falsy-on-suppress means it
/// still works as a gate in `when` / `and`, and threads cleanly through
/// `->` into a downstream sink.
fn callSquelch(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const encoded = try state.encodeForState(ctx.arena, args[0]);
    const key = try state.stateKey(ctx.arena, "squelch", ctx.subject);
    const changed = try state.stateEqualsOrStore(ctx.gpa, rule, key, encoded);
    if (!changed) rule.publishes_suppressed += 1;
    return if (changed) args[0] else .nil;
}

/// `(deadband DELTA X)`, passes X through iff it differs from the last
/// accepted X on this subject by at least DELTA, otherwise nil. Classic
/// analog-sensor gate: suppresses noise smaller than DELTA. Truthy-
/// on-pass keeps it composable with both `when` and `->`.
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

/// `(changed? X)`, boolean predicate: true iff X differs from the last
/// X seen on this subject by this rule. First sight returns true. Unlike
/// `squelch` (returns value-or-nil), this returns a boolean so it
/// composes cleanly inside `if` / `and` / `or`. Keyed per (rule, subject)
/// with a distinct prefix so it doesn't collide with `squelch`.
fn callChanged(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const encoded = try state.encodeForState(ctx.arena, args[0]);
    const key = try state.stateKey(ctx.arena, "changed?", ctx.subject);
    const changed = try state.stateEqualsOrStore(ctx.gpa, rule, key, encoded);
    if (!changed) rule.publishes_suppressed += 1;
    return .{ .boolean = changed };
}

/// `(delta X)`, numeric difference between X and the last X seen on
/// this (rule, subject). First sight returns 0. Stored as a number in
/// the existing state union. Keyed per (rule, subject) with a distinct
/// prefix so it doesn't collide with `deadband`.
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

/// `(hold-off MS X)` (radar term: after firing, ignore further triggers
/// for MS milliseconds). Passes X through on first sight and on any
/// subsequent call that arrives at least MS ms after the previous pass;
/// returns nil otherwise. Time source is `ctx.now_ms`, stamped once per
/// ingress by the server, so every op in one evaluation sees the same
/// "now". Per (rule, subject). Composes with `->` the same as `squelch`.
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

/// `(rising-edge X)` / `(falling-edge X)`, returns X on the matching
/// boolean transition, nil otherwise. First sight returns nil (no prior
/// state means no edge). Keyed per (rule, op, subject) so two rising-edge
/// gates on the same subject don't share state. Stored as a number (0/1)
/// in the existing state union, no new variant needed.
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

/// `(moving-avg N X)` / `moving-sum` / `moving-max` / `moving-min`,
/// push numeric X into an N-wide ring (per rule+subject+op) and return
/// the corresponding aggregate over the current window. The ring is
/// allocated on first sight per slot; `N` must be a positive literal
/// (well, any number ≥ 1; we round down). First call returns the
/// aggregate over just the first sample.
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

/// `(bar N PAYLOAD)`. Side-effecting tick-count bar accumulator. Each
/// call adds one sample. Every Nth call closes a bar and publishes four
/// sub-subjects under `<current-subject>.bar`:
///
///   .open   first sample of the bar
///   .high   max sample seen in the bar
///   .low    min sample seen in the bar
///   .close  this Nth sample (the one that closed the bar)
///
/// State is per (rule, subject). Returns nil so it composes with `do` /
/// `->` without polluting downstream values. Bar-in-progress state
/// survives a snapshot reload; if the new patchbay's literal `N` differs
/// from the saved `cap`, the saved `cap` wins until that bar closes.
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
        ctx.publisher.publish(subj, out) catch return error.PublishFailed;
        ctx.rule_publishes += 1;
        rule.publishes_emitted += 1;
        recordTraceEmit(ctx, subj, out);
    }
    bar.count = 0;
    return .nil;
}

// --- Counters -----------------------------------------------------------

/// `(count)` or `(count COND)`. Side-effecting running counter, per
/// (rule, subject). With no args, increments on every call. With one arg,
/// increments only when COND is truthy (any value type, same `isTruthy`
/// rules as `if` / `when`). Each increment publishes the new total to
/// `<subject>.count`. Returns nil so it slots into a `do` block or sits at
/// the tail of a `->` pipeline without disturbing the threaded value:
/// `(-> payload-float (count) (round 1) (publish-to ...))`. State is a
/// plain `.number` so it round-trips through snapshots for free.
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
    ctx.publisher.publish(subj, out) catch return error.PublishFailed;
    ctx.rule_publishes += 1;
    rule.publishes_emitted += 1;
    recordTraceEmit(ctx, subj, out);
    return .nil;
}

// --- JSON ops -----------------------------------------------------------

/// `(json-get KEY PAYLOAD)`. Top-level object lookup only. Returns the field
/// as a number, string, or boolean (matching the JSON type), or nil if the
/// payload isn't a JSON object, the key is missing, or the value is null /
/// nested (object or array). No JSON path: keys are matched as exact strings,
/// dots in keys are not special. Value-last so it threads:
/// `(-> payload (json-get "temp") (round 1) (publish-to ...))`.
fn callJsonGet(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const key = try state.asString(args[0]);
    const payload = try state.asString(args[1]);
    return jsonLookup(ctx.arena, payload, key) catch |e| switch (e) {
        error.OutOfMemory => error.OutOfMemory,
        else => .nil,
    };
}

/// `(json-demux KEY ... PAYLOAD)`. Side-effecting demux: for each KEY found
/// in the top-level JSON object, publishes its value to
/// `<current-subject>.<key>`. Skips missing keys, null values, and nested
/// objects/arrays silently. Returns nil. Value-last so it can sit at the end
/// of a pipeline if needed, though typically it's the whole body:
/// `(json-demux "temp" "hum" payload)`.
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
        ctx.publisher.publish(subj, out) catch return error.PublishFailed;
        ctx.rule_publishes += 1;
        if (ctx.current_rule) |r| r.publishes_emitted += 1;
        recordTraceEmit(ctx, subj, out);
    }
    return .nil;
}

const JsonLookupError = error{ Malformed, NotObject, KeyMissing, NestedValue } || Allocator.Error;

/// Find KEY in a top-level JSON object payload and return its value as a
/// rule Value (number / string / boolean / nil-for-null). Returns an error
/// for missing keys, malformed payloads, non-object payloads, or values
/// that are nested objects/arrays. Backed by `std.json.Scanner` so escape
/// handling, number validation, and \uXXXX are someone else's problem.
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
