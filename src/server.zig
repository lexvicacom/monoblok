//! libxev-based TCP server. Single-threaded event loop; per-connection state
//! machine: on each read completion, parse as many complete ops as possible
//! from the rx buffer, dispatch them, then re-arm read. Writes are serialised
//! per-connection via xev.WriteQueue.
const std = @import("std");
const Allocator = std.mem.Allocator;
const xev = @import("xev");

const proto = @import("proto.zig");
const router_mod = @import("router.zig");
const rules_mod = @import("rules.zig");
const subject_mod = @import("subject.zig");

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
/// wall-clock tick. Subscribers read via `$LVC.$STATS.>` for last-value
/// or `$STATS.>` for live deltas.
const stats_prefix = "$STATS.";

/// Wall-clock tick for `$STATS.*` publishes.
const stats_tick_ms: u64 = 60_000;

pub const Server = struct {
    gpa: Allocator,
    loop: *xev.Loop,
    router: *router_mod.Router,
    rules: []rules_mod.Rule,
    listener: xev.TCP,
    accept_completion: xev.Completion = undefined,
    next_conn_id: u64 = 1,
    /// Process-wide unique id, generated once at startup. Sent in every
    /// INFO line's `server_id` / `server_name` fields.
    server_id: []const u8,
    listen_host: []const u8,
    listen_port: u16,

    /// `--stats` mode: print running-max summaries every `stats_interval`
    /// PUBs. Counters are reset each time the line is printed.
    stats_enabled: bool = false,
    stats_pubs: u64 = 0,
    stats_max_rule_publishes: u32 = 0,
    stats_max_out_hwm: usize = 0,

    /// Cumulative total of inbound client PUBs since server start. Published
    /// on the `$STATS.*` tick.
    total_pubs: u64 = 0,

    /// Periodic `$STATS.*` publisher state. Timer fires every
    /// `stats_tick_ms` and re-arms itself from the callback.
    stats_timer: xev.Timer = undefined,
    stats_completion: xev.Completion = undefined,

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
    }

    pub fn deinit(self: *Server) void {
        self.stats_timer.deinit();
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

        var subj_buf: [64]u8 = undefined;
        for (self.rules, 0..) |*rule, i| {
            const emit_subj = try std.fmt.bufPrint(&subj_buf, "$STATS.rules.{d}.emitted", .{i});
            try self.publishStat(emit_subj, rule.publishes_emitted);

            const supp_subj = try std.fmt.bufPrint(&subj_buf, "$STATS.rules.{d}.suppressed", .{i});
            try self.publishStat(supp_subj, rule.publishes_suppressed);
        }
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
                };
                rules_mod.run(self.server.rules, &ctx) catch |err| {
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

        const gpa = self.server.gpa;
        // Swap the router's out buffer with the (empty) in_flight buffer.
        // After the write completes we reuse the now-empty buffer we just
        // handed off, so both keep their capacity.
        std.mem.swap(std.ArrayList(u8), &self.router_conn.out, &self.in_flight_buf);
        _ = gpa;

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
