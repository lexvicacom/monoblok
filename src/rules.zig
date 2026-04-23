const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");
const subject_mod = @import("subject.zig");

pub const Value = sexpr.Value;

/// A compiled rule: a subject filter, a body to evaluate when the filter
/// matches an incoming publish, and a per-subject state table used by
/// stateful primitives (`squelch`, `deadband`).
pub const Rule = struct {
    filter: []const u8,
    body: Value,
    /// Per-subject memory for stateful primitives. Keyed by the concrete
    /// incoming subject; value is the last payload observed (or the last
    /// value of whatever expression the stateful op was called on).
    /// Keys and stored payloads are owned by `gpa` and freed in `deinit`.
    state: std.StringHashMapUnmanaged(StateEntry) = .empty,

    /// Cumulative totals since server start. Exposed via `$STATS.rules.<i>.*`.
    /// `emitted` bumps on each successful `publish` / `publish-to` from this
    /// rule; `suppressed` bumps whenever a gate (`squelch`, `deadband`,
    /// `changed?`, `rising-edge`, `falling-edge`) returns nil.
    publishes_emitted: u64 = 0,
    publishes_suppressed: u64 = 0,

    pub fn deinit(self: *Rule, gpa: Allocator) void {
        var it = self.state.iterator();
        while (it.next()) |e| {
            gpa.free(e.key_ptr.*);
            e.value_ptr.deinit(gpa);
        }
        self.state.deinit(gpa);
    }
};

/// Stored last-seen value for a (rule, subject) pair. Stateful ops store
/// whatever shape they need: a byte buffer (for `squelch`), a number
/// (for `deadband`'s anchor), or a ring of numeric samples (for the
/// `moving-*` family). Buffers are reused in place across messages —
/// clear + appendSlice instead of free + dupe.
pub const StateEntry = union(enum) {
    empty,
    bytes: std.ArrayList(u8),
    number: f64,
    ring: Ring,

    pub fn deinit(self: *StateEntry, gpa: Allocator) void {
        switch (self.*) {
            .bytes => |*b| b.deinit(gpa),
            .ring => |*r| r.deinit(gpa),
            else => {},
        }
    }
};

/// Fixed-capacity ring of f64 samples with a running sum and monotonic
/// deques for O(1)-amortized max/min. The deques hold indices into the
/// ring (index is a monotonically increasing logical counter, not a
/// buffer offset — modular math happens at read time).
///
/// Window size is fixed at first use per (rule, subject, op) slot. It's
/// a usage error to call different `moving-N` sizes on the same slot;
/// we just trust the first allocation.
pub const Ring = struct {
    buf: []f64,
    /// Total samples pushed ever. `len = min(counter, cap)`. Positions
    /// live at `buf[counter % cap]` on write.
    counter: u64 = 0,
    sum: f64 = 0,
    /// Indices (as `counter` values, not buffer offsets) of candidates
    /// still in the window, front holds the running max / min.
    max_deque: std.ArrayList(u64) = .empty,
    min_deque: std.ArrayList(u64) = .empty,

    pub fn init(gpa: Allocator, cap: usize) !Ring {
        return .{ .buf = try gpa.alloc(f64, cap) };
    }

    pub fn deinit(self: *Ring, gpa: Allocator) void {
        gpa.free(self.buf);
        self.max_deque.deinit(gpa);
        self.min_deque.deinit(gpa);
    }

    pub fn len(self: Ring) usize {
        const cap = self.buf.len;
        return if (self.counter < cap) @intCast(self.counter) else cap;
    }

    /// Push `x`. Evicts the oldest sample if the ring was full, updates
    /// sum and both monotonic deques.
    pub fn push(self: *Ring, gpa: Allocator, x: f64) !void {
        const cap = self.buf.len;
        const idx = self.counter;
        if (self.counter >= cap) {
            const evict_idx = self.counter - cap;
            self.sum -= self.buf[@intCast(evict_idx % cap)];
            if (self.max_deque.items.len > 0 and self.max_deque.items[0] == evict_idx) {
                _ = self.max_deque.orderedRemove(0);
            }
            if (self.min_deque.items.len > 0 and self.min_deque.items[0] == evict_idx) {
                _ = self.min_deque.orderedRemove(0);
            }
        }
        self.buf[@intCast(idx % cap)] = x;
        self.sum += x;

        while (self.max_deque.items.len > 0) {
            const tail = self.max_deque.items[self.max_deque.items.len - 1];
            if (self.buf[@intCast(tail % cap)] <= x) {
                _ = self.max_deque.pop();
            } else break;
        }
        try self.max_deque.append(gpa, idx);

        while (self.min_deque.items.len > 0) {
            const tail = self.min_deque.items[self.min_deque.items.len - 1];
            if (self.buf[@intCast(tail % cap)] >= x) {
                _ = self.min_deque.pop();
            } else break;
        }
        try self.min_deque.append(gpa, idx);

        self.counter += 1;
    }

    pub fn mean(self: Ring) f64 {
        const n = self.len();
        return self.sum / @as(f64, @floatFromInt(n));
    }

    pub fn max(self: Ring) f64 {
        const cap = self.buf.len;
        return self.buf[@intCast(self.max_deque.items[0] % cap)];
    }

    pub fn min(self: Ring) f64 {
        const cap = self.buf.len;
        return self.buf[@intCast(self.min_deque.items[0] % cap)];
    }
};

pub const LoadError = error{
    InvalidRuleForm,
    UnknownTopLevel,
} || sexpr.ParseError || subject_mod.Error;

pub fn loadRules(arena: Allocator, source: []const u8) LoadError![]Rule {
    const forms = try sexpr.parseAll(arena, source);
    var out: std.ArrayList(Rule) = .empty;
    for (forms) |f| {
        if (f != .list) return error.InvalidRuleForm;
        const items = f.list;
        if (items.len < 2 or items[0] != .symbol) return error.UnknownTopLevel;
        // Other modules own their own top-level forms (e.g. `bridge`). Skip
        // anything we don't recognize as a rule — the other loader will
        // claim it and raise its own error if malformed.
        if (!std.mem.eql(u8, items[0].symbol, "on")) continue;
        if (items.len != 3 or items[1] != .string) return error.InvalidRuleForm;
        try subject_mod.validateFilter(items[1].string);
        try out.append(arena, .{ .filter = items[1].string, .body = items[2] });
    }
    return try out.toOwnedSlice(arena);
}

pub fn deinitRules(rules: []Rule, gpa: Allocator) void {
    for (rules) |*r| r.deinit(gpa);
}

pub const EvalError = error{
    UnknownSymbol,
    TypeMismatch,
    ArityMismatch,
    InvalidSubject,
    PublishFailed,
} || Allocator.Error;

/// Side-effect callback. The router plugs in a closure that enqueues a
/// fan-out for `(publish subject payload)`.
pub const Publisher = struct {
    ctx: *anyopaque,
    publish_fn: *const fn (ctx: *anyopaque, subject: []const u8, payload: []const u8) anyerror!void,

    pub fn publish(self: Publisher, subject: []const u8, payload: []const u8) !void {
        return self.publish_fn(self.ctx, subject, payload);
    }
};

pub const Context = struct {
    subject: []const u8,
    payload: []const u8,
    publisher: Publisher,
    /// Per-message scratch; caller owns an arena that is reset between messages.
    arena: Allocator,
    /// Long-lived allocator for per-rule state (e.g. `squelch` keeps the
    /// last-seen payload across messages). Distinct from `arena`, which is
    /// reset between messages.
    gpa: Allocator,
    /// Set by `run` before each rule body executes so stateful primitives
    /// know which rule's state table to read/write.
    current_rule: ?*Rule = null,
    /// Count of publishes the patchbay generated for this one inbound PUB.
    /// Incremented by `callPublish` / `callPublishTo`. Read by the server
    /// after `run()` returns to detect rule-amplification blow-ups.
    rule_publishes: u32 = 0,
};

pub fn run(rules: []Rule, ctx: *Context) !void {
    for (rules) |*rule| {
        if (!subject_mod.matches(rule.filter, ctx.subject)) continue;
        ctx.current_rule = rule;
        _ = try eval(ctx, rule.body);
    }
    ctx.current_rule = null;
}

fn eval(ctx: *Context, v: Value) EvalError!Value {
    return switch (v) {
        .nil, .boolean, .number, .string => v,
        .symbol => |s| evalSymbol(ctx, s),
        .list => |items| evalCall(ctx, items),
        .keyword => error.UnknownSymbol, // keywords are config-only, not rule body values
    };
}

fn evalSymbol(ctx: *Context, name: []const u8) EvalError!Value {
    if (std.mem.eql(u8, name, "subject")) return .{ .string = ctx.subject };
    if (std.mem.eql(u8, name, "payload")) return .{ .string = ctx.payload };
    if (std.mem.eql(u8, name, "payload-float")) {
        const n = std.fmt.parseFloat(f64, ctx.payload) catch return error.TypeMismatch;
        return .{ .number = n };
    }
    if (std.mem.eql(u8, name, "payload-int")) {
        const n = std.fmt.parseInt(i64, ctx.payload, 10) catch return error.TypeMismatch;
        return .{ .number = @floatFromInt(n) };
    }
    return error.UnknownSymbol;
}

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
    squelch, deadband, changed, delta,
    moving_avg, moving_sum, moving_max, moving_min,
    rising_edge, falling_edge,
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
    .{ "moving-avg", .moving_avg },
    .{ "moving-sum", .moving_sum },
    .{ "moving-max", .moving_max },
    .{ "moving-min", .moving_min },
    .{ "rising-edge", .rising_edge },
    .{ "falling-edge", .falling_edge },
});

fn evalCall(ctx: *Context, items: []const Value) EvalError!Value {
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

    const evaled = try ctx.arena.alloc(Value, args.len);
    for (args, 0..) |a, i| evaled[i] = try eval(ctx, a);

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
        .moving_avg => callMoving(ctx, evaled, .avg),
        .moving_sum => callMoving(ctx, evaled, .sum),
        .moving_max => callMoving(ctx, evaled, .max),
        .moving_min => callMoving(ctx, evaled, .min),
        .rising_edge => callEdge(ctx, evaled, .rising),
        .falling_edge => callEdge(ctx, evaled, .falling),
    };
}

fn evalIf(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 2 or args.len > 3) return error.ArityMismatch;
    const cond = try eval(ctx, args[0]);
    if (cond.isTruthy()) return eval(ctx, args[1]);
    if (args.len == 3) return eval(ctx, args[2]);
    return .nil;
}

fn evalWhen(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    const cond = try eval(ctx, args[0]);
    if (!cond.isTruthy()) return .nil;
    var last: Value = .nil;
    for (args[1..]) |a| last = try eval(ctx, a);
    return last;
}

fn evalAnd(ctx: *Context, args: []const Value) EvalError!Value {
    var last: Value = .{ .boolean = true };
    for (args) |a| {
        last = try eval(ctx, a);
        if (!last.isTruthy()) return last;
    }
    return last;
}

fn evalOr(ctx: *Context, args: []const Value) EvalError!Value {
    var last: Value = .{ .boolean = false };
    for (args) |a| {
        last = try eval(ctx, a);
        if (last.isTruthy()) return last;
    }
    return last;
}

fn evalDo(ctx: *Context, args: []const Value) EvalError!Value {
    var last: Value = .nil;
    for (args) |a| last = try eval(ctx, a);
    return last;
}

/// `(-> X f1 f2 ...)` — thread X through each form as its LAST argument.
/// A bare symbol `f` is treated as the call `(f)`, so
/// `(-> payload-float (round 3) (squelch))` expands to
/// `(squelch (round 3 payload-float))`. Last-arg threading fits this
/// dialect: `round`, `moving-avg`, `deadband`, `publish-to` all take the
/// value as their final argument.
fn evalThread(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len == 0) return error.ArityMismatch;
    var acc = try eval(ctx, args[0]);
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
        acc = try evalCall(ctx, rebuilt);
    }
    return acc;
}

fn callPublish(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const subj = try asString(args[0]);
    const payload = try asString(args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    ctx.publisher.publish(subj, payload) catch return error.PublishFailed;
    ctx.rule_publishes += 1;
    if (ctx.current_rule) |r| r.publishes_emitted += 1;
    return .nil;
}

/// `(publish-to SUBJECT VALUE)` — publish with args flipped so it slots
/// into a `->` pipeline: `(-> x (publish-to "foo"))` publishes x to foo.
/// Numbers are formatted canonically; strings pass through. If VALUE is
/// nil (a suppressed gate upstream), this is a no-op — that's what makes
/// `(-> ... (squelch) ... (publish-to ...))` read top-to-bottom.
fn callPublishTo(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    if (args[1] == .nil) return .nil;
    const subj = try asString(args[0]);
    const payload = try coercePayload(ctx.arena, args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    ctx.publisher.publish(subj, payload) catch return error.PublishFailed;
    ctx.rule_publishes += 1;
    if (ctx.current_rule) |r| r.publishes_emitted += 1;
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

fn callSubjectAppend(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const suffix = try asString(args[0]);
    const joined = try std.fmt.allocPrint(ctx.arena, "{s}.{s}", .{ ctx.subject, suffix });
    return .{ .string = joined };
}

fn callStrConcat(ctx: *Context, args: []const Value) EvalError!Value {
    var total: usize = 0;
    for (args) |a| total += (try asString(a)).len;
    const buf = try ctx.arena.alloc(u8, total);
    var i: usize = 0;
    for (args) |a| {
        const s = try asString(a);
        @memcpy(buf[i..][0..s.len], s);
        i += s.len;
    }
    return .{ .string = buf };
}

fn callNot(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    return .{ .boolean = !args[0].isTruthy() };
}

fn callEq(args: []const Value) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    const first = args[0];
    for (args[1..]) |a| if (!valueEql(first, a)) return .{ .boolean = false };
    return .{ .boolean = true };
}

const Cmp = enum { gt, lt, ge, le };

fn callCmp(args: []const Value, cmp: Cmp) EvalError!Value {
    if (args.len < 2) return error.ArityMismatch;
    var i: usize = 0;
    while (i + 1 < args.len) : (i += 1) {
        const a = try asNumber(args[i]);
        const b = try asNumber(args[i + 1]);
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
    var acc = try asNumber(args[0]);
    if (args.len == 1) {
        return .{ .number = switch (op) {
            .add, .mul => acc,
            .sub => -acc,
            .div => 1.0 / acc,
        } };
    }
    for (args[1..]) |a| {
        const x = try asNumber(a);
        acc = switch (op) {
            .add => acc + x,
            .sub => acc - x,
            .mul => acc * x,
            .div => acc / x,
        };
    }
    return .{ .number = acc };
}

fn callContains(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const hay = try asString(args[0]);
    const needle = try asString(args[1]);
    return .{ .boolean = std.mem.indexOf(u8, hay, needle) != null };
}

const AffixKind = enum { starts, ends };

/// `(starts-with? HAY NEEDLE)` / `(ends-with? HAY NEEDLE)` — mirror `contains?`.
fn callStartsEnds(args: []const Value, kind: AffixKind) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const hay = try asString(args[0]);
    const needle = try asString(args[1]);
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
    const n_f = try asNumber(args[0]);
    if (n_f < 0) return error.TypeMismatch;
    const n: usize = @intFromFloat(n_f);
    const s = if (args.len == 2) try asString(args[1]) else ctx.subject;
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |tok| : (i += 1) {
        if (i == n) return .{ .string = tok };
    }
    return .nil;
}


/// `(round N X)` — round X to N decimal places. Returns a number.
fn callRound(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const decimals = try asNumber(args[0]);
    const x = try asNumber(args[1]);
    if (decimals < 0 or decimals > 15) return error.TypeMismatch;
    const scale = std.math.pow(f64, 10.0, decimals);
    return .{ .number = @round(x * scale) / scale };
}

/// `(quantize STEP X)` — snap X to the nearest multiple of STEP. Useful
/// when you want 0.5-degree buckets, 100-ms buckets, etc.
fn callQuantize(args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const step = try asNumber(args[0]);
    const x = try asNumber(args[1]);
    if (step == 0) return error.TypeMismatch;
    return .{ .number = @round(x / step) * step };
}

/// `(clamp LO HI X)` — clip X to the inclusive range [LO, HI]. Value
/// last so it threads: `(-> payload-float (clamp 0 100))`.
fn callClamp(args: []const Value) EvalError!Value {
    if (args.len != 3) return error.ArityMismatch;
    const lo = try asNumber(args[0]);
    const hi = try asNumber(args[1]);
    const x = try asNumber(args[2]);
    if (lo > hi) return error.TypeMismatch;
    return .{ .number = std.math.clamp(x, lo, hi) };
}

const MinMaxKind = enum { min, max };

/// `(min A B ...)` / `(max A B ...)` — variadic over numbers; at least one arg.
fn callMinMax(args: []const Value, kind: MinMaxKind) EvalError!Value {
    if (args.len == 0) return error.ArityMismatch;
    var acc = try asNumber(args[0]);
    for (args[1..]) |a| {
        const x = try asNumber(a);
        acc = switch (kind) {
            .min => @min(acc, x),
            .max => @max(acc, x),
        };
    }
    return .{ .number = acc };
}

fn callAbs(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    return .{ .number = @abs(try asNumber(args[0])) };
}

/// `(sign X)` — returns -1, 0, or 1.
fn callSign(args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const x = try asNumber(args[0]);
    return .{ .number = if (x > 0) 1.0 else if (x < 0) -1.0 else 0.0 };
}

/// `(squelch X)` — passes X through iff it differs from the last X seen
/// on this subject by this rule, otherwise returns nil. On first sight
/// for a subject, passes. Truthy-on-pass / falsy-on-suppress means it
/// still works as a gate in `when` / `and`, and threads cleanly through
/// `->` into a downstream sink.
fn callSquelch(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const encoded = try encodeForState(ctx.arena, args[0]);
    const key = try stateKey(ctx.arena, "squelch", ctx.subject);
    const changed = try stateEqualsOrStore(ctx.gpa, rule, key, encoded);
    if (!changed) rule.publishes_suppressed += 1;
    return if (changed) args[0] else .nil;
}

/// `(deadband DELTA X)` — passes X through iff it differs from the last
/// accepted X on this subject by at least DELTA, otherwise nil. Classic
/// analog-sensor gate: suppresses noise smaller than DELTA. Truthy-
/// on-pass keeps it composable with both `when` and `->`.
fn callDeadband(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const delta = try asNumber(args[0]);
    const x = try asNumber(args[1]);
    if (delta < 0) return error.TypeMismatch;

    const key = try stateKey(ctx.arena, "deadband", ctx.subject);
    const slot = try getOrPutStateSlot(ctx.gpa, rule, key);
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

/// `(changed? X)` — boolean predicate: true iff X differs from the last
/// X seen on this subject by this rule. First sight returns true. Unlike
/// `squelch` (returns value-or-nil), this returns a boolean so it
/// composes cleanly inside `if` / `and` / `or`. Keyed per (rule, subject)
/// with a distinct prefix so it doesn't collide with `squelch`.
fn callChanged(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const encoded = try encodeForState(ctx.arena, args[0]);
    const key = try stateKey(ctx.arena, "changed?", ctx.subject);
    const changed = try stateEqualsOrStore(ctx.gpa, rule, key, encoded);
    if (!changed) rule.publishes_suppressed += 1;
    return .{ .boolean = changed };
}

/// `(delta X)` — numeric difference between X and the last X seen on
/// this (rule, subject). First sight returns 0. Stored as a number in
/// the existing state union. Keyed per (rule, subject) with a distinct
/// prefix so it doesn't collide with `deadband`.
fn callDelta(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const x = try asNumber(args[0]);
    const key = try stateKey(ctx.arena, "delta", ctx.subject);
    const slot = try getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = x };
        return .{ .number = 0 };
    }
    const last = slot.value_ptr.number;
    slot.value_ptr.* = .{ .number = x };
    return .{ .number = x - last };
}

const EdgeKind = enum { rising, falling };

/// `(rising-edge X)` / `(falling-edge X)` — returns X on the matching
/// boolean transition, nil otherwise. First sight returns nil (no prior
/// state means no edge). Keyed per (rule, op, subject) so two rising-edge
/// gates on the same subject don't share state. Stored as a number (0/1)
/// in the existing state union — no new variant needed.
fn callEdge(ctx: *Context, args: []const Value, kind: EdgeKind) EvalError!Value {
    if (args.len != 1) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const now = args[0].isTruthy();

    const op_name = switch (kind) {
        .rising => "rising-edge",
        .falling => "falling-edge",
    };
    const key = try stateKey(ctx.arena, op_name, ctx.subject);
    const slot = try getOrPutStateSlot(ctx.gpa, rule, key);
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

/// `(transition BOOL RISING-BRANCH FALLING-BRANCH)` — one boolean edge
/// detector that dispatches both directions. Evaluates BOOL, compares to
/// the prior value stored per (rule, subject), and evaluates exactly one
/// branch: RISING-BRANCH on false→true, FALLING-BRANCH on true→false.
/// Returns nil (and leaves both branches unevaluated) on first sight or
/// no-change. State key is distinct from rising-edge/falling-edge so the
/// three can coexist without aliasing.
fn evalTransition(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 3) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const now = (try eval(ctx, args[0])).isTruthy();

    const key = try stateKey(ctx.arena, "transition", ctx.subject);
    const slot = try getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .number = if (now) 1 else 0 };
        rule.publishes_suppressed += 1;
        return .nil;
    }
    const prev = slot.value_ptr.number != 0;
    slot.value_ptr.* = .{ .number = if (now) 1 else 0 };
    if (!prev and now) return eval(ctx, args[1]);
    if (prev and !now) return eval(ctx, args[2]);
    rule.publishes_suppressed += 1;
    return .nil;
}

const MovingKind = enum { avg, sum, max, min };

/// `(moving-avg N X)` / `moving-sum` / `moving-max` / `moving-min` —
/// push numeric X into an N-wide ring (per rule+subject+op) and return
/// the corresponding aggregate over the current window. The ring is
/// allocated on first sight per slot; `N` must be a positive literal
/// (well, any number ≥ 1; we round down). First call returns the
/// aggregate over just the first sample.
fn callMoving(ctx: *Context, args: []const Value, kind: MovingKind) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const rule = ctx.current_rule orelse return error.TypeMismatch;
    const n_f = try asNumber(args[0]);
    if (n_f < 1) return error.TypeMismatch;
    const n: usize = @intFromFloat(n_f);
    const x = try asNumber(args[1]);

    const op_name = switch (kind) {
        .avg => "moving-avg",
        .sum => "moving-sum",
        .max => "moving-max",
        .min => "moving-min",
    };
    const key = try stateKey(ctx.arena, op_name, ctx.subject);
    const slot = try getOrPutStateSlot(ctx.gpa, rule, key);
    if (!slot.found_existing) {
        slot.value_ptr.* = .{ .ring = try Ring.init(ctx.gpa, n) };
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

/// Coerce a value into a stable byte representation for equality state.
/// Numbers are formatted canonically so `(squelch 1)` and `(squelch 1.0)`
/// compare equal.
fn encodeForState(arena: Allocator, v: Value) EvalError![]const u8 {
    return switch (v) {
        .string, .symbol => |s| s,
        .number => |n| try std.fmt.allocPrint(arena, "{d}", .{n}),
        .boolean => |b| if (b) "true" else "false",
        .nil => "",
        .list, .keyword => error.TypeMismatch,
    };
}

/// Build a state-table key `"op:subject"` in the per-message arena. Every
/// stateful op namespaces its keys by op name so distinct ops on the same
/// subject cannot alias each other's state slot.
fn stateKey(arena: Allocator, op_name: []const u8, subject: []const u8) EvalError![]const u8 {
    return std.fmt.allocPrint(arena, "{s}:{s}", .{ op_name, subject });
}

/// Look up (or create) a state slot. On insert the key is dup'd into `gpa`
/// before calling `getOrPut`, so the map never holds a pointer into the
/// caller's per-message arena (avoids a UAF if a later alloc fails between
/// `getOrPut` and an overwrite, and makes map iteration safe across the
/// next arena reset).
const StateSlot = struct {
    value_ptr: *StateEntry,
    found_existing: bool,
};

fn getOrPutStateSlot(gpa: Allocator, rule: *Rule, key: []const u8) EvalError!StateSlot {
    if (rule.state.getEntry(key)) |entry| {
        return .{ .value_ptr = entry.value_ptr, .found_existing = true };
    }
    const owned_key = try gpa.dupe(u8, key);
    errdefer gpa.free(owned_key);
    const gop = try rule.state.getOrPut(gpa, owned_key);
    // getEntry missed, so getOrPut can only be !found_existing here.
    std.debug.assert(!gop.found_existing);
    gop.key_ptr.* = owned_key;
    gop.value_ptr.* = .empty;
    return .{ .value_ptr = gop.value_ptr, .found_existing = false };
}

/// Look up (or create) a bytes-typed state slot. Returns true iff the slot
/// was absent or differed from `encoded` — and updates it to `encoded` in
/// either case. The backing ArrayList is reused across messages
/// (clear + appendSlice, not free + dupe).
fn stateEqualsOrStore(
    gpa: Allocator,
    rule: *Rule,
    key: []const u8,
    encoded: []const u8,
) EvalError!bool {
    const slot = try getOrPutStateSlot(gpa, rule, key);
    if (!slot.found_existing) {
        var buf: std.ArrayList(u8) = .empty;
        try buf.appendSlice(gpa, encoded);
        slot.value_ptr.* = .{ .bytes = buf };
        return true;
    }
    const prev = &slot.value_ptr.bytes;
    if (std.mem.eql(u8, prev.items, encoded)) return false;
    prev.clearRetainingCapacity();
    try prev.appendSlice(gpa, encoded);
    return true;
}

fn asString(v: Value) EvalError![]const u8 {
    return switch (v) {
        .string => |s| s,
        .symbol => |s| s,
        else => error.TypeMismatch,
    };
}

fn asNumber(v: Value) EvalError!f64 {
    return switch (v) {
        .number => |n| n,
        .string => |s| std.fmt.parseFloat(f64, s) catch error.TypeMismatch,
        else => error.TypeMismatch,
    };
}

fn valueEql(a: Value, b: Value) bool {
    const TagA = @typeInfo(Value).@"union".tag_type.?;
    if (@as(TagA, a) != @as(TagA, b)) return false;
    return switch (a) {
        .nil => true,
        .boolean => |x| x == b.boolean,
        .number => |x| x == b.number,
        .symbol => |s| std.mem.eql(u8, s, b.symbol),
        .keyword => |s| std.mem.eql(u8, s, b.keyword),
        .string => |s| std.mem.eql(u8, s, b.string),
        .list => false, // lists aren't comparable in our dialect
    };
}

// --- Tests ---------------------------------------------------------------

const testing = std.testing;

const TestPublisher = struct {
    buf: std.ArrayList(Emitted) = .empty,
    alloc: Allocator,

    const Emitted = struct { subject: []const u8, payload: []const u8 };

    fn publisher(self: *TestPublisher) Publisher {
        return .{ .ctx = self, .publish_fn = handle };
    }

    fn handle(ctx: *anyopaque, subj: []const u8, payload: []const u8) anyerror!void {
        const self: *TestPublisher = @ptrCast(@alignCast(ctx));
        try self.buf.append(self.alloc, .{
            .subject = try self.alloc.dupe(u8, subj),
            .payload = try self.alloc.dupe(u8, payload),
        });
    }
};

test "rule with publish side effect" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(on "sensors.*"
        \\  (when (> (payload-float) 10.0)
        \\    (publish (subject-append "high") (payload))))
    ;
    // note: payload is a symbol in our grammar, not a call; adjust rule.
    _ = src;

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (> payload-float 10.0)
        \\    (publish (subject-append "high") payload)))
    );
    try testing.expectEqual(@as(usize, 1), rules.len);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.temp",
        .payload = "42.5",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = arena,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("sensors.temp.high", tp.buf.items[0].subject);
    try testing.expectEqualStrings("42.5", tp.buf.items[0].payload);
}

test "rule does not fire when filter mismatches" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena, "(on \"foo.bar\" (publish \"quux\" payload))");
    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "foo.baz",
        .payload = "x",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = arena,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 0), tp.buf.items.len);
}

test "rule threshold below does not publish" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (> payload-float 10.0)
        \\    (publish (subject-append "high") payload)))
    );
    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.temp",
        .payload = "1.5",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = arena,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 0), tp.buf.items.len);
}

test "round and quantize are pure" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Rule only fires if (round 1 42.567) == 42.6 and (quantize 0.5 42.3) == 42.5.
    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (and (= (round 1 42.567) 42.6)
        \\             (= (quantize 0.5 42.3) 42.5))
        \\    (publish "ok" payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.temp",
        .payload = "x",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
}

test "squelch only emits on change" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (squelch (round 1 payload-float))
        \\    (publish (subject-append "changed") payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };

    const feed = [_][]const u8{
        "42.01", // first sight → emit
        "42.04", // rounds to 42.0 → same → suppress
        "42.08", // rounds to 42.1 → change → emit
        "42.12", // rounds to 42.1 → same → suppress
        "43.00", // → emit
    };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
}

test "deadband suppresses small deltas" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (deadband 0.5 payload-float)
        \\    (publish (subject-append "stable") payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{
        "10.0", // first → emit
        "10.2", // |0.2| < 0.5 → suppress
        "10.4", // still within 0.5 of 10.0 → suppress
        "10.6", // |0.6| ≥ 0.5 → emit, update anchor to 10.6
        "10.7", // |0.1| < 0.5 → suppress
        "11.2", // |0.6| ≥ 0.5 → emit
    };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
}

test "squelch state is per-subject" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (squelch payload)
        \\    (publish (subject-append "changed") payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_]struct { s: []const u8, p: []const u8 }{
        .{ .s = "sensors.a", .p = "1" }, // a first → emit
        .{ .s = "sensors.b", .p = "1" }, // b first → emit (different subject)
        .{ .s = "sensors.a", .p = "1" }, // a same → suppress
        .{ .s = "sensors.b", .p = "2" }, // b change → emit
    };
    for (feed) |m| {
        var ctx: Context = .{
            .subject = m.s,
            .payload = m.p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
}

test "moving-avg smooths a stream" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Rule fires when the 3-sample moving avg crosses 10.
    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (> (moving-avg 3 payload-float) 10.0)
        \\    (publish (subject-append "hot") payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // Averages: 5, 7.5, 10, 12.5 (5+15+15/3=) wait -> push 15: avg(5,10,15)=10 → not > 10
    // push 20: avg(10,15,20)=15 → > 10 → emit.
    const feed = [_][]const u8{ "5", "10", "15", "20" };
    var n_emits: usize = 0;
    for (feed) |p| {
        tp.buf.clearRetainingCapacity();
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
        n_emits += tp.buf.items.len;
    }
    // Only the last sample's window crosses the threshold.
    try testing.expectEqual(@as(usize, 1), n_emits);
}

test "moving-max and moving-min track window extremes" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (> (- (moving-max 3 payload-float) (moving-min 3 payload-float)) 5.0)
        \\    (publish (subject-append "spread") payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // values: 1, 2, 3, 10 → last window (2,3,10) spread = 8 > 5 → emit.
    const feed = [_][]const u8{ "1", "2", "3", "10" };
    var n_emits: usize = 0;
    for (feed) |p| {
        tp.buf.clearRetainingCapacity();
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
        n_emits += tp.buf.items.len;
    }
    try testing.expectEqual(@as(usize, 1), n_emits);
}

test "moving-avg composes with deadband" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Smooth then gate: only emit if the smoothed value drifts by >= 1.0.
    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (deadband 1.0 (moving-avg 3 payload-float))
        \\    (publish (subject-append "drift") payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // Averages:
    // push 10 → avg=10       → first → emit
    // push 10 → avg=10       → Δ=0   → suppress
    // push 10 → avg=10       → Δ=0   → suppress
    // push 13 → avg=11       → Δ=1   → emit (update anchor to 11)
    // push 13 → avg=12       → Δ=1   → emit (anchor → 12)
    const feed = [_][]const u8{ "10", "10", "10", "13", "13" };
    var n_emits: usize = 0;
    for (feed) |p| {
        tp.buf.clearRetainingCapacity();
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
        n_emits += tp.buf.items.len;
    }
    try testing.expectEqual(@as(usize, 3), n_emits);
}

test "thread -> with round, squelch, publish-to" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> payload-float
        \\      (round 1)
        \\      (squelch)
        \\      (publish-to (subject-append "stable"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{ "42.01", "42.04", "42.08", "42.12", "43.00" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    // 42.01→42.0 emit, 42.04→42.0 suppress, 42.08→42.1 emit,
    // 42.12→42.1 suppress, 43.00→43.0 emit. Payload carries the
    // rounded number, not the raw string.
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
    try testing.expectEqualStrings("sensors.temp.stable", tp.buf.items[0].subject);
    try testing.expectEqualStrings("42", tp.buf.items[0].payload);
    try testing.expectEqualStrings("42.1", tp.buf.items[1].payload);
    try testing.expectEqualStrings("43", tp.buf.items[2].payload);
}

test "thread -> expands moving-avg pipeline" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> payload-float
        \\      (moving-avg 3)
        \\      (deadband 1.0)
        \\      (publish-to (subject-append "drift"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{ "10", "10", "10", "13", "13" };
    var n_emits: usize = 0;
    for (feed) |p| {
        tp.buf.clearRetainingCapacity();
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
        n_emits += tp.buf.items.len;
    }
    try testing.expectEqual(@as(usize, 3), n_emits);
}

test "rising-edge and falling-edge fire once per transition" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Alert on rising edge, all-clear on falling edge of payload > 10.
    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> (> payload-float 10.0)
        \\      (rising-edge)
        \\      (publish-to (subject-append "alert"))))
        \\(on "sensors.*"
        \\  (-> (> payload-float 10.0)
        \\      (falling-edge)
        \\      (publish-to (subject-append "ok"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // 5 (first, no edge) → 6 (still false, no edge) → 15 (rising) → 20 (still true, no edge)
    // → 8 (falling) → 7 (still false, no edge) → 12 (rising again).
    const feed = [_][]const u8{ "5", "6", "15", "20", "8", "7", "12" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
    try testing.expectEqualStrings("sensors.temp.alert", tp.buf.items[0].subject);
    try testing.expectEqualStrings("sensors.temp.ok", tp.buf.items[1].subject);
    try testing.expectEqualStrings("sensors.temp.alert", tp.buf.items[2].subject);
}

test "transition dispatches rising and falling in one rule" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (transition (> payload-float 10.0)
        \\    (publish-to (subject-append "alert") "up")
        \\    (publish-to (subject-append "ok") "down")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{ "5", "6", "15", "20", "8", "7", "12" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
    try testing.expectEqualStrings("sensors.temp.alert", tp.buf.items[0].subject);
    try testing.expectEqualStrings("up", tp.buf.items[0].payload);
    try testing.expectEqualStrings("sensors.temp.ok", tp.buf.items[1].subject);
    try testing.expectEqualStrings("down", tp.buf.items[1].payload);
    try testing.expectEqualStrings("sensors.temp.alert", tp.buf.items[2].subject);
}

test "contains? and str-concat" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (when (contains? payload "alert")
        \\    (publish "events.alerts" (str-concat subject ": " payload))))
    );
    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "log.app",
        .payload = "something alert here",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = arena,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("events.alerts", tp.buf.items[0].subject);
    try testing.expectEqualStrings("log.app: something alert here", tp.buf.items[0].payload);
}

test "starts-with? and ends-with?" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (when (and (starts-with? subject "sensors.") (ends-with? payload "!"))
        \\    (publish "hit" payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.temp",
        .payload = "boom!",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);

    ctx.subject = "other.thing";
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);

    ctx.subject = "sensors.temp";
    ctx.payload = "quiet";
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
}

test "subject-token extracts Nth dot-separated token" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (publish (str-concat "room." (subject-token 1)) payload))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.kitchen.temp",
        .payload = "72",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("room.kitchen", tp.buf.items[0].subject);
}

test "payload-int parses integer payloads" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (when (> payload-int 100)
        \\    (publish "big" payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "x",
        .payload = "200",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);

    ctx.payload = "50";
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
}

test "clamp, min, max, abs, sign" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (do
        \\    (-> payload-float (clamp 0 10) (publish-to "c"))
        \\    (-> (min 1 2 3) (publish-to "m"))
        \\    (-> (max 1 5 3) (publish-to "x"))
        \\    (-> (abs -7) (publish-to "a"))
        \\    (-> (sign -42) (publish-to "s"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "z",
        .payload = "99",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 5), tp.buf.items.len);
    try testing.expectEqualStrings("10", tp.buf.items[0].payload);
    try testing.expectEqualStrings("1", tp.buf.items[1].payload);
    try testing.expectEqualStrings("5", tp.buf.items[2].payload);
    try testing.expectEqualStrings("7", tp.buf.items[3].payload);
    try testing.expectEqualStrings("-1", tp.buf.items[4].payload);
}

test "changed? fires only on distinct values" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (when (changed? payload)
        \\    (publish "c" payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "x",
        .payload = "a",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try run(rules, &ctx);
    ctx.payload = "b";
    try run(rules, &ctx);
    try run(rules, &ctx);
    ctx.payload = "a";
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
}

test "squelch and deadband on same subject do not alias" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Both gates on the same subject: squelch on the raw payload, deadband
    // on the float. Before the per-op key prefix, they shared one slot and
    // clobbered each other's state on every message.
    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (and (squelch payload) (deadband 0.5 payload-float))
        \\    (publish "hit" payload)))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // 10.0 first → emit
    // 10.0 same string → squelch suppresses
    // 10.2 → squelch passes (changed), deadband suppresses (|0.2|<0.5)
    // 10.6 → squelch passes, deadband emits (|0.6|>=0.5)
    const feed = [_][]const u8{ "10.0", "10.0", "10.2", "10.6" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 2), tp.buf.items.len);
}

test "delta returns difference from last value" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (-> (delta payload-float) (publish-to "d")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "x",
        .payload = "10",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    ctx.payload = "13";
    try run(rules, &ctx);
    ctx.payload = "12";
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
    try testing.expectEqualStrings("0", tp.buf.items[0].payload);
    try testing.expectEqualStrings("3", tp.buf.items[1].payload);
    try testing.expectEqualStrings("-1", tp.buf.items[2].payload);
}
