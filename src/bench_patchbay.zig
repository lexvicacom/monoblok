//! In-process microbench for patchbay rule evaluation.
//!
//! Measures pure eval cost (no router, no TCP, no parser, no syscalls) so
//! a change to the evaluator or builtins shows up cleanly. The publisher
//! is a no-op that just bumps a counter, so each iteration is bounded by
//! `RuleSet.run` + the body's ops.
//!
//! Run with: `zig build bench-patchbay -- [MODE] [N] [PUBS]`
//!   MODE  one of:
//!         pass   - body is just (publish! subject payload). Floor: dispatch + emit.
//!         gate   - (-> payload-float (squelch) (publish! ...)). Per-rule state hashmap.
//!         window - (-> payload-float (moving-avg 10) (publish! ...)). Ring updates.
//!         json   - (json-demux! "a" "b" "c" payload) over a small object.
//!         mixed  - loads ./patchbay.edn from cwd and runs that.
//!   N     rule count (default 1; ignored in `mixed` since the file fixes it)
//!   PUBS  publishes per run (default 1_000_000)
//!
//! Subject is `bench.k<i % 1024>` so literal-index lookups are cold-ish
//! and wildcard rules ("sensors.*", ">") still match. Payload is a short
//! numeric string for the float-coerce modes, a tiny JSON object for json.

const std = @import("std");
const Io = std.Io;
const patchbay = @import("patchbay");

const eval = patchbay.eval;
const Rule = eval.Rule;
const RuleSet = eval.RuleSet;
const Context = eval.Context;
const Publisher = eval.Publisher;

const Mode = enum { pass, gate, window, json, mixed };

/// Monotonic ns. `std.time.nanoTimestamp` was removed in Zig 0.16.
fn monoNs() i128 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
    return @as(i128, ts.sec) * std.time.ns_per_s + @as(i128, ts.nsec);
}

const NoopPublisher = struct {
    count: u64 = 0,

    fn publish(ctx: *anyopaque, _: []const u8, _: []const u8) anyerror!void {
        const self: *NoopPublisher = @ptrCast(@alignCast(ctx));
        self.count += 1;
    }

    fn publisher(self: *NoopPublisher) Publisher {
        return .{ .ctx = self, .publish_fn = publish };
    }
};

fn parseMode(s: []const u8) ?Mode {
    if (std.mem.eql(u8, s, "pass")) return .pass;
    if (std.mem.eql(u8, s, "gate")) return .gate;
    if (std.mem.eql(u8, s, "window")) return .window;
    if (std.mem.eql(u8, s, "json")) return .json;
    if (std.mem.eql(u8, s, "mixed")) return .mixed;
    return null;
}

/// Build a synthetic patchbay source for `mode` with `n` rules. All rules
/// share the filter "sensors.*" so the timed loop's "sensors.k<i>" subject
/// matches every one (worst-case: every rule fires per PUB).
fn buildSource(arena: std.mem.Allocator, mode: Mode, n: usize) ![]const u8 {
    var buf: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        switch (mode) {
            .pass => try buf.print(
                arena,
                "(on \"sensors.*\" (publish! (subject-append \"r{d}\") payload))\n",
                .{i},
            ),
            .gate => try buf.print(
                arena,
                "(on \"sensors.*\" (-> payload-float (squelch) (publish! (subject-append \"r{d}\"))))\n",
                .{i},
            ),
            .window => try buf.print(
                arena,
                "(on \"sensors.*\" (-> payload-float (moving-avg 10) (publish! (subject-append \"r{d}\"))))\n",
                .{i},
            ),
            .json => try buf.print(
                arena,
                "(on \"sensors.*\" (json-demux! \"a\" \"b\" \"c\" payload))\n",
                .{},
            ),
            .mixed => unreachable,
        }
    }
    return buf.toOwnedSlice(arena);
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const fsio = init.io;

    var it = try init.minimal.args.iterateAllocator(gpa);
    defer it.deinit();
    _ = it.skip(); // exe name

    const mode_arg: []const u8 = it.next() orelse "pass";
    const mode = parseMode(mode_arg) orelse {
        std.debug.print("bad mode '{s}' (want pass|gate|window|json|mixed)\n", .{mode_arg});
        return error.BadArgs;
    };
    const n: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 1;
    const pubs: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 1_000_000;

    if (n == 0 and mode != .mixed) {
        std.debug.print("N must be > 0\n", .{});
        return error.BadArgs;
    }

    // Arena for the source string + parsed AST + RuleSet index. Lives the
    // length of the run; everything else (per-rule state, the per-message
    // arena) uses gpa.
    var src_arena_state: std.heap.ArenaAllocator = .init(gpa);
    defer src_arena_state.deinit();
    const src_arena = src_arena_state.allocator();

    const source: []const u8 = switch (mode) {
        .mixed => try readFile(fsio, src_arena, "patchbay.edn"),
        else => try buildSource(src_arena, mode, n),
    };

    const rules = try eval.loadRules(src_arena, source);
    defer eval.deinitRules(rules, gpa);

    const ruleset = try eval.buildRuleSet(src_arena, rules);

    var noop: NoopPublisher = .{};

    // Per-message arena reused across iterations; reset each PUB like the
    // server does. Allocations inside eval (subject-append, str-concat,
    // json-demux output) live here.
    var msg_arena_state: std.heap.ArenaAllocator = .init(gpa);
    defer msg_arena_state.deinit();

    // Subject + payload buffers reused across iterations.
    var subj_buf: [64]u8 = undefined;
    const float_payloads = [_][]const u8{
        "1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7",
        "2.0", "2.1", "2.2", "2.3", "2.4", "2.5", "2.6", "2.7",
    };

    const pickPayload = struct {
        fn f(m: Mode, i: usize, floats: []const []const u8) []const u8 {
            return switch (m) {
                .json => "{\"a\":1,\"b\":2,\"c\":3}",
                else => floats[i % floats.len],
            };
        }
    }.f;

    // Warm up: stateful ops (squelch hashmap, moving-avg ring) measure
    // different work cold vs warm. 10k iterations is plenty to populate
    // every per-rule slot for the "sensors.k0..k1023" subject space.
    var warm_i: usize = 0;
    while (warm_i < 10_000) : (warm_i += 1) {
        _ = msg_arena_state.reset(.retain_capacity);
        const subj = try std.fmt.bufPrint(&subj_buf, "sensors.k{d}", .{warm_i % 1024});
        var ctx: Context = .{
            .subject = subj,
            .payload = pickPayload(mode, warm_i, &float_payloads),
            .publisher = noop.publisher(),
            .arena = msg_arena_state.allocator(),
            .gpa = gpa,
        };
        try ruleset.run(&ctx);
    }
    const warm_emits = noop.count;
    noop.count = 0;

    // Timed loop.
    const start = monoNs();
    var i: usize = 0;
    while (i < pubs) : (i += 1) {
        _ = msg_arena_state.reset(.retain_capacity);
        const subj = try std.fmt.bufPrint(&subj_buf, "sensors.k{d}", .{i % 1024});
        var ctx: Context = .{
            .subject = subj,
            .payload = pickPayload(mode, i, &float_payloads),
            .publisher = noop.publisher(),
            .arena = msg_arena_state.allocator(),
            .gpa = gpa,
        };
        try ruleset.run(&ctx);
    }
    const elapsed_ns = monoNs() - start;

    const elapsed_s: f64 = @as(f64, @floatFromInt(elapsed_ns)) / 1e9;
    const rate: f64 = @as(f64, @floatFromInt(pubs)) / elapsed_s;
    const ns_per_pub: f64 = @as(f64, @floatFromInt(elapsed_ns)) / @as(f64, @floatFromInt(pubs));
    const emits_per_pub: f64 = @as(f64, @floatFromInt(noop.count)) / @as(f64, @floatFromInt(pubs));

    std.debug.print(
        "mode={s} rules={d} | {d} pubs in {d:.3}s = {d:.0} pubs/s ({d:.1} ns/pub, {d:.2} emits/pub, warm_emits={d})\n",
        .{ @tagName(mode), rules.len, pubs, elapsed_s, rate, ns_per_pub, emits_per_pub, warm_emits },
    );
}

fn readFile(io: Io, arena: std.mem.Allocator, path: []const u8) ![]u8 {
    const cwd = Io.Dir.cwd();
    var file = try cwd.openFile(io, path, .{});
    defer file.close(io);
    const len = try file.length(io);
    const buf = try arena.alloc(u8, @intCast(len));
    var file_reader = file.reader(io, buf);
    try file_reader.interface.readSliceAll(buf);
    return buf;
}
