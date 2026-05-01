// Parser for the (mixer ...) form. Worker entries are nested lists
// (:k v :k v ...) since sexpr only has parens. Exactly one :shard "*"
// (catch-all) is required.

const std = @import("std");
const Allocator = std.mem.Allocator;

const patchbay = @import("patchbay");
const sexpr = patchbay.sexpr;

pub const ConfigError = error{
    InvalidMixerForm,
    DuplicateMixer,
    MissingMixer,
    MissingWorkers,
    MissingListen,
    MissingShard,
    DuplicateShard,
    MissingCatchAll,
    UnknownKeyword,
} || sexpr.ParseError;

pub const Worker = struct {
    shard: []const u8,
    patchbay: []const u8,
    /// Per-worker stderr file. If set the mixer dup2s this onto fd 2 before
    /// exec, so each worker's logs land in their own file.
    log: ?[]const u8 = null,
    /// Pass --trace on the worker's CLI.
    trace: bool = false,
};

pub const Config = struct {
    listen: []const u8,
    workers: []const Worker,
    catch_all: usize,
};

pub fn loadConfig(arena: Allocator, source: []const u8) ConfigError!Config {
    const forms = try sexpr.parseAll(arena, source);
    var found: ?Config = null;
    for (forms) |f| {
        if (f != .list or f.list.len == 0) continue;
        const head = f.list[0];
        if (head != .symbol or !std.mem.eql(u8, head.symbol, "mixer")) continue;
        if (found != null) return error.DuplicateMixer;
        found = try parseMixerForm(arena, f.list[1..]);
    }
    return found orelse error.MissingMixer;
}

const MixerKey = enum { listen, workers };

const mixer_kw = std.StaticStringMap(MixerKey).initComptime(.{
    .{ "listen", .listen },
    .{ "workers", .workers },
});

const WorkerKey = enum { shard, patchbay, log, trace };

const worker_kw = std.StaticStringMap(WorkerKey).initComptime(.{
    .{ "shard", .shard },
    .{ "patchbay", .patchbay },
    .{ "log", .log },
    .{ "trace", .trace },
});

fn parseMixerForm(arena: Allocator, kvs: []const sexpr.Value) ConfigError!Config {
    if (kvs.len % 2 != 0) return error.InvalidMixerForm;

    var listen: ?[]const u8 = null;
    var workers: ?[]const Worker = null;

    var i: usize = 0;
    while (i < kvs.len) : (i += 2) {
        if (kvs[i] != .keyword) return error.InvalidMixerForm;
        const k = kvs[i].keyword;
        const v = kvs[i + 1];
        const tag = mixer_kw.get(k) orelse return error.UnknownKeyword;
        switch (tag) {
            .listen => listen = try asString(v),
            .workers => workers = try parseWorkers(arena, v),
        }
    }

    const ws = workers orelse return error.MissingWorkers;
    if (ws.len == 0) return error.MissingWorkers;
    const ls = listen orelse return error.MissingListen;

    var catch_all: ?usize = null;
    for (ws, 0..) |w, idx| {
        if (std.mem.eql(u8, w.shard, "*")) {
            if (catch_all != null) return error.DuplicateShard;
            catch_all = idx;
            continue;
        }
        for (ws[0..idx]) |prev| {
            if (std.mem.eql(u8, prev.shard, w.shard)) return error.DuplicateShard;
        }
    }

    return .{
        .listen = ls,
        .workers = ws,
        .catch_all = catch_all orelse return error.MissingCatchAll,
    };
}

fn parseWorkers(arena: Allocator, v: sexpr.Value) ConfigError![]const Worker {
    if (v != .list) return error.InvalidMixerForm;
    const out = try arena.alloc(Worker, v.list.len);
    for (v.list, 0..) |entry, idx| {
        out[idx] = try parseWorker(entry);
    }
    return out;
}

fn parseWorker(entry: sexpr.Value) ConfigError!Worker {
    if (entry != .list) return error.InvalidMixerForm;
    const kvs = entry.list;
    if (kvs.len % 2 != 0) return error.InvalidMixerForm;

    var shard: ?[]const u8 = null;
    var pb: ?[]const u8 = null;
    var log: ?[]const u8 = null;
    var trace: bool = false;

    var i: usize = 0;
    while (i < kvs.len) : (i += 2) {
        if (kvs[i] != .keyword) return error.InvalidMixerForm;
        const k = kvs[i].keyword;
        const val = kvs[i + 1];
        const tag = worker_kw.get(k) orelse return error.UnknownKeyword;
        switch (tag) {
            .shard => shard = try asString(val),
            .patchbay => pb = try asString(val),
            .log => log = try asString(val),
            .trace => trace = switch (val) {
                .boolean => |b| b,
                else => return error.InvalidMixerForm,
            },
        }
    }

    const s = shard orelse return error.MissingShard;
    if (s.len == 0) return error.MissingShard;
    const p = pb orelse return error.InvalidMixerForm;
    return .{ .shard = s, .patchbay = p, .log = log, .trace = trace };
}

fn asString(v: sexpr.Value) ConfigError![]const u8 {
    return switch (v) {
        .string => |s| s,
        else => error.InvalidMixerForm,
    };
}

// --- Tests ---------------------------------------------------------------

const testing = std.testing;

test "load minimal mixer" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(mixer
        \\  :listen "tcp://0.0.0.0:4222"
        \\  :workers
        \\    ((:shard "T"      :patchbay "t.edn")
        \\     (:shard "*"      :patchbay "default.edn" :log "d.log" :trace true)))
    ;
    const cfg = try loadConfig(arena, src);
    try testing.expectEqualStrings("tcp://0.0.0.0:4222", cfg.listen);
    try testing.expectEqual(@as(usize, 2), cfg.workers.len);
    try testing.expectEqualStrings("T", cfg.workers[0].shard);
    try testing.expectEqualStrings("t.edn", cfg.workers[0].patchbay);
    try testing.expectEqual(false, cfg.workers[0].trace);
    try testing.expectEqualStrings("*", cfg.workers[1].shard);
    try testing.expectEqualStrings("d.log", cfg.workers[1].log.?);
    try testing.expectEqual(true, cfg.workers[1].trace);
    try testing.expectEqual(@as(usize, 1), cfg.catch_all);
}

test "missing catch-all rejected" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(mixer
        \\  :listen "tcp://0.0.0.0:4222"
        \\  :workers ((:shard "T" :patchbay "t.edn")))
    ;
    try testing.expectError(error.MissingCatchAll, loadConfig(arena, src));
}

test "duplicate shard rejected" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(mixer
        \\  :listen "tcp://0.0.0.0:4222"
        \\  :workers
        \\    ((:shard "T" :patchbay "a.edn")
        \\     (:shard "T" :patchbay "b.edn")
        \\     (:shard "*" :patchbay "d.edn")))
    ;
    try testing.expectError(error.DuplicateShard, loadConfig(arena, src));
}

test "missing mixer form" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();
    try testing.expectError(error.MissingMixer, loadConfig(arena, "(bridge :servers [\"x\"])"));
}
