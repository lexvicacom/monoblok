//! In-process microbench for the mixer's hot paths.
//!
//! Measures pure parse + dispatch + format cost (no syscalls, no real
//! socketpair, no worker process). The real loop is created but never
//! run; `write_in_flight` is forced true so `maybeKickWrite` short-circuits
//! before touching any fd.
//!
//! Three paths exercised independently:
//!   forward  - ClientConn.processRx parsing N synthetic PUBs and forwarding
//!              raw bytes into a fake Worker's outbound buffer.
//!   return   - Worker.processUpstream parsing N synthetic MSGs from the
//!              worker fd and per-subscriber writeMsg formatting into client
//!              outbound buffers. Subscribers-per-filter is a knob so the
//!              writeMsg-scales-with-subscribers hypothesis is testable.
//!   kick     - same dispatch path as `return` but with a real socketpair
//!              under the ClientConn and the libxev loop actually running.
//!              Reports ns/op for two policies on the same workload:
//!                early - maybeKickWrite after every MSG (production)
//!                late  - one explicit kick after the whole batch
//!              The delta isolates how much per-MSG kick-write churn costs
//!              vs the cost the formatter already pays. Single-subscriber
//!              only; multi-sub kick churn is just N copies of single-sub.
//!
//! Run with: `zig build bench-mixer -- [PATH] [N] [PUBS]`
//!   PATH    one of `forward`, `return`, `kick`. Default `forward`.
//!   N       forward: ignored (always 1024 distinct subjects).
//!           return:  subscribers per filter (default 1).
//!           kick:    ignored (always 1 subscriber).
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

const Path = enum { forward, @"return", kick };

extern "c" fn socketpair(domain: c_int, ty: c_int, protocol: c_int, sv: *[2]c_int) c_int;

fn monoNs() i128 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
    return @as(i128, ts.sec) * std.time.ns_per_s + @as(i128, ts.nsec);
}

fn parsePath(s: []const u8) ?Path {
    if (std.mem.eql(u8, s, "forward")) return .forward;
    if (std.mem.eql(u8, s, "return")) return .@"return";
    if (std.mem.eql(u8, s, "kick")) return .kick;
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
        if (!std.mem.eql(u8, w.shard, "*")) {
            try mixer.worker_by_shard.put(gpa, w.shard, &mixer.workers[idx]);
        }
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
        w.entries_by_sid.deinit(gpa);
    };
    defer mixer.worker_by_shard.deinit(gpa);

    switch (path) {
        .forward => try benchForward(gpa, &mixer, &loop, pubs),
        .@"return" => try benchReturn(gpa, &mixer, &loop, n, pubs),
        .kick => try benchKick(gpa, &mixer, &loop, pubs),
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

    report("forward", pubs, elapsed_ns, batch.len, bytes_out, batch.len);
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
        .internal_sid_num = 1,
    };
    try w.filter_by_str.put(gpa, entry.filter, entry);
    try registerBenchSid(gpa, w, entry);

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
    const expected_out = expectedMsgFanoutBytes(msgs, entry.subscribers.items);

    std.debug.print("subs/filter={d} ", .{subs_per_filter});
    report("return", msgs, elapsed_ns, batch.len, bytes_out, expected_out);
}

/// Real-loop drain harness. Owns a socketpair: `client_fd` is wrapped as the
/// ClientConn's xev.TCP (writes go out here); `drain_fd` is read in a libxev
/// completion that just discards bytes and counts them. The bench stops the
/// loop once `drained_target` bytes have been read.
const Drain = struct {
    fd: std.posix.fd_t,
    tcp: xev.TCP,
    completion: xev.Completion = undefined,
    buf: [64 * 1024]u8 = undefined,
    drained: usize = 0,
    target: usize = 0,
    loop: *xev.Loop,

    fn start(self: *Drain) void {
        self.tcp.read(self.loop, &self.completion, .{ .slice = &self.buf }, Drain, self, onRead);
    }

    fn onRead(
        self_opt: ?*Drain,
        loop: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        buf: xev.ReadBuffer,
        r: xev.ReadError!usize,
    ) xev.CallbackAction {
        const self = self_opt.?;
        const n = r catch {
            loop.stop();
            return .disarm;
        };
        if (n == 0) {
            loop.stop();
            return .disarm;
        }
        _ = buf;
        self.drained += n;
        if (self.drained >= self.target) {
            loop.stop();
            return .disarm;
        }
        return .rearm;
    }
};

fn benchKick(gpa: std.mem.Allocator, mixer: *Mixer, loop: *xev.Loop, msgs: usize) !void {
    // Pre-register one filter "ns0.*" with one subscriber (a real ClientConn
    // backed by a socketpair). Drive `msgs` MSGs through Worker.processUpstream
    // twice: once with kicks fired per-MSG (production), once with kicks
    // suppressed and a single explicit flush at the end. Both runs include
    // the time to fully drain bytes off the socket (loop.run until target).
    //
    // Each run gets its own fresh xev.Loop. Reusing one loop across runs is
    // fragile: the first run's completions can still be referenced after
    // loop.stop(), and macOS kqueue + .until_done don't always play well
    // with mid-flight teardown.
    _ = loop;
    const w = &mixer.workers[0];

    const entry = try gpa.create(FilterEntry);
    entry.* = .{
        .filter = try gpa.dupe(u8, "ns0.*"),
        .internal_sid = try gpa.dupe(u8, "1"),
        .internal_sid_num = 1,
    };
    try w.filter_by_str.put(gpa, entry.filter, entry);
    try registerBenchSid(gpa, w, entry);
    // FilterEntry storage (filter/internal_sid bufs and the entry itself) is
    // freed by main's worker defer block via the filter_by_str iterator.

    const batch = try buildMsgBatch(gpa, msgs, "1");
    defer gpa.free(batch);

    // Expected output bytes per run: one MSG per input MSG (single subscriber,
    // client_sid "1" same length as internal_sid so the formatted line is the
    // same byte count as the input MSG).
    var expected_out: usize = 0;
    {
        var i: usize = 0;
        while (i < msgs) : (i += 1) {
            // "MSG ns0.k<n> 1 5\r\nhello\r\n"
            expected_out += msgWireLen(i, "1", "hello".len);
        }
    }

    // --- early-kick run ---
    try kickRun(gpa, w, entry, msgs, batch, expected_out, .early);
    // --- late-kick run ---
    try kickRun(gpa, w, entry, msgs, batch, expected_out, .late);
}

const KickPolicy = enum { early, late };

fn kickRun(
    gpa: std.mem.Allocator,
    w: *Worker,
    entry: *FilterEntry,
    msgs: usize,
    batch: []const u8,
    expected_out: usize,
    policy: KickPolicy,
) !void {
    var loop = try xev.Loop.init(.{});
    defer loop.deinit();

    const SOCK_STREAM: c_int = std.posix.SOCK.STREAM;
    const AF_UNIX: c_int = std.posix.AF.UNIX;

    var sv: [2]c_int = .{ -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, &sv) != 0) return error.SocketpairFailed;
    for (sv) |fd| {
        const fl = std.c.fcntl(fd, std.posix.F.GETFL, @as(c_int, 0));
        if (fl >= 0) {
            const nonblock_bit: c_int = @bitCast(std.posix.O{ .NONBLOCK = true });
            _ = std.c.fcntl(fd, std.posix.F.SETFL, @as(c_int, fl | nonblock_bit));
        }
    }

    var client: ClientConn = .{
        .mixer = w.mixer,
        .id = 1,
        .tcp = xev.TCP.initFd(sv[0]),
    };
    // early: kicks fire per-MSG (write_in_flight=false enables maybeKickWrite).
    // late:  kicks suppressed via write_in_flight=true; flipped to false at end.
    client.write_in_flight = (policy == .late);
    defer {
        client.rx.deinit(gpa);
        client.out.deinit(gpa);
        client.in_flight.deinit(gpa);
        var it = client.subs.iterator();
        while (it.next()) |e| gpa.free(e.key_ptr.*);
        client.subs.deinit(gpa);
        _ = std.c.close(sv[0]);
    }

    entry.subscribers.clearRetainingCapacity();
    const sid_owned = try gpa.dupe(u8, "1");
    try entry.subscribers.append(gpa, .{ .client = &client, .client_sid = sid_owned });
    defer {
        for (entry.subscribers.items) |s| gpa.free(s.client_sid);
        entry.subscribers.clearRetainingCapacity();
    }

    var drain: Drain = .{
        .fd = sv[1],
        .tcp = xev.TCP.initFd(sv[1]),
        .target = expected_out,
        .loop = &loop,
    };
    defer _ = std.c.close(sv[1]);
    drain.start();

    w.rx.clearRetainingCapacity();
    try w.rx.appendSlice(gpa, batch);

    const start = monoNs();
    try processUpstreamForReal(w, &loop);
    if (policy == .late) {
        client.write_in_flight = false;
        kickClient(&client, &loop);
    }
    try loop.run(.until_done);
    const elapsed_ns = monoNs() - start;

    const label = if (policy == .early) "early" else "late ";
    std.debug.print("policy={s} msgs={d} drained={d}/{d} ", .{ label, msgs, drain.drained, drain.target });
    report("kick", msgs, elapsed_ns, batch.len, drain.drained, expected_out);
}

/// Mirror of ClientConn.maybeKickWrite (private). Same logic; we need a
/// public-ish entry point for the explicit late flush.
fn kickClient(c: *ClientConn, loop: *xev.Loop) void {
    if (c.closing or c.write_in_flight) return;
    if (c.out.items.len == 0) return;
    std.mem.swap(std.ArrayList(u8), &c.out, &c.in_flight);
    c.write_in_flight = true;
    c.tcp.write(loop, &c.write_completion, .{ .slice = c.in_flight.items }, ClientConn, c, ClientConn_onWrite);
}

/// We need ClientConn.onWrite to drive the write completion, but it's private.
/// Re-implement the same behavior inline. If the production onWrite changes
/// shape, this drifts; the kick bench will still measure something sensible
/// (drain target gates correctness), but compare numbers against the same
/// commit.
fn ClientConn_onWrite(
    self_opt: ?*ClientConn,
    loop: *xev.Loop,
    _: *xev.Completion,
    _: xev.TCP,
    buf: xev.WriteBuffer,
    r: xev.WriteError!usize,
) xev.CallbackAction {
    const self = self_opt.?;
    const written = r catch {
        self.write_in_flight = false;
        return .disarm;
    };
    const total = buf.slice.len;
    if (written < total) {
        self.tcp.write(loop, &self.write_completion, .{ .slice = buf.slice[written..] }, ClientConn, self, ClientConn_onWrite);
        return .disarm;
    }
    self.write_in_flight = false;
    self.in_flight.clearRetainingCapacity();
    kickClient(self, loop);
    return .disarm;
}

/// Like processUpstreamFor but uses the production maybeKickWrite path
/// (real writes hit the socket as we go). Sid-translation, formatter,
/// kick-per-MSG — all live.
fn processUpstreamForReal(w: *Worker, loop: *xev.Loop) !void {
    const gpa = w.mixer.gpa;
    var kicks: KickQueueBench = .{};

    var cursor: usize = 0;
    while (cursor < w.rx.items.len) {
        const slice = w.rx.items[cursor..];
        if (proto.parseServerMsgEnvelope(slice)) |m| {
            const entry = benchEntryForSid(w, m.sid) orelse {
                cursor += m.consumed;
                continue;
            };
            if (entry.subscribers.items.len == 1) {
                const sub = entry.subscribers.items[0];
                if (!sub.client.closing and std.mem.eql(u8, sub.client_sid, m.sid)) {
                    try sub.client.out.appendSlice(gpa, slice[0..m.consumed]);
                    kicks.push(sub.client);
                    cursor += m.consumed;
                    continue;
                }
            }
            for (entry.subscribers.items) |sub| {
                if (sub.client.closing) continue;
                try proto.writeServerMsgWithSid(gpa, &sub.client.out, m.kind, m.subject, sub.client_sid, m.suffix_after_sid);
                kicks.push(sub.client);
            }
            cursor += m.consumed;
            continue;
        } else |err| switch (err) {
            error.NeedMoreData => break,
            error.UnknownOp => {},
            else => return err,
        }

        const result = proto.parseServerOp(slice) catch |err| switch (err) {
            error.NeedMoreData => break,
            else => return err,
        };
        switch (result.op) {
            .msg => |m| {
                const entry = benchEntryForSid(w, m.sid) orelse {
                    cursor += result.consumed;
                    continue;
                };
                if (entry.subscribers.items.len == 1) {
                    const sub = entry.subscribers.items[0];
                    if (!sub.client.closing and std.mem.eql(u8, sub.client_sid, m.sid)) {
                        try sub.client.out.appendSlice(gpa, slice[0..result.consumed]);
                        kicks.push(sub.client);
                        cursor += result.consumed;
                        continue;
                    }
                }
                for (entry.subscribers.items) |sub| {
                    if (sub.client.closing) continue;
                    try proto.writeServerMsgWithSid(gpa, &sub.client.out, m.kind, m.subject, sub.client_sid, m.suffix_after_sid);
                    kicks.push(sub.client);
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
    kicks.flush(loop);
}

fn report(label: []const u8, ops: usize, elapsed_ns: i128, bytes_in: usize, bytes_out: usize, expected_out: usize) void {
    const elapsed_s: f64 = @as(f64, @floatFromInt(elapsed_ns)) / 1e9;
    const rate: f64 = @as(f64, @floatFromInt(ops)) / elapsed_s;
    const ns_per_op: f64 = @as(f64, @floatFromInt(elapsed_ns)) / @as(f64, @floatFromInt(ops));
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
        if (proto.parseServerMsgEnvelope(slice)) |m| {
            const entry = benchEntryForSid(w, m.sid) orelse {
                cursor += m.consumed;
                continue;
            };
            if (entry.subscribers.items.len == 1) {
                const sub = entry.subscribers.items[0];
                if (!sub.client.closing and std.mem.eql(u8, sub.client_sid, m.sid)) {
                    try sub.client.out.appendSlice(gpa, slice[0..m.consumed]);
                    cursor += m.consumed;
                    continue;
                }
            }
            for (entry.subscribers.items) |sub| {
                if (sub.client.closing) continue;
                try proto.writeServerMsgWithSid(gpa, &sub.client.out, m.kind, m.subject, sub.client_sid, m.suffix_after_sid);
            }
            cursor += m.consumed;
            continue;
        } else |err| switch (err) {
            error.NeedMoreData => break,
            error.UnknownOp => {},
            else => return err,
        }

        const result = proto.parseServerOp(slice) catch |err| switch (err) {
            error.NeedMoreData => break,
            else => return err,
        };
        switch (result.op) {
            .msg => |m| {
                const entry = benchEntryForSid(w, m.sid) orelse {
                    cursor += result.consumed;
                    continue;
                };
                if (entry.subscribers.items.len == 1) {
                    const sub = entry.subscribers.items[0];
                    if (!sub.client.closing and std.mem.eql(u8, sub.client_sid, m.sid)) {
                        try sub.client.out.appendSlice(gpa, slice[0..result.consumed]);
                        cursor += result.consumed;
                        continue;
                    }
                }
                for (entry.subscribers.items) |sub| {
                    if (sub.client.closing) continue;
                    try proto.writeServerMsgWithSid(gpa, &sub.client.out, m.kind, m.subject, sub.client_sid, m.suffix_after_sid);
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

const KickQueueBench = struct {
    head: ?*ClientConn = null,
    tail: ?*ClientConn = null,

    fn push(self: *KickQueueBench, c: *ClientConn) void {
        if (c.kick_queued) return;
        c.kick_queued = true;
        c.kick_next = null;
        if (self.tail) |tail| {
            tail.kick_next = c;
        } else {
            self.head = c;
        }
        self.tail = c;
    }

    fn flush(self: *KickQueueBench, loop: *xev.Loop) void {
        var cur = self.head;
        while (cur) |c| {
            const next = c.kick_next;
            c.kick_next = null;
            c.kick_queued = false;
            kickClient(c, loop);
            cur = next;
        }
        self.head = null;
        self.tail = null;
    }
};

fn registerBenchSid(gpa: std.mem.Allocator, w: *Worker, entry: *FilterEntry) !void {
    const idx: usize = @intCast(entry.internal_sid_num);
    if (w.entries_by_sid.items.len <= idx) {
        const old_len = w.entries_by_sid.items.len;
        try w.entries_by_sid.resize(gpa, idx + 1);
        @memset(w.entries_by_sid.items[old_len..], null);
    }
    w.entries_by_sid.items[idx] = entry;
}

fn benchEntryForSid(w: *Worker, sid: []const u8) ?*FilterEntry {
    var n: usize = 0;
    for (sid) |c| {
        if (c < '0' or c > '9') return null;
        n = std.math.mul(usize, n, 10) catch return null;
        n = std.math.add(usize, n, @as(usize, @intCast(c - '0'))) catch return null;
    }
    if (n == 0 or n >= w.entries_by_sid.items.len) return null;
    return w.entries_by_sid.items[n];
}

fn msgWireLen(i: usize, sid: []const u8, payload_len: usize) usize {
    return 4 +
        std.fmt.count("ns0.k{d}", .{i % 1024}) + 1 +
        sid.len + 1 +
        std.fmt.count("{d}", .{payload_len}) + 2 +
        payload_len + 2;
}

fn expectedMsgFanoutBytes(msgs: usize, subs: []const Subscriber) usize {
    var total: usize = 0;
    var i: usize = 0;
    while (i < msgs) : (i += 1) {
        for (subs) |sub| total += msgWireLen(i, sub.client_sid, "hello".len);
    }
    return total;
}
