//! libxev-based TCP server. Single-threaded event loop; per-connection state
//! machine: on each read completion, parse as many complete ops as possible
//! from the rx buffer, dispatch them, then re-arm read. Writes are serialised
//! per-connection via a single-in-flight guard (`write_in_flight`); see the
//! comment on the write-side fields for why we don't use xev.WriteQueue.
const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;
const xev = @import("xev");

const proto = @import("proto.zig");
const router_mod = @import("router.zig");
const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;
const subject_mod = patchbay.subject;
const snapshot_mod = @import("snapshot.zig");

const rx_initial = 32 * 1024;
const rx_max = proto.max_control_line + proto.max_payload + 64;

/// Warn (once per PUB) if the patchbay emits more than this many publishes
/// for a single inbound message. Catches rule-amplification blow-ups before
/// they become memory/rate problems.
const rule_publish_warn_threshold: u32 = 64;

/// Warn (once per PUB) if any conn's outbound buffer grew past this many
/// bytes during the dispatch. Catches slow-consumer / fan-out blow-ups.
const out_queued_warn_threshold: usize = 4 * 1024 * 1024;

/// `--stats` flushes a summary line every this many inbound PUBs.
const stats_interval: u64 = 10_000;

/// Reserved prefix for the stats stream. Client publishes to `$STATS.*`
/// are rejected (same rule as `$LVC.*`); the server emits to it on a
/// wall-clock tick. Subscribers read via `$STATS.>` for live deltas; the
/// stream is deliberately NOT cached by the LVC (router.publish skips
/// storeLast for this prefix) because a tick-driven snapshot is stale
/// the moment the next tick fires.
const stats_prefix = "$STATS.";

/// Wall-clock tick for `$STATS.*` publishes.
const stats_tick_ms: u64 = 60_000;

/// Shared-layout view of the bridge's counter struct. Server reads from a
/// *const BridgeStats without importing bridge.zig directly (so the -Dbridge=false
/// build still compiles server.zig unchanged). bridge.Stats starts with these
/// fields in this order.
pub const BridgeStats = extern struct {
    published: u64 = 0,
    dropped: u64 = 0,
};

pub const Server = struct {
    gpa: Allocator,
    loop: *xev.Loop,
    router: *router_mod.Router,
    rules: rules_mod.RuleSet,
    listener: xev.TCP,
    accept_completion: xev.Completion = undefined,
    next_conn_id: u64 = 1,
    /// Process-wide unique id, generated once at startup. Sent in every
    /// INFO line's `server_id` / `server_name` fields.
    server_id: []const u8,
    listen_host: []const u8,
    listen_port: u16,

    /// `--trace` mode: print each rule evaluation step (form, result,
    /// elapsed time) to stderr. Read once per inbound PUB and copied into
    /// `rules.Context.trace`; the rules module flips its eval path based
    /// on the flag.
    trace_enabled: bool = false,

    /// `--stats` mode: print running-max summaries every `stats_interval`
    /// PUBs. Counters are reset each time the line is printed.
    stats_enabled: bool = false,
    stats_pubs: u64 = 0,
    stats_max_rule_publishes: u32 = 0,
    stats_max_out_hwm: usize = 0,

    /// Cumulative total of inbound client PUBs since server start. Published
    /// on the `$STATS.*` tick.
    total_pubs: u64 = 0,

    /// Optional pointer to the bridge's running counters. `emitStats` reads
    /// these on the stats tick to publish `$STATS.bridge.*`. Avoids pulling
    /// bridge.zig into server.zig directly.
    bridge_stats: ?*const BridgeStats = null,

    /// Periodic `$STATS.*` publisher state. Timer fires every
    /// `stats_tick_ms` and re-arms itself from the callback.
    stats_timer: xev.Timer = undefined,
    stats_completion: xev.Completion = undefined,

    /// LVC snapshot config. If `snapshot_path` is null, no periodic dump
    /// runs. If set, `snapshot_every_ms > 0` enables the dump timer.
    /// `snapshot_io` is required when dumping is enabled (the worker
    /// thread does its file I/O through it).
    snapshot_path: ?[]const u8 = null,
    snapshot_every_ms: u64 = 0,
    snapshot_io: ?Io = null,
    snapshot_timer: xev.Timer = undefined,
    snapshot_completion: xev.Completion = undefined,
    /// Serialises dumps so a slow disk can't stack up multiple threads.
    /// Set from loop thread before spawn; cleared from worker thread on
    /// exit. Atomic so the worker's clear is observable.
    snapshot_in_flight: std.atomic.Value(bool) = .init(false),

    /// Shutdown hook: a signal handler (installed by main) calls
    /// `shutdown_async.notify()`, which fires `onShutdown` on the loop
    /// thread. That callback writes a final synchronous snapshot (if
    /// configured) and stops the loop. Optional — only initialised if
    /// `shutdown_enabled` is true.
    shutdown_enabled: bool = false,
    shutdown_async: xev.Async = undefined,
    shutdown_completion: xev.Completion = undefined,

    fn recordPub(self: *Server, rule_publishes: u32) void {
        if (rule_publishes > self.stats_max_rule_publishes) {
            self.stats_max_rule_publishes = rule_publishes;
        }
        self.stats_pubs += 1;
        if (self.stats_pubs >= stats_interval) {
            std.log.info(
                "stats: pubs={d} max_rule_publishes={d} max_out_hwm={d}B",
                .{ self.stats_pubs, self.stats_max_rule_publishes, self.stats_max_out_hwm },
            );
            self.stats_pubs = 0;
            self.stats_max_rule_publishes = 0;
            self.stats_max_out_hwm = 0;
        }
    }

    pub fn listen(self: *Server, address: std.Io.net.IpAddress) !void {
        self.listener = try xev.TCP.init(address);
        try self.listener.bind(address);
        try self.listener.listen(128);
        self.listen_port = address.getPort();
        self.listener.accept(self.loop, &self.accept_completion, Server, self, onAccept);

        self.stats_timer = try xev.Timer.init();
        self.stats_timer.run(self.loop, &self.stats_completion, stats_tick_ms, Server, self, onStatsTick);

        if (self.snapshot_path != null and self.snapshot_every_ms > 0) {
            self.snapshot_timer = try xev.Timer.init();
            self.snapshot_timer.run(
                self.loop,
                &self.snapshot_completion,
                self.snapshot_every_ms,
                Server,
                self,
                onSnapshotTick,
            );
        }

        if (self.shutdown_enabled) {
            self.shutdown_async = try xev.Async.init();
            self.shutdown_async.wait(
                self.loop,
                &self.shutdown_completion,
                Server,
                self,
                onShutdown,
            );
        }
    }

    pub fn deinit(self: *Server) void {
        self.stats_timer.deinit();
        if (self.snapshot_path != null and self.snapshot_every_ms > 0) {
            self.snapshot_timer.deinit();
        }
        if (self.shutdown_enabled) self.shutdown_async.deinit();
    }

    /// Exposed for the signal-handler trampoline in main.zig. Safe to call
    /// from a signal handler: `Async.notify` only does a small non-blocking
    /// write to an eventfd / mach port.
    pub fn requestShutdown(self: *Server) void {
        if (!self.shutdown_enabled) return;
        self.shutdown_async.notify() catch {};
    }

    fn onShutdown(
        self_opt: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.Async.WaitError!void,
    ) xev.CallbackAction {
        _ = r catch {};
        const self = self_opt.?;
        // Single-threaded loop invariant: while this callback is running,
        // no read/parse/publish can interleave. Any PUB bytes that arrive
        // on the wire during the snapshot sit in the kernel buffer and are
        // dropped when the process exits — same semantics as any ungraceful
        // NATS shutdown (PUBs are unacked).
        std.log.info("shutdown: writing final snapshot...", .{});
        self.snapshotSync() catch |err| {
            std.log.warn("shutdown: snapshot failed: {s}", .{@errorName(err)});
        };
        loop.stop();
        return .disarm;
    }

    /// Synchronous snapshot: collect entries and write inline on the loop
    /// thread. Used only at shutdown, when blocking the loop is fine (we
    /// are exiting). No-op if no snapshot path is configured.
    fn snapshotSync(self: *Server) !void {
        const path = self.snapshot_path orelse return;
        const io = self.snapshot_io orelse return;

        var arena_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer arena_state.deinit();
        const arena = arena_state.allocator();

        const snap = try snapshot_mod.collect(arena, self.router, self.rules.rules);
        try snapshot_mod.writeFileAtomic(self.gpa, io, path, snap);
        std.log.info(
            "shutdown: wrote {d} lvc / {d} rule-state entries",
            .{ snap.lvc.len, snap.rule_state.len },
        );
    }

    fn onStatsTick(
        self_opt: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.Timer.RunError!void,
    ) xev.CallbackAction {
        _ = r catch {};
        const self = self_opt.?;

        self.emitStats() catch |err| {
            std.log.warn("stats emit failed: {s}", .{@errorName(err)});
        };

        self.stats_timer.run(loop, &self.stats_completion, stats_tick_ms, Server, self, onStatsTick);
        return .disarm;
    }

    /// Publish the current cumulative totals to `$STATS.*`. Values are
    /// u64 formatted as decimal; the router fan-outs and LVC caches them
    /// like any other stream. `router.publish` copies the payload, so a
    /// stack-local buffer is fine.
    fn emitStats(self: *Server) !void {
        try self.publishStat("$STATS.global.pubs", self.total_pubs);

        // Reusing subj_buf across both bufPrints is safe because
        // publishStat -> router.publish copies the subject before returning.
        var subj_buf: [64]u8 = undefined;
        for (self.rules.rules, 0..) |*rule, i| {
            const emit_subj = try std.fmt.bufPrint(&subj_buf, "$STATS.rules.{d}.emitted", .{i});
            try self.publishStat(emit_subj, rule.publishes_emitted);

            const supp_subj = try std.fmt.bufPrint(&subj_buf, "$STATS.rules.{d}.suppressed", .{i});
            try self.publishStat(supp_subj, rule.publishes_suppressed);
        }

        if (self.bridge_stats) |s| {
            try self.publishStat("$STATS.bridge.published", s.published);
            try self.publishStat("$STATS.bridge.dropped", s.dropped);
        }
    }

    fn onSnapshotTick(
        self_opt: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.Timer.RunError!void,
    ) xev.CallbackAction {
        _ = r catch {};
        const self = self_opt.?;

        self.kickSnapshot() catch |err| {
            std.log.warn("snapshot: tick failed: {s}", .{@errorName(err)});
        };

        self.snapshot_timer.run(
            loop,
            &self.snapshot_completion,
            self.snapshot_every_ms,
            Server,
            self,
            onSnapshotTick,
        );
        return .disarm;
    }

    /// Take a point-in-time copy of the LVC on the loop thread, then hand
    /// it off to a worker thread for serialization + fsync + atomic
    /// rename. The copy keeps the loop-side pause bounded (memcpy-only,
    /// no I/O). If a prior dump is still writing we skip this tick.
    ///
    /// Invariant: `collect` runs on the loop thread, so no publish can
    /// interleave with it (single-threaded loop). The worker thread only
    /// touches the arena it was given — a private copy — so subsequent
    /// mutations of `router.last_value` and `rule.state` are safe.
    fn kickSnapshot(self: *Server) !void {
        const path = self.snapshot_path orelse return;
        const io = self.snapshot_io orelse return;

        if (self.snapshot_in_flight.swap(true, .acquire)) {
            std.log.warn("snapshot: previous dump still running, skipping tick", .{});
            return;
        }
        errdefer self.snapshot_in_flight.store(false, .release);

        // The worker owns this arena: all bytes it touches live here,
        // and it frees the whole arena when done. Using the gpa backing
        // means no cross-thread arena ownership issues.
        const arena_ptr = try self.gpa.create(std.heap.ArenaAllocator);
        errdefer self.gpa.destroy(arena_ptr);
        arena_ptr.* = .init(self.gpa);
        errdefer arena_ptr.deinit();
        const arena = arena_ptr.allocator();

        const snap = try snapshot_mod.collect(arena, self.router, self.rules.rules);
        const path_copy = try arena.dupe(u8, path);

        const Job = struct {
            server: *Server,
            arena_ptr: *std.heap.ArenaAllocator,
            io: Io,
            path: []const u8,
            snap: snapshot_mod.Snapshot,

            fn run(job: *@This()) void {
                snapshot_mod.writeFileAtomic(job.server.gpa, job.io, job.path, job.snap) catch |err| {
                    std.log.warn("snapshot: write failed: {s}", .{@errorName(err)});
                };
                std.log.info(
                    "snapshot: wrote {d} lvc / {d} rule-state entries",
                    .{ job.snap.lvc.len, job.snap.rule_state.len },
                );

                const ap = job.arena_ptr;
                const gpa = job.server.gpa;
                job.server.snapshot_in_flight.store(false, .release);
                ap.deinit();
                gpa.destroy(ap);
                gpa.destroy(job);
            }
        };

        const job = try self.gpa.create(Job);
        errdefer self.gpa.destroy(job);
        job.* = .{
            .server = self,
            .arena_ptr = arena_ptr,
            .io = io,
            .path = path_copy,
            .snap = snap,
        };

        const t = try std.Thread.spawn(.{}, Job.run, .{job});
        t.detach();
    }

    fn publishStat(self: *Server, subject: []const u8, value: u64) !void {
        var buf: [32]u8 = undefined;
        const payload = std.fmt.bufPrint(&buf, "{d}", .{value}) catch unreachable;
        try self.router.publish(subject, payload);
    }

    fn onAccept(
        self_opt: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.AcceptError!xev.TCP,
    ) xev.CallbackAction {
        const self = self_opt.?;
        defer self.listener.accept(loop, &self.accept_completion, Server, self, onAccept);

        const tcp = r catch |err| {
            std.log.warn("accept failed: {s}", .{@errorName(err)});
            return .disarm;
        };

        const conn_state = Conn.init(self, tcp) catch |err| {
            std.log.warn("conn init failed: {s}", .{@errorName(err)});
            return .disarm;
        };
        std.log.info("conn {d} accepted", .{conn_state.router_conn.id});
        conn_state.start(loop);
        return .disarm;
    }
};

const Conn = struct {
    server: *Server,
    tcp: xev.TCP,
    router_conn: *router_mod.Conn,

    // Read side
    rx: std.ArrayList(u8) = .empty,
    read_completion: xev.Completion = undefined,
    read_buf: [16 * 1024]u8 = undefined,

    // Write side. We serialise writes ourselves via `write_in_flight` and
    // use the non-queued `write` API (simpler, and we've seen queueWrite
    // reuse-the-same-WriteRequest races under heavy load).
    write_completion: xev.Completion = undefined,
    write_in_flight: bool = false,
    // In-flight bytes are owned by the Conn and freed on completion. We copy
    // `router_conn.out` into this on each drain to free the outbound buffer
    // back for more appends while the write is flying.
    in_flight_buf: std.ArrayList(u8) = .empty,

    close_completion: xev.Completion = undefined,
    closing: bool = false,

    // Per-message arena, reset between messages so rule allocations don't
    // accumulate across the connection's lifetime.
    msg_arena: std.heap.ArenaAllocator,

    fn init(server: *Server, tcp: xev.TCP) !*Conn {
        const gpa = server.gpa;

        const self = try gpa.create(Conn);
        errdefer gpa.destroy(self);

        const rconn = try gpa.create(router_mod.Conn);
        errdefer gpa.destroy(rconn);

        const id = server.next_conn_id;
        server.next_conn_id += 1;

        rconn.* = .{ .id = id, .gpa = gpa };

        self.* = .{
            .server = server,
            .tcp = tcp,
            .router_conn = rconn,
            .msg_arena = .init(gpa),
        };

        rconn.kick_ctx = self;
        rconn.kick_fn = onKick;

        // Seed INFO.
        try proto.writeInfo(
            gpa,
            &rconn.out,
            server.server_id,
            id,
            server.listen_host,
            server.listen_port,
        );

        return self;
    }

    fn deinit(self: *Conn) void {
        const gpa = self.server.gpa;
        self.rx.deinit(gpa);
        self.in_flight_buf.deinit(gpa);
        self.msg_arena.deinit();
        self.router_conn.release();
        gpa.destroy(self);
    }

    fn start(self: *Conn, loop: *xev.Loop) void {
        self.maybeKickWrite(loop);
        self.startRead(loop);
    }

    fn startRead(self: *Conn, loop: *xev.Loop) void {
        if (self.closing) return;
        self.tcp.read(
            loop,
            &self.read_completion,
            .{ .slice = &self.read_buf },
            Conn,
            self,
            onRead,
        );
    }

    fn onRead(
        self_opt: ?*Conn,
        loop: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        buf: xev.ReadBuffer,
        r: xev.ReadError!usize,
    ) xev.CallbackAction {
        const self = self_opt.?;
        const n = r catch |err| switch (err) {
            error.EOF => {
                self.beginClose(loop);
                return .disarm;
            },
            else => {
                std.log.debug("conn {d} read err: {s}", .{ self.router_conn.id, @errorName(err) });
                self.beginClose(loop);
                return .disarm;
            },
        };
        if (n == 0) {
            self.beginClose(loop);
            return .disarm;
        }

        const gpa = self.server.gpa;
        self.rx.appendSlice(gpa, buf.slice[0..n]) catch {
            self.beginClose(loop);
            return .disarm;
        };

        self.processRx(loop) catch {
            self.beginClose(loop);
            return .disarm;
        };

        self.maybeKickWrite(loop);
        return if (self.closing) .disarm else .rearm;
    }

    fn processRx(self: *Conn, loop: *xev.Loop) !void {
        _ = loop;
        const gpa = self.server.gpa;
        var cursor: usize = 0;
        while (cursor < self.rx.items.len) {
            const result = proto.parseClientOp(self.rx.items[cursor..]) catch |err| {
                const err_msg: []const u8 = switch (err) {
                    error.NeedMoreData => break,
                    error.UnknownOp => "Unknown Protocol Operation",
                    error.InvalidArgs, error.MalformedOp => "Invalid Operation",
                    error.ControlLineTooLong, error.PayloadTooLarge => {
                        try proto.writeErr(gpa, &self.router_conn.out, "Protocol Violation");
                        self.closing = true;
                        break;
                    },
                };
                try proto.writeErr(gpa, &self.router_conn.out, err_msg);
                const nl = std.mem.indexOfScalar(u8, self.rx.items[cursor..], '\n') orelse break;
                cursor += nl + 1;
                continue;
            };

            try self.handleOp(result.op);
            cursor += result.consumed;
        }

        if (cursor > 0) {
            const rest = self.rx.items.len - cursor;
            std.mem.copyForwards(u8, self.rx.items[0..rest], self.rx.items[cursor..]);
            self.rx.items.len = rest;
        }

        // Cap RX so a misbehaving client can't hog memory.
        if (self.rx.items.len > rx_max) return error.RxTooLarge;
    }

    fn handleOp(self: *Conn, op: proto.ClientOp) !void {
        const gpa = self.server.gpa;
        const router = self.server.router;
        const rconn = self.router_conn;

        switch (op) {
            .connect => {},
            .ping => try proto.writePong(gpa, &rconn.out),
            .pong => {},
            .sub => |s| {
                subject_mod.validateFilter(s.subject) catch {
                    try proto.writeErr(gpa, &rconn.out, "Invalid Subject");
                    return;
                };
                router.subscribe(rconn, s.subject, s.sid) catch |err| switch (err) {
                    error.LvcDisabled => try proto.writeErr(gpa, &rconn.out, "$LVC is disabled"),
                    else => try proto.writeErr(gpa, &rconn.out, "Subscribe Failed"),
                };
            },
            .unsub => |u| {
                router.unsubscribe(rconn, u.sid, u.max_msgs) catch {};
            },
            .pub_msg => |p| {
                subject_mod.validatePublish(p.subject) catch {
                    try proto.writeErr(gpa, &rconn.out, "Invalid Subject");
                    return;
                };
                if (std.mem.startsWith(u8, p.subject, router_mod.Router.lvc_prefix)) {
                    try proto.writeErr(gpa, &rconn.out, "$LVC is read-only");
                    return;
                }
                if (std.mem.startsWith(u8, p.subject, stats_prefix)) {
                    try proto.writeErr(gpa, &rconn.out, "$STATS is read-only");
                    return;
                }

                self.server.total_pubs += 1;

                _ = self.msg_arena.reset(.retain_capacity);
                const arena = self.msg_arena.allocator();
                const subject = try arena.dupe(u8, p.subject);
                const payload = try arena.dupe(u8, p.payload);

                var ctx: rules_mod.Context = .{
                    .subject = subject,
                    .payload = payload,
                    .publisher = router_mod.rulesPublisher(router),
                    .arena = arena,
                    .gpa = gpa,
                    .now_ms = self.server.loop.now(),
                    .trace = self.server.trace_enabled,
                    .reentry_ctx = self.server,
                    .reentry_fn = ruleReentry,
                };
                self.server.rules.run(&ctx) catch |err| {
                    std.log.warn("rule error: {s}", .{@errorName(err)});
                };

                if (ctx.rule_publishes >= rule_publish_warn_threshold) {
                    std.log.warn(
                        "patchbay amplification: subject={s} generated {d} publishes",
                        .{ subject, ctx.rule_publishes },
                    );
                }

                router.publish(subject, payload) catch |err| {
                    std.log.warn("publish error: {s}", .{@errorName(err)});
                };

                if (self.server.stats_enabled) self.server.recordPub(ctx.rule_publishes);
            },
        }
    }

    fn onKick(ctx: *anyopaque) void {
        const self: *Conn = @ptrCast(@alignCast(ctx));
        self.maybeKickWrite(self.server.loop);
    }

    fn maybeKickWrite(self: *Conn, loop: *xev.Loop) void {
        if (self.closing or self.write_in_flight) return;
        if (self.router_conn.out.items.len == 0) return;

        if (self.router_conn.out_hwm >= out_queued_warn_threshold) {
            std.log.warn(
                "conn {d} outbound hwm {d} bytes (slow consumer or fan-out blow-up)",
                .{ self.router_conn.id, self.router_conn.out_hwm },
            );
        }
        if (self.server.stats_enabled and self.router_conn.out_hwm > self.server.stats_max_out_hwm) {
            self.server.stats_max_out_hwm = self.router_conn.out_hwm;
        }
        self.router_conn.out_hwm = 0;

        // Swap the router's out buffer with the (empty) in_flight buffer.
        // After the write completes we reuse the now-empty buffer we just
        // handed off, so both keep their capacity.
        std.mem.swap(std.ArrayList(u8), &self.router_conn.out, &self.in_flight_buf);

        self.write_in_flight = true;
        self.tcp.write(
            loop,
            &self.write_completion,
            .{ .slice = self.in_flight_buf.items },
            Conn,
            self,
            onWrite,
        );
    }

    fn onWrite(
        self_opt: ?*Conn,
        loop: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        buf: xev.WriteBuffer,
        r: xev.WriteError!usize,
    ) xev.CallbackAction {
        const self = self_opt.?;
        const written = r catch |err| {
            std.log.debug("conn {d} write err: {s}", .{ self.router_conn.id, @errorName(err) });
            self.write_in_flight = false;
            self.beginClose(loop);
            return .disarm;
        };

        const total = buf.slice.len;
        if (written < total) {
            // Partial write: resubmit with the remainder on the same
            // completion. We're still "in flight"; don't clear state.
            // buf.slice[written..] is only valid because in_flight_buf is
            // not mutated while write_in_flight is true (maybeKickWrite
            // bails early). Don't break that invariant.
            self.tcp.write(
                loop,
                &self.write_completion,
                .{ .slice = buf.slice[written..] },
                Conn,
                self,
                onWrite,
            );
            return .disarm;
        }

        self.write_in_flight = false;
        self.in_flight_buf.clearRetainingCapacity();
        self.maybeKickWrite(loop);
        return .disarm;
    }

    fn beginClose(self: *Conn, loop: *xev.Loop) void {
        if (self.closing) return;
        std.log.info("conn {d} closed", .{self.router_conn.id});
        self.closing = true;
        self.router_conn.markClosed();
        self.server.router.removeAllFor(self.router_conn);
        self.tcp.close(loop, &self.close_completion, Conn, self, onClose);
    }

    fn onClose(
        self_opt: ?*Conn,
        _: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        _: xev.CloseError!void,
    ) xev.CallbackAction {
        const self = self_opt.?;
        self.deinit();
        return .disarm;
    }
};

/// Re-entry hook installed on every inbound `rules.Context`. After a
/// patchbay-emitted publish has been fanned out, the eval layer calls this
/// with the new (subject, payload) so downstream rules whose filter matches
/// the emitted subject can fire too. The depth check that prevents
/// runaway loops lives in `Context.emit` (caps at `Context.max_depth`),
/// not here, so this stays a thin dispatcher.
///
/// The child Context inherits arena, gpa, publisher, trace flag, and the
/// re-entry hook itself, but starts with fresh per-PUB counters
/// (`rule_publishes`, `trace_emissions`) and an incremented `depth`. The
/// parent's counters aren't bumped by this nested run; the only signal
/// that propagates back is the side-effecting publishes themselves.
fn ruleReentry(
    reentry_ctx: ?*anyopaque,
    parent: *rules_mod.Context,
    subject: []const u8,
    payload: []const u8,
) anyerror!void {
    const self: *Server = @ptrCast(@alignCast(reentry_ctx.?));
    var child: rules_mod.Context = .{
        .subject = subject,
        .payload = payload,
        .publisher = parent.publisher,
        .arena = parent.arena,
        .gpa = parent.gpa,
        .now_ms = parent.now_ms,
        .trace = parent.trace,
        .depth = parent.depth + 1,
        .max_depth = parent.max_depth,
        .reentry_ctx = parent.reentry_ctx,
        .reentry_fn = parent.reentry_fn,
    };
    try self.rules.run(&child);
}
