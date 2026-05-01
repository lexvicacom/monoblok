//! Integration tests for the patchbay evaluator. Live in a sibling file
//! rather than inline at the bottom of eval.zig so the implementation
//! stays browsable; they reach `eval`, `state`, etc. only through the
//! public API.

const std = @import("std");
const testing = std.testing;
const Allocator = std.mem.Allocator;

const eval = @import("eval.zig");
const Context = eval.Context;
const Publisher = eval.Publisher;
const loadRules = eval.loadRules;
const deinitRules = eval.deinitRules;
const run = eval.run;

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
        \\    (publish! (subject-append "high") (payload))))
    ;
    // note: payload is a symbol in our grammar, not a call; adjust rule.
    _ = src;

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (when (> payload-float 10.0)
        \\    (publish! (subject-append "high") payload)))
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

    const rules = try loadRules(arena, "(on \"foo.bar\" (publish! \"quux\" payload))");
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
        \\    (publish! (subject-append "high") payload)))
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
        \\    (publish! "ok" payload)))
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
        \\    (publish! (subject-append "changed") payload)))
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
        \\    (publish! (subject-append "stable") payload)))
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
        \\    (publish! (subject-append "changed") payload)))
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
        \\    (publish! (subject-append "hot") payload)))
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
        \\    (publish! (subject-append "spread") payload)))
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
        \\    (publish! (subject-append "drift") payload)))
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
        \\      (publish! (subject-append "stable"))))
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
        \\      (publish! (subject-append "drift"))))
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
        \\      (publish! (subject-append "alert"))))
        \\(on "sensors.*"
        \\  (-> (> payload-float 10.0)
        \\      (falling-edge)
        \\      (publish! (subject-append "ok"))))
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
        \\    (publish! (subject-append "alert") "up")
        \\    (publish! (subject-append "ok") "down")))
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
        \\    (publish! "events.alerts" (str-concat subject ": " payload))))
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
        \\    (publish! "hit" payload)))
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
        \\  (publish! (str-concat "room." (subject-token 1)) payload))
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
        \\    (publish! "big" payload)))
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
        \\    (-> payload-float (clamp 0 10) (publish! "c"))
        \\    (-> (min 1 2 3) (publish! "m"))
        \\    (-> (max 1 5 3) (publish! "x"))
        \\    (-> (abs -7) (publish! "a"))
        \\    (-> (sign -42) (publish! "s"))))
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
        \\    (publish! "c" payload)))
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
        \\    (publish! "hit" payload)))
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
        \\  (-> (delta payload-float) (publish! "d")))
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

test "hold-off suppresses rapid re-fires within the interval" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> payload-float
        \\      (hold-off 500)
        \\      (publish! (subject-append "gated"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };

    const Frame = struct { now_ms: i64, payload: []const u8 };
    const feed = [_]Frame{
        .{ .now_ms = 1000, .payload = "1" }, // first → emit
        .{ .now_ms = 1100, .payload = "2" }, // +100ms → suppress
        .{ .now_ms = 1499, .payload = "3" }, // +499ms → suppress
        .{ .now_ms = 1500, .payload = "4" }, // +500ms exactly → emit
        .{ .now_ms = 1700, .payload = "5" }, // +200ms → suppress
        .{ .now_ms = 2001, .payload = "6" }, // +501ms → emit
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = "sensors.temp",
            .payload = f.payload,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
    try testing.expectEqualStrings("1", tp.buf.items[0].payload);
    try testing.expectEqualStrings("4", tp.buf.items[1].payload);
    try testing.expectEqualStrings("6", tp.buf.items[2].payload);
}

test "hold-off state is per-subject" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> payload-float
        \\      (hold-off 500)
        \\      (publish! (subject-append "gated"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };

    const Frame = struct { subject: []const u8, now_ms: i64 };
    const feed = [_]Frame{
        .{ .subject = "sensors.a", .now_ms = 1000 }, // a first → emit
        .{ .subject = "sensors.b", .now_ms = 1050 }, // b first → emit, unaffected by a
        .{ .subject = "sensors.a", .now_ms = 1200 }, // a +200 → suppress
        .{ .subject = "sensors.b", .now_ms = 1600 }, // b +550 → emit
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = f.subject,
            .payload = "1",
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);
}

test "json-get extracts numeric field and threads through pipeline" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> payload
        \\      (json-get "temp")
        \\      (round 1)
        \\      (publish! (subject-append "temp"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.x",
        .payload = "{\"temp\":12.04,\"hum\":80}",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("sensors.x.temp", tp.buf.items[0].subject);
    try testing.expectEqualStrings("12", tp.buf.items[0].payload);
}

test "json-get returns nil for missing key, malformed payload, nested values" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // publish-to short-circuits on nil, so a successful pipeline run must
    // be guarded by a key that resolves to a number.
    const rules = try loadRules(arena,
        \\(on "x.*"
        \\  (-> payload
        \\      (json-get "v")
        \\      (publish! (subject-append "out"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const cases = [_]struct { subj: []const u8, payload: []const u8 }{
        .{ .subj = "x.miss", .payload = "{\"other\":1}" },
        .{ .subj = "x.bad", .payload = "not json at all" },
        .{ .subj = "x.nest", .payload = "{\"v\":{\"inner\":1}}" },
        .{ .subj = "x.nul", .payload = "{\"v\":null}" },
        .{ .subj = "x.ok", .payload = "{\"v\":42}" },
    };
    for (cases) |c| {
        var ctx: Context = .{
            .subject = c.subj,
            .payload = c.payload,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("x.ok.out", tp.buf.items[0].subject);
    try testing.expectEqualStrings("42", tp.buf.items[0].payload);
}

test "json-get extracts strings (escapes decoded) and booleans" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on ">"
        \\  (do
        \\    (publish! "out.s" (json-get "s" payload))
        \\    (publish! "out.b" (json-get "b" payload))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "in",
        .payload = "{\"s\":\"hi\\nthere\",\"b\":true}",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 2), tp.buf.items.len);
    try testing.expectEqualStrings("out.s", tp.buf.items[0].subject);
    try testing.expectEqualStrings("hi\nthere", tp.buf.items[0].payload);
    try testing.expectEqualStrings("out.b", tp.buf.items[1].subject);
    try testing.expectEqualStrings("true", tp.buf.items[1].payload);
}

test "json-demux fans a flat object out onto sub-subjects" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (json-demux! "temp" "hum" "missing" "nested" payload))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "sensors.foo",
        .payload = "{\"temp\":12.5,\"hum\":80,\"nested\":{\"x\":1}}",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 2), tp.buf.items.len);
    try testing.expectEqualStrings("sensors.foo.temp", tp.buf.items[0].subject);
    try testing.expectEqualStrings("12.5", tp.buf.items[0].payload);
    try testing.expectEqualStrings("sensors.foo.hum", tp.buf.items[1].subject);
    try testing.expectEqualStrings("80", tp.buf.items[1].payload);
}

test "bar emits open/high/low/close every N ticks" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "MARKET.*"
        \\  (bar! 4 payload-float))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // Bar 1 over [10, 12, 9, 11]:  o=10 h=12 l=9 c=11
    // Bar 2 over [11, 8, 13, 10]:  o=11 h=13 l=8 c=10
    // Then one extra tick (15) that opens a new bar but doesn't close it.
    const feed = [_][]const u8{ "10", "12", "9", "11", "11", "8", "13", "10", "15" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "MARKET.AAPL",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }

    // Two closed bars × 4 fields = 8 publishes.
    try testing.expectEqual(@as(usize, 8), tp.buf.items.len);
    try testing.expectEqualStrings("MARKET.AAPL.bar.open", tp.buf.items[0].subject);
    try testing.expectEqualStrings("10", tp.buf.items[0].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.high", tp.buf.items[1].subject);
    try testing.expectEqualStrings("12", tp.buf.items[1].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.low", tp.buf.items[2].subject);
    try testing.expectEqualStrings("9", tp.buf.items[2].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.close", tp.buf.items[3].subject);
    try testing.expectEqualStrings("11", tp.buf.items[3].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.open", tp.buf.items[4].subject);
    try testing.expectEqualStrings("11", tp.buf.items[4].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.high", tp.buf.items[5].subject);
    try testing.expectEqualStrings("13", tp.buf.items[5].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.low", tp.buf.items[6].subject);
    try testing.expectEqualStrings("8", tp.buf.items[6].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.close", tp.buf.items[7].subject);
    try testing.expectEqualStrings("10", tp.buf.items[7].payload);
}

test "bar state is per-subject" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "MARKET.*"
        \\  (bar! 2 payload-float))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_]struct { s: []const u8, p: []const u8 }{
        .{ .s = "MARKET.A", .p = "10" }, // A: open
        .{ .s = "MARKET.B", .p = "20" }, // B: open (independent)
        .{ .s = "MARKET.A", .p = "11" }, // A: closes bar (o=10 h=11 l=10 c=11)
        .{ .s = "MARKET.B", .p = "21" }, // B: closes bar (o=20 h=21 l=20 c=21)
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
    try testing.expectEqual(@as(usize, 8), tp.buf.items.len);
    // First bar is A's, second is B's, each in field order.
    try testing.expectEqualStrings("MARKET.A.bar.close", tp.buf.items[3].subject);
    try testing.expectEqualStrings("11", tp.buf.items[3].payload);
    try testing.expectEqualStrings("MARKET.B.bar.close", tp.buf.items[7].subject);
    try testing.expectEqualStrings("21", tp.buf.items[7].payload);
}

test "count fires every call when unconditional, per (rule, subject)" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "events.>"
        \\  (count!))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_]struct { s: []const u8, p: []const u8 }{
        .{ .s = "events.a", .p = "x" },
        .{ .s = "events.a", .p = "y" },
        .{ .s = "events.b", .p = "z" },
        .{ .s = "events.a", .p = "w" },
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

    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
    try testing.expectEqualStrings("events.a.count", tp.buf.items[0].subject);
    try testing.expectEqualStrings("1", tp.buf.items[0].payload);
    try testing.expectEqualStrings("events.a.count", tp.buf.items[1].subject);
    try testing.expectEqualStrings("2", tp.buf.items[1].payload);
    try testing.expectEqualStrings("events.b.count", tp.buf.items[2].subject);
    try testing.expectEqualStrings("1", tp.buf.items[2].payload);
    try testing.expectEqualStrings("events.a.count", tp.buf.items[3].subject);
    try testing.expectEqualStrings("3", tp.buf.items[3].payload);
}

test "count with predicate only increments on truthy" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "events.>"
        \\  (count! (contains? payload "ERROR")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{ "ok", "ERROR: oops", "ok", "ERROR: again", "fine" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "events.svc",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }

    try testing.expectEqual(@as(usize, 2), tp.buf.items.len);
    try testing.expectEqualStrings("events.svc.count", tp.buf.items[0].subject);
    try testing.expectEqualStrings("1", tp.buf.items[0].payload);
    try testing.expectEqualStrings("events.svc.count", tp.buf.items[1].subject);
    try testing.expectEqualStrings("2", tp.buf.items[1].payload);
}

test "moving-avg with window-ms evicts by age" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // 1-second sliding window. Anything older than now-1000ms leaves.
    const rules = try loadRules(arena,
        \\(on "sensors.*"
        \\  (-> payload-float
        \\      (moving-avg :ms 1000)
        \\      (publish! (subject-append "avg"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };

    const Frame = struct { now_ms: i64, payload: []const u8 };
    // t=0    push 10 → avg=10
    // t=500  push 20 → avg=15 (both in window)
    // t=900  push 30 → avg=20 (all three)
    // t=1100 push 40 → t=0 evicted; avg over 20,30,40 = 30
    // t=2200 push 50 → only 50 left in 1.2..2.2; avg=50
    const feed = [_]Frame{
        .{ .now_ms = 0, .payload = "10" },
        .{ .now_ms = 500, .payload = "20" },
        .{ .now_ms = 900, .payload = "30" },
        .{ .now_ms = 1100, .payload = "40" },
        .{ .now_ms = 2200, .payload = "50" },
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = "sensors.t",
            .payload = f.payload,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 5), tp.buf.items.len);
    try testing.expectEqualStrings("10", tp.buf.items[0].payload);
    try testing.expectEqualStrings("15", tp.buf.items[1].payload);
    try testing.expectEqualStrings("20", tp.buf.items[2].payload);
    try testing.expectEqualStrings("30", tp.buf.items[3].payload);
    try testing.expectEqualStrings("50", tp.buf.items[4].payload);
}

test "ticks and window-ms keep distinct slots on the same rule" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Two moving-avgs over the same input: a tick window and a time
    // window. They mustn't share state.
    const rules = try loadRules(arena,
        \\(on "x.*"
        \\  (do
        \\    (-> payload-float (moving-avg 2)     (publish! "tick.avg"))
        \\    (-> payload-float (moving-avg :ms 1000) (publish! "time.avg"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const Frame = struct { now_ms: i64, payload: []const u8 };
    const feed = [_]Frame{
        .{ .now_ms = 0,    .payload = "10" }, // ticks: 10  | time: 10
        .{ .now_ms = 500,  .payload = "20" }, // ticks: 15  | time: 15
        .{ .now_ms = 1500, .payload = "30" }, // ticks: 25 (drop 10) | time: 25 (drop 10)
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = "x.a",
            .payload = f.payload,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 6), tp.buf.items.len);
    // Order alternates tick.avg, time.avg per input.
    try testing.expectEqualStrings("10", tp.buf.items[0].payload);
    try testing.expectEqualStrings("10", tp.buf.items[1].payload);
    try testing.expectEqualStrings("15", tp.buf.items[2].payload);
    try testing.expectEqualStrings("15", tp.buf.items[3].payload);
    try testing.expectEqualStrings("25", tp.buf.items[4].payload);
    try testing.expectEqualStrings("25", tp.buf.items[5].payload);
}

test "bar with window-ms closes on the next tick after the boundary" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // 1s aligned bars. Boundary at floor(now/1000)*1000.
    const rules = try loadRules(arena,
        \\(on "MARKET.*"
        \\  (bar! :ms 1000 payload-float))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const Frame = struct { now_ms: i64, payload: []const u8 };
    // Window [1000,2000) gets {10,12,9,11}; window [2000,3000) starts
    // with the next tick (15) which crosses the boundary and emits the
    // [1000,2000) close.
    const feed = [_]Frame{
        .{ .now_ms = 1100, .payload = "10" }, // open bar at 1000
        .{ .now_ms = 1300, .payload = "12" },
        .{ .now_ms = 1700, .payload = "9" },
        .{ .now_ms = 1900, .payload = "11" }, // last in [1000,2000)
        .{ .now_ms = 2100, .payload = "15" }, // crosses → close prior bar
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = "MARKET.AAPL",
            .payload = f.payload,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    // o=10 h=12 l=9 c=11 — close = last sample seen in the window (11),
    // not the boundary-crossing sample (15).
    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
    try testing.expectEqualStrings("MARKET.AAPL.bar.open", tp.buf.items[0].subject);
    try testing.expectEqualStrings("10", tp.buf.items[0].payload);
    try testing.expectEqualStrings("12", tp.buf.items[1].payload);
    try testing.expectEqualStrings("9", tp.buf.items[2].payload);
    try testing.expectEqualStrings("11", tp.buf.items[3].payload);
}

test "tickClocks closes a stalled time bar" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const loaded = try loadRules(arena,
        \\(on "MARKET.*"
        \\  (bar! :ms 1000 payload-float))
    );
    defer deinitRules(loaded, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // Open a bar in window [1000,2000) and never push another tick.
    var ctx: Context = .{
        .subject = "MARKET.AAPL",
        .payload = "10",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .now_ms = 1500,
    };
    try run(loaded, &ctx);
    try testing.expectEqual(@as(usize, 0), tp.buf.items.len);

    // Walker fires at t=2500: window [1000,2000) is fully past, close it.
    try eval.tickClocks(arena, loaded, 2500, tp.publisher());
    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
    try testing.expectEqualStrings("MARKET.AAPL.bar.open", tp.buf.items[0].subject);
    try testing.expectEqualStrings("10", tp.buf.items[0].payload);
    try testing.expectEqualStrings("MARKET.AAPL.bar.close", tp.buf.items[3].subject);
    try testing.expectEqualStrings("10", tp.buf.items[3].payload);

    // A second walker tick on an empty bar must not re-emit.
    try eval.tickClocks(arena, loaded, 9000, tp.publisher());
    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
}

test "rate over a 1s window reports events per second" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "x"
        \\  (-> payload-float
        \\      (rate :ms 1000)
        \\      (publish! "hz")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const Frame = struct { now_ms: i64 };
    // Five events spread across 800ms → all live → 5 events / 1s = 5 Hz
    // on the last call.
    const feed = [_]Frame{
        .{ .now_ms = 100 },
        .{ .now_ms = 300 },
        .{ .now_ms = 500 },
        .{ .now_ms = 700 },
        .{ .now_ms = 900 },
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = "x",
            .payload = "0",
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 5), tp.buf.items.len);
    try testing.expectEqualStrings("1", tp.buf.items[0].payload);
    try testing.expectEqualStrings("2", tp.buf.items[1].payload);
    try testing.expectEqualStrings("3", tp.buf.items[2].payload);
    try testing.expectEqualStrings("4", tp.buf.items[3].payload);
    try testing.expectEqualStrings("5", tp.buf.items[4].payload);

    // Skip ahead: at t=2500 only one call lands; the prior five are
    // all > 1s old → 1 Hz.
    var ctx: Context = .{
        .subject = "x",
        .payload = "0",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .now_ms = 2500,
    };
    try run(rules, &ctx);
    try testing.expectEqualStrings("1", tp.buf.items[5].payload);
}

test "rate rejects (ticks N) — needs a time window" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "x" (-> payload-float (rate 10) (publish! "hz")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "x",
        .payload = "0",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .now_ms = 0,
    };
    try testing.expectError(error.TypeMismatch, run(rules, &ctx));
}

test "percentile interpolates between window samples" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // p99 latency over the last 5 samples.
    const rules = try loadRules(arena,
        \\(on "lat"
        \\  (-> payload-float
        \\      (percentile 5 0.5)
        \\      (publish! "p50")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{ "10", "20", "30", "40", "50" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "lat",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    // After all 5 samples, sorted = [10,20,30,40,50], p50 = 30.
    try testing.expectEqual(@as(usize, 5), tp.buf.items.len);
    try testing.expectEqualStrings("30", tp.buf.items[4].payload);
}

test "median is sugar for percentile 0.5" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const rules = try loadRules(arena,
        \\(on "x" (-> payload-float (median 4) (publish! "med")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // [3, 7, 1, 9] sorted [1,3,7,9] → median between idx 1.5 → (3+7)/2 = 5.
    const feed = [_][]const u8{ "3", "7", "1", "9" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "x",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqualStrings("5", tp.buf.items[3].payload);
}

test "stddev and variance over a tick window" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // population variance for [2, 4, 4, 4, 5, 5, 7, 9] is 4, stddev = 2.
    const rules = try loadRules(arena,
        \\(on "x"
        \\  (do
        \\    (-> payload-float (stddev 8)   (publish! "sd"))
        \\    (-> payload-float (variance 8) (publish! "var"))))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const feed = [_][]const u8{ "2", "4", "4", "4", "5", "5", "7", "9" };
    for (feed) |p| {
        var ctx: Context = .{
            .subject = "x",
            .payload = p,
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    // Last pair of emits is the final 8-sample window.
    const last_sd = tp.buf.items[tp.buf.items.len - 2].payload;
    const last_var = tp.buf.items[tp.buf.items.len - 1].payload;
    try testing.expectEqualStrings("2", last_sd);
    try testing.expectEqualStrings("4", last_var);
}

test "throttle passes at most MAX per ticks window" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Up to 2 passes per 5-tick window.
    const rules = try loadRules(arena,
        \\(on "x"
        \\  (-> payload
        \\      (throttle 5 2)
        \\      (publish! "out")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    // Calls 1,2 pass; 3,4,5 suppressed (window has 2 passes already);
    // call 6 evicts call 1 (still 1 pass in window) → pass; call 7
    // window now holds {pass@2, supp@3, supp@4, supp@5, pass@6} → 2
    // passes → suppress; call 8 evicts pass@2 → 1 in window → pass.
    var i: u8 = 0;
    while (i < 8) : (i += 1) {
        var ctx: Context = .{
            .subject = "x",
            .payload = "p",
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
        };
        try run(rules, &ctx);
    }
    // 4 passes total: calls 1, 2, 6, 8.
    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
}

test "throttle passes at most MAX per window-ms" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Up to 3 passes per 1s window.
    const rules = try loadRules(arena,
        \\(on "x"
        \\  (-> payload
        \\      (throttle :ms 1000 3)
        \\      (publish! "out")))
    );
    defer deinitRules(rules, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    const Frame = struct { now_ms: i64 };
    // 5 calls inside the same 1s → first 3 pass, 4th and 5th drop.
    const feed = [_]Frame{
        .{ .now_ms = 100 },
        .{ .now_ms = 200 },
        .{ .now_ms = 300 },
        .{ .now_ms = 400 },
        .{ .now_ms = 500 },
    };
    for (feed) |f| {
        var ctx: Context = .{
            .subject = "x",
            .payload = "p",
            .publisher = tp.publisher(),
            .arena = arena,
            .gpa = testing.allocator,
            .now_ms = f.now_ms,
        };
        try run(rules, &ctx);
    }
    try testing.expectEqual(@as(usize, 3), tp.buf.items.len);

    // Skip ahead past the window — bucket clears, next call passes.
    var ctx: Context = .{
        .subject = "x",
        .payload = "p",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .now_ms = 1600,
    };
    try run(rules, &ctx);
    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
}

test "tickClocks evicts old samples from time-ring moving-avg" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const loaded = try loadRules(arena,
        \\(on "x"
        \\  (-> payload-float
        \\      (moving-avg :ms 1000)
        \\      (publish! "avg")))
    );
    defer deinitRules(loaded, testing.allocator);

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "x",
        .payload = "10",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .now_ms = 0,
    };
    try run(loaded, &ctx);
    // Walker at t=5000 should evict the t=0 sample. We can't observe the
    // ring directly through the public API, so push a fresh sample after
    // the walker and verify the new average doesn't include the old one.
    try eval.tickClocks(arena, loaded, 5000, tp.publisher());

    ctx.payload = "30";
    ctx.now_ms = 5100;
    try run(loaded, &ctx);
    try testing.expectEqual(@as(usize, 2), tp.buf.items.len);
    try testing.expectEqualStrings("avg", tp.buf.items[1].subject);
    try testing.expectEqualStrings("30", tp.buf.items[1].payload);
}

// --- Re-entry: patchbay-emitted publishes match downstream rules ---------

const RuleSet = eval.RuleSet;
const buildRuleSet = eval.buildRuleSet;

// Test harness for re-entry. Holds the RuleSet so the hook can dispatch.
const ReentryHarness = struct {
    rs: *const RuleSet,

    fn hook(
        ctx_opaque: ?*anyopaque,
        parent: *Context,
        subject: []const u8,
        payload: []const u8,
    ) anyerror!void {
        const self: *ReentryHarness = @ptrCast(@alignCast(ctx_opaque.?));
        var child: Context = .{
            .subject = subject,
            .payload = payload,
            .publisher = parent.publisher,
            .arena = parent.arena,
            .gpa = parent.gpa,
            .depth = parent.depth + 1,
            .max_depth = parent.max_depth,
            .reentry_ctx = parent.reentry_ctx,
            .reentry_fn = parent.reentry_fn,
        };
        try self.rs.run(&child);
    }
};

test "staged rules: demuxed subject re-enters and matches downstream" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // First rule demuxes a JSON frame into devices.<id>.temp.
    // Second rule consumes that demuxed subject and republishes a
    // "stable" mirror, only reachable via re-entry.
    const rules = try loadRules(arena,
        \\(on "devices.*" :reentrant true
        \\  (json-demux! "temp" payload))
        \\(on "devices.*.temp"
        \\  (-> payload-float
        \\      (round 0)
        \\      (publish! (subject-append "stable"))))
    );
    defer deinitRules(rules, testing.allocator);

    const rs = try buildRuleSet(arena, rules);
    var harness: ReentryHarness = .{ .rs = &rs };

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "devices.kitchen",
        .payload = "{\"temp\":12.7}",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .reentry_ctx = &harness,
        .reentry_fn = ReentryHarness.hook,
    };
    try rs.run(&ctx);

    // Expect: devices.kitchen.temp 12.7 (from demux),
    //         devices.kitchen.temp.stable 13 (from staged rule via re-entry).
    try testing.expectEqual(@as(usize, 2), tp.buf.items.len);
    try testing.expectEqualStrings("devices.kitchen.temp", tp.buf.items[0].subject);
    try testing.expectEqualStrings("12.7", tp.buf.items[0].payload);
    try testing.expectEqualStrings("devices.kitchen.temp.stable", tp.buf.items[1].subject);
    try testing.expectEqualStrings("13", tp.buf.items[1].payload);
}

test "re-entry depth cap prevents runaway loops" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // A rule whose own emission matches its own filter. Without the cap
    // this would loop forever; with the cap we get exactly max_depth + 1
    // emissions (the original + one per allowed re-entry).
    const rules = try loadRules(arena,
        \\(on "loop.>" :reentrant true
        \\  (publish! (subject-append "x") payload))
    );
    defer deinitRules(rules, testing.allocator);

    const rs = try buildRuleSet(arena, rules);
    var harness: ReentryHarness = .{ .rs = &rs };

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "loop.start",
        .payload = "p",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .max_depth = 3,
        .reentry_ctx = &harness,
        .reentry_fn = ReentryHarness.hook,
    };
    try rs.run(&ctx);

    // depth 0 emits, depth 1 emits, depth 2 emits, depth 3 emits, depth 4 blocked.
    // So 4 total emissions (one per allowed level, 0..max_depth inclusive).
    try testing.expectEqual(@as(usize, 4), tp.buf.items.len);
}

test "re-entry is opt-in: non-reentrant rule does not feed downstream rules" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Same staged shape as the demux test, but the upstream rule is NOT
    // marked :reentrant — its emission must not match the downstream rule.
    const rules = try loadRules(arena,
        \\(on "devices.*"
        \\  (json-demux! "temp" payload))
        \\(on "devices.*.temp"
        \\  (-> payload-float
        \\      (round 0)
        \\      (publish! (subject-append "stable"))))
    );
    defer deinitRules(rules, testing.allocator);

    const rs = try buildRuleSet(arena, rules);
    var harness: ReentryHarness = .{ .rs = &rs };

    var tp: TestPublisher = .{ .alloc = arena };
    var ctx: Context = .{
        .subject = "devices.kitchen",
        .payload = "{\"temp\":12.7}",
        .publisher = tp.publisher(),
        .arena = arena,
        .gpa = testing.allocator,
        .reentry_ctx = &harness,
        .reentry_fn = ReentryHarness.hook,
    };
    try rs.run(&ctx);

    // Only the demux emission; the staged rule never sees it.
    try testing.expectEqual(@as(usize, 1), tp.buf.items.len);
    try testing.expectEqualStrings("devices.kitchen.temp", tp.buf.items[0].subject);
}
