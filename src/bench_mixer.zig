//! In-process microbench for the mixer's hot paths.
//!
//! Measures pure parse + dispatch + format cost (no syscalls, no real
//! socketpair, no worker process). The real loop is created but never
//! run; `write_in_flight` is forced true so `maybeKickWrite` short-circuits
//! before touching any fd.
//!
//! Two paths exercised independently:
//!   forward  - ClientConn.processRx parsing N synthetic PUBs and forwarding
//!              raw bytes into a fake Worker's outbound buffer.
//!   return   - Worker.processUpstream parsing N synthetic MSGs from the
//!              worker fd and per-subscriber writeMsg formatting into client
//!              outbound buffers. Subscribers-per-filter is a knob so the
//!              writeMsg-scales-with-subscribers hypothesis is testable.
//!
//! Run with: `zig build bench-mixer -- [PATH] [N] [PUBS]`
//!   PATH    one of `forward` or `return`. Default `forward`.
//!   N       forward: ignored (always 1024 distinct subjects).
//!           return:  subscribers per filter (default 1).
//!   PUBS    publishes / messages to drive (default 1_000_000).

const std = @import("std");
const xev = @import("xev");

const mixer_mod = @import("mixer.zig");
const mixer_config = @import("mixer_config.zig");
const proto = @import("proto.zig");

const Mixer = mixer_mod.Mixer;
const Worker = mixer_mod.Worker;
const ClientConn = mixer_mod.ClientConn;
const FilterEntry = mixer_mod.FilterEntry;
const Subscriber = mixer_mod.Subscriber;

const Path = enum { forward, @"return" };

fn monoNs() i128 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
    return @as(i128, ts.sec) * std.time.ns_per_s + @as(i128, ts.nsec);
}

fn parsePath(s: []const u8) ?Path {
    if (std.mem.eql(u8, s, "forward")) return .forward;
    if (std.mem.eql(u8, s, "return")) return .@"return";
    return null;
}

/// Build N PUB frames into one buffer, all sharded to "ns0.*". Subjects
/// rotate over 1024 distinct values so the workerFor lookup actually does
/// the first-token-eql work each call (otherwise the same string compare
/// would dominate the L1 cache).
fn buildPubBatch(gpa: std.mem.Allocator, n: usize) ![]u8 {
    var buf: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    const payload = "hello";
    while (i < n) : (i += 1) {
        try buf.print(gpa, "PUB ns0.k{d} {d}\r\n", .{ i % 1024, payload.len });
        try buf.appendSlice(gpa, payload);
        try buf.appendSlice(gpa, "\r\n");
    }
    return buf.toOwnedSlice(gpa);
}

/// Build N MSG frames as if they came from the worker. Single internal_sid
/// "1" so the dispatchMsg lookup hits the same FilterEntry every time
/// (this is the realistic case: one filter, many deliveries).
fn buildMsgBatch(gpa: std.mem.Allocator, n: usize, internal_sid: []const u8) ![]u8 {
    var buf: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    const payload = "hello";
    while (i < n) : (i += 1) {
        try buf.print(gpa, "MSG ns0.k{d} {s} {d}\r\n", .{ i % 1024, internal_sid, payload.len });
        try buf.appendSlice(gpa, payload);
        try buf.appendSlice(gpa, "\r\n");
    }
    return buf.toOwnedSlice(gpa);
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;

    var it = try init.minimal.args.iterateAllocator(gpa);
    defer it.deinit();
    _ = it.skip();

    const path_arg: []const u8 = it.next() orelse "forward";
    const path = parsePath(path_arg) orelse {
        std.debug.print("bad path '{s}' (want forward|return)\n", .{path_arg});
        return error.BadArgs;
    };
    const n: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 1;
    const pubs: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 1_000_000;

    if (n == 0) {
        std.debug.print("N must be > 0\n", .{});
        return error.BadArgs;
    }

    var loop = try xev.Loop.init(.{});
    defer loop.deinit();

    // Minimal mixer config: one worker shard "ns0", one catch-all.
    const workers_cfg = [_]mixer_config.Worker{
        .{ .shard = "ns0", .patchbay = "/dev/null" },
        .{ .shard = "*", .patchbay = "/dev/null" },
    };
    const cfg: mixer_config.Config = .{
        .listen = "tcp://127.0.0.1:0",
        .workers = &workers_cfg,
        .catch_all = 1,
    };

    var mixer: Mixer = .{ .gpa = gpa, .loop = &loop, .cfg = cfg };
    mixer.workers = try gpa.alloc(Worker, cfg.workers.len);
    defer gpa.free(mixer.workers);
    for (cfg.workers, 0..) |w, idx| {
        mixer.workers[idx] = .{ .mixer = &mixer, .cfg_idx = idx, .shard = w.shard };
        // Force kicks to short-circuit: pretend a write is always in flight.
        mixer.workers[idx].write_in_flight = true;
    }
    defer for (mixer.workers) |*w| {
        w.rx.deinit(gpa);
        w.out.deinit(gpa);
        w.in_flight.deinit(gpa);
        var it2 = w.filter_by_str.iterator();
        while (it2.next()) |e| {
            gpa.free(e.value_ptr.*.filter);
            gpa.free(e.value_ptr.*.internal_sid);
            for (e.value_ptr.*.subscribers.items) |s| gpa.free(s.client_sid);
            e.value_ptr.*.subscribers.deinit(gpa);
            gpa.destroy(e.value_ptr.*);
        }
        w.filter_by_str.deinit(gpa);
        w.filter_by_sid.deinit(gpa);
    };

    switch (path) {
        .forward => try benchForward(gpa, &mixer, &loop, pubs),
        .@"return" => try benchReturn(gpa, &mixer, &loop, n, pubs),
    }
}

fn benchForward(gpa: std.mem.Allocator, mixer: *Mixer, loop: *xev.Loop, pubs: usize) !void {
    // Build a synthetic ClientConn manually. ClientConn.init does I/O setup
    // we don't need; we only want the rx buffer + processRx + out buffers.
    var client: ClientConn = .{
        .mixer = mixer,
        .id = 1,
        .tcp = undefined,
    };
    client.write_in_flight = true; // suppress write kicks
    defer {
        client.rx.deinit(gpa);
        client.out.deinit(gpa);
        client.in_flight.deinit(gpa);
        var it = client.subs.iterator();
        while (it.next()) |e| gpa.free(e.key_ptr.*);
        client.subs.deinit(gpa);
    }

    const batch = try buildPubBatch(gpa, pubs);
    defer gpa.free(batch);

    // Warm.
    try client.rx.appendSlice(gpa, batch[0..@min(64 * 1024, batch.len)]);
    try processRxFor(&client, loop);
    client.rx.clearRetainingCapacity();
    for (mixer.workers) |*w| w.out.clearRetainingCapacity();

    // Time.
    try client.rx.appendSlice(gpa, batch);
    const start = monoNs();
    try processRxFor(&client, loop);
    const elapsed_ns = monoNs() - start;

    var bytes_out: usize = 0;
    for (mixer.workers) |*w| bytes_out += w.out.items.len;

    report("forward", pubs, elapsed_ns, batch.len, bytes_out, 0);
}

fn benchReturn(gpa: std.mem.Allocator, mixer: *Mixer, loop: *xev.Loop, subs_per_filter: usize, msgs: usize) !void {
    // One Worker (the ns0 shard). Pre-register one filter "ns0.*" with
    // `subs_per_filter` synthetic ClientConns subscribed to it.
    const w = &mixer.workers[0];

    // Allocate the FilterEntry directly so we don't need addSubscriber's
    // upstream-write path.
    const entry = try gpa.create(FilterEntry);
    entry.* = .{
        .filter = try gpa.dupe(u8, "ns0.*"),
        .internal_sid = try gpa.dupe(u8, "1"),
    };
    try w.filter_by_str.put(gpa, entry.filter, entry);
    try w.filter_by_sid.put(gpa, entry.internal_sid, entry);

    const clients = try gpa.alloc(ClientConn, subs_per_filter);
    defer gpa.free(clients);
    for (clients, 0..) |*c, i| {
        c.* = .{ .mixer = mixer, .id = @intCast(i + 1), .tcp = undefined };
        c.write_in_flight = true;
        var sid_buf: [32]u8 = undefined;
        const sid = try std.fmt.bufPrint(&sid_buf, "{d}", .{i + 1});
        const sid_owned = try gpa.dupe(u8, sid);
        try entry.subscribers.append(gpa, .{ .client = c, .client_sid = sid_owned });
    }
    defer for (clients) |*c| {
        c.rx.deinit(gpa);
        c.out.deinit(gpa);
        c.in_flight.deinit(gpa);
        var it = c.subs.iterator();
        while (it.next()) |e| gpa.free(e.key_ptr.*);
        c.subs.deinit(gpa);
    };

    const batch = try buildMsgBatch(gpa, msgs, "1");
    defer gpa.free(batch);

    // Warm.
    try w.rx.appendSlice(gpa, batch[0..@min(64 * 1024, batch.len)]);
    try processUpstreamFor(w, loop);
    w.rx.clearRetainingCapacity();
    for (clients) |*c| c.out.clearRetainingCapacity();

    // Time.
    try w.rx.appendSlice(gpa, batch);
    const start = monoNs();
    try processUpstreamFor(w, loop);
    const elapsed_ns = monoNs() - start;

    var bytes_out: usize = 0;
    for (clients) |*c| bytes_out += c.out.items.len;

    std.debug.print("subs/filter={d} ", .{subs_per_filter});
    report("return", msgs, elapsed_ns, batch.len, bytes_out, subs_per_filter);
}

fn report(label: []const u8, ops: usize, elapsed_ns: i128, bytes_in: usize, bytes_out: usize, subs: usize) void {
    const elapsed_s: f64 = @as(f64, @floatFromInt(elapsed_ns)) / 1e9;
    const rate: f64 = @as(f64, @floatFromInt(ops)) / elapsed_s;
    const ns_per_op: f64 = @as(f64, @floatFromInt(elapsed_ns)) / @as(f64, @floatFromInt(ops));
    const expected_out = if (subs == 0) bytes_in else bytes_in * subs;
    std.debug.print(
        "path={s} ops={d} | {d:.3}s = {d:.0} ops/s ({d:.1} ns/op, in={d}B out={d}B exp={d}B)\n",
        .{ label, ops, elapsed_s, rate, ns_per_op, bytes_in, bytes_out, expected_out },
    );
}

// ClientConn.processRx and Worker.processUpstream are private. We can't
// call them from outside the file, so we re-implement the same loop here
// for the bench. Both upstream functions are short and don't change often;
// if they drift this bench will under-measure, which is preferable to
// exposing them publicly just for tests.
fn processRxFor(client: *ClientConn, loop: *xev.Loop) !void {
    _ = loop;
    const gpa = client.mixer.gpa;
    var cursor: usize = 0;
    while (cursor < client.rx.items.len) {
        const slice = client.rx.items[cursor..];
        const result = proto.parseClientOp(slice) catch |err| switch (err) {
            error.NeedMoreData => break,
            else => break,
        };
        switch (result.op) {
            .pub_msg => |p| {
                const w = client.mixer.workerFor(p.subject);
                try w.out.appendSlice(gpa, slice[0..result.consumed]);
            },
            else => {},
        }
        cursor += result.consumed;
    }
    if (cursor > 0) {
        const rest = client.rx.items.len - cursor;
        std.mem.copyForwards(u8, client.rx.items[0..rest], client.rx.items[cursor..]);
        client.rx.items.len = rest;
    }
}

fn processUpstreamFor(w: *Worker, loop: *xev.Loop) !void {
    _ = loop;
    const gpa = w.mixer.gpa;
    var cursor: usize = 0;
    while (cursor < w.rx.items.len) {
        const slice = w.rx.items[cursor..];
        const result = proto.parseServerOp(slice) catch |err| switch (err) {
            error.NeedMoreData => break,
            else => return err,
        };
        switch (result.op) {
            .msg => |m| {
                const entry = w.filter_by_sid.get(m.sid) orelse {
                    cursor += result.consumed;
                    continue;
                };
                for (entry.subscribers.items) |sub| {
                    if (sub.client.closing) continue;
                    if (m.headers) |h| {
                        try proto.writeHmsg(gpa, &sub.client.out, m.subject, sub.client_sid, m.reply, h, m.payload);
                    } else {
                        try proto.writeMsg(gpa, &sub.client.out, m.subject, sub.client_sid, m.reply, m.payload);
                    }
                }
            },
            else => {},
        }
        cursor += result.consumed;
    }
    if (cursor > 0) {
        const rest = w.rx.items.len - cursor;
        std.mem.copyForwards(u8, w.rx.items[0..rest], w.rx.items[cursor..]);
        w.rx.items.len = rest;
    }
}
