const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");
const subject_mod = @import("subject.zig");

pub const Value = sexpr.Value;

/// A compiled rule: a subject filter and a body to evaluate when the filter
/// matches an incoming publish.
pub const Rule = struct {
    filter: []const u8,
    body: Value,
};

pub const LoadError = error{
    InvalidRuleForm,
    UnknownTopLevel,
} || sexpr.ParseError || subject_mod.Error;

pub fn loadRules(arena: Allocator, source: []const u8) LoadError![]const Rule {
    const forms = try sexpr.parseAll(arena, source);
    var out: std.ArrayList(Rule) = .empty;
    for (forms) |f| {
        if (f != .list) return error.InvalidRuleForm;
        const items = f.list;
        if (items.len < 2 or items[0] != .symbol) return error.UnknownTopLevel;
        if (!std.mem.eql(u8, items[0].symbol, "on")) return error.UnknownTopLevel;
        if (items.len != 3 or items[1] != .string) return error.InvalidRuleForm;
        try subject_mod.validateFilter(items[1].string);
        try out.append(arena, .{ .filter = items[1].string, .body = items[2] });
    }
    return try out.toOwnedSlice(arena);
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
};

pub fn run(rules: []const Rule, ctx: *Context) !void {
    for (rules) |rule| {
        if (!subject_mod.matches(rule.filter, ctx.subject)) continue;
        _ = try eval(ctx, rule.body);
    }
}

fn eval(ctx: *Context, v: Value) EvalError!Value {
    return switch (v) {
        .nil, .boolean, .number, .string => v,
        .symbol => |s| evalSymbol(ctx, s),
        .list => |items| evalCall(ctx, items),
    };
}

fn evalSymbol(ctx: *Context, name: []const u8) EvalError!Value {
    if (std.mem.eql(u8, name, "subject")) return .{ .string = ctx.subject };
    if (std.mem.eql(u8, name, "payload")) return .{ .string = ctx.payload };
    if (std.mem.eql(u8, name, "payload-float")) {
        const n = std.fmt.parseFloat(f64, ctx.payload) catch return error.TypeMismatch;
        return .{ .number = n };
    }
    return error.UnknownSymbol;
}

fn evalCall(ctx: *Context, items: []const Value) EvalError!Value {
    if (items.len == 0) return .nil;
    if (items[0] != .symbol) return error.TypeMismatch;
    const op = items[0].symbol;
    const args = items[1..];

    // Special forms that don't pre-evaluate their args.
    if (std.mem.eql(u8, op, "if")) return evalIf(ctx, args);
    if (std.mem.eql(u8, op, "when")) return evalWhen(ctx, args);
    if (std.mem.eql(u8, op, "and")) return evalAnd(ctx, args);
    if (std.mem.eql(u8, op, "or")) return evalOr(ctx, args);
    if (std.mem.eql(u8, op, "do")) return evalDo(ctx, args);

    // Ordinary functions: evaluate args left-to-right, then dispatch.
    const evaled = try ctx.arena.alloc(Value, args.len);
    for (args, 0..) |a, i| evaled[i] = try eval(ctx, a);

    if (std.mem.eql(u8, op, "publish")) return callPublish(ctx, evaled);
    if (std.mem.eql(u8, op, "subject-append")) return callSubjectAppend(ctx, evaled);
    if (std.mem.eql(u8, op, "str-concat")) return callStrConcat(ctx, evaled);
    if (std.mem.eql(u8, op, "not")) return callNot(evaled);
    if (std.mem.eql(u8, op, "=")) return callEq(evaled);
    if (std.mem.eql(u8, op, ">")) return callCmp(evaled, .gt);
    if (std.mem.eql(u8, op, "<")) return callCmp(evaled, .lt);
    if (std.mem.eql(u8, op, ">=")) return callCmp(evaled, .ge);
    if (std.mem.eql(u8, op, "<=")) return callCmp(evaled, .le);
    if (std.mem.eql(u8, op, "+")) return callArith(evaled, .add);
    if (std.mem.eql(u8, op, "-")) return callArith(evaled, .sub);
    if (std.mem.eql(u8, op, "*")) return callArith(evaled, .mul);
    if (std.mem.eql(u8, op, "/")) return callArith(evaled, .div);
    if (std.mem.eql(u8, op, "contains?")) return callContains(evaled);

    return error.UnknownSymbol;
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

fn callPublish(ctx: *Context, args: []const Value) EvalError!Value {
    if (args.len != 2) return error.ArityMismatch;
    const subj = try asString(args[0]);
    const payload = try asString(args[1]);
    subject_mod.validatePublish(subj) catch return error.InvalidSubject;
    ctx.publisher.publish(subj, payload) catch return error.PublishFailed;
    return .nil;
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
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 0), tp.buf.items.len);
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
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("events.alerts", tp.buf.items[0].subject);
    try testing.expectEqualStrings("log.app: something alert here", tp.buf.items[0].payload);
}
