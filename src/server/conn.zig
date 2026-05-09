//! Per-connection protocol state machine for the libxev server.
const std = @import("std");
const xev = @import("xev");

const proto = @import("../proto.zig");
const router_mod = @import("../router.zig");
const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;
const subject_mod = patchbay.subject;
const clock_mod = @import("../clock.zig");

const rx_max = proto.max_control_line + proto.max_payload + 64;

/// Warn (once per PUB) if the patchbay emits more than this many publishes
/// for a single inbound message. Catches rule-amplification blow-ups before
/// they become memory/rate problems.
const rule_publish_warn_threshold: u32 = 64;

/// Warn (once per PUB) if any conn's outbound buffer grew past this many
/// bytes during the dispatch. Catches slow-consumer / fan-out blow-ups.
const out_queued_warn_threshold: usize = 4 * 1024 * 1024;

/// Reserved prefix for the stats stream. Client PUBs are rejected; server
/// emits on a wall-clock tick. Excluded from the LVC (see router.publish).
const stats_prefix = "$STATS.";

pub fn Conn(comptime Server: type) type {
    return struct {
        const Self = @This();
        server: *Server,
        tcp: xev.TCP,
        router_conn: *router_mod.Conn,

        rx: std.ArrayList(u8) = .empty,
        read_completion: xev.Completion = undefined,
        read_buf: [16 * 1024]u8 = undefined,

        // We serialise writes ourselves via `write_in_flight` and use the
        // non-queued `write` API; `xev.WriteQueue` raced under heavy load.
        write_completion: xev.Completion = undefined,
        write_in_flight: bool = false,
        // Swapped with `router_conn.out` on each drain so appends can keep
        // landing while the write is flying.
        in_flight_buf: std.ArrayList(u8) = .empty,

        close_completion: xev.Completion = undefined,
        closing: bool = false,
        close_cleanup_done: bool = false,
        close_started: bool = false,
        shutdown_requested: bool = false,

        // Reset between messages to bound per-rule allocation lifetime.
        msg_arena: std.heap.ArenaAllocator,

        /// CONNECT options.
        verbose: bool = false,

        /// Reaper bookkeeping. last_recv_ms uses loop.now() (monotonic ms).
        last_recv_ms: i64 = 0,
        connect_ms: i64 = 0,
        ping_outstanding: bool = false,

        /// Intrusive list links for Server.conns_head.
        prev: ?*Self = null,
        next: ?*Self = null,

        pub fn init(server: *Server, tcp: xev.TCP) !*Self {
            const gpa = server.gpa;

            const self = try gpa.create(Self);
            errdefer gpa.destroy(self);

            const rconn = try gpa.create(router_mod.Conn);

            const id = server.next_conn_id;
            server.next_conn_id += 1;

            rconn.* = .{ .id = id, .gpa = gpa };
            errdefer rconn.deinit();

            self.* = .{
                .server = server,
                .tcp = tcp,
                .router_conn = rconn,
                .msg_arena = .init(gpa),
                .last_recv_ms = server.loop.now(),
                .connect_ms = server.loop.now(),
            };
            errdefer self.msg_arena.deinit();

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
                server.mode,
            );

            // Link into the server's conn list (head insert) only after all
            // fallible initialization has completed.
            self.next = server.conns_head;
            if (server.conns_head) |h| h.prev = self;
            server.conns_head = self;

            return self;
        }

        fn deinit(self: *Self) void {
            const gpa = self.server.gpa;
            // Unlink from the server's conn list.
            if (self.prev) |p| p.next = self.next else self.server.conns_head = self.next;
            if (self.next) |n| n.prev = self.prev;
            self.rx.deinit(gpa);
            self.in_flight_buf.deinit(gpa);
            self.msg_arena.deinit();
            self.router_conn.deinit();
            gpa.destroy(self);
        }

        pub fn start(self: *Self, loop: *xev.Loop) void {
            self.maybeKickWrite(loop);
            self.startRead(loop);
        }

        fn startRead(self: *Self, loop: *xev.Loop) void {
            if (self.closing) return;
            self.tcp.read(
                loop,
                &self.read_completion,
                .{ .slice = &self.read_buf },
                Self,
                self,
                onRead,
            );
        }

        fn onRead(
            self_opt: ?*Self,
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

            // Any inbound byte resets the reaper.
            self.last_recv_ms = loop.now();
            self.ping_outstanding = false;

            const gpa = self.server.gpa;
            self.rx.appendSlice(gpa, buf.slice[0..n]) catch {
                self.beginClose(loop);
                return .disarm;
            };

            self.processRx(loop) catch {
                self.beginClose(loop);
                return .disarm;
            };
            if (self.closing) {
                self.beginClose(loop);
                return .disarm;
            }

            self.maybeKickWrite(loop);
            return if (self.closing) .disarm else .rearm;
        }

        fn processRx(self: *Self, loop: *xev.Loop) !void {
            _ = loop;
            const gpa = self.server.gpa;
            var cursor: usize = 0;
            while (cursor < self.rx.items.len) {
                const result = proto.parseClientOp(self.rx.items[cursor..]) catch |err| switch (err) {
                    error.NeedMoreData => break,
                    error.ControlLineTooLong, error.PayloadTooLarge => {
                        try proto.writeErr(gpa, &self.router_conn.out, "Protocol Violation");
                        self.closing = true;
                        break;
                    },
                    error.UnknownOp, error.InvalidArgs, error.MalformedOp => {
                        const msg: []const u8 = if (err == error.UnknownOp)
                            "Unknown Protocol Operation"
                        else
                            "Invalid Operation";
                        try proto.writeErr(gpa, &self.router_conn.out, msg);
                        const nl = std.mem.indexOfScalar(u8, self.rx.items[cursor..], '\n') orelse break;
                        cursor += nl + 1;
                        continue;
                    },
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

        fn handleOp(self: *Self, op: proto.ClientOp) !void {
            const gpa = self.server.gpa;
            const router = self.server.router;
            const rconn = self.router_conn;

            switch (op) {
                .connect => |body| {
                    const opts = proto.parseConnect(gpa, body);
                    self.verbose = opts.verbose;
                    if (self.verbose) try proto.writeOk(gpa, &rconn.out);
                },
                .ping => try proto.writePong(gpa, &rconn.out),
                .pong => {},
                .sub => |s| {
                    subject_mod.validateFilter(s.subject) catch {
                        try proto.writeErr(gpa, &rconn.out, "Invalid Subject");
                        return;
                    };
                    router.subscribe(rconn, s.subject, s.sid, s.queue) catch |err| switch (err) {
                        error.LvcDisabled => try proto.writeErr(gpa, &rconn.out, "$LVC is disabled"),
                        else => try proto.writeErr(gpa, &rconn.out, "Subscribe Failed"),
                    };
                    if (self.verbose) try proto.writeOk(gpa, &rconn.out);
                },
                .unsub => |u| {
                    router.unsubscribe(rconn, u.sid, u.max_msgs) catch {};
                    if (self.verbose) try proto.writeOk(gpa, &rconn.out);
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

                    if (self.server.rules.rules.len == 0) {
                        router.publishRequest(p.subject, p.headers, p.payload, p.reply, p.no_responders) catch |err| {
                            std.log.warn("publish error: {s}", .{@errorName(err)});
                        };
                        if (self.server.stats_enabled) self.server.recordPub(0);
                        if (self.verbose) try proto.writeOk(gpa, &rconn.out);
                        return;
                    }

                    _ = self.msg_arena.reset(.retain_capacity);
                    const arena = self.msg_arena.allocator();
                    const subject = try arena.dupe(u8, p.subject);
                    const payload = try arena.dupe(u8, p.payload);
                    const headers: ?[]const u8 = if (p.headers) |h| try arena.dupe(u8, h) else null;

                    var ctx: rules_mod.Context = .{
                        .subject = subject,
                        .payload = payload,
                        .publisher = router_mod.rulesPublisher(router),
                        .arena = arena,
                        .gpa = gpa,
                        .now_ms = self.server.loop.now(),
                        .wall_ms = wallClockMs(),
                        .trace = self.server.trace_enabled,
                        .reentry_ctx = self.server,
                        .reentry_fn = ruleReentry,
                        .clock_hook_ctx = self.server.clock_registry,
                        .clock_hook_fn = if (self.server.clock_registry != null) clock_mod.hookFn else null,
                    };
                    self.server.rules.run(&ctx) catch |err| {
                        std.log.warn("rule error: {s}", .{@errorName(err)});
                    };

                    if (ctx.rule_publishes >= rule_publish_warn_threshold) {
                        std.log.warn(
                            "patchbay amplification: subject={s} exploded into {d} publishes",
                            .{ subject, ctx.rule_publishes },
                        );
                    }

                    router.publishRequest(subject, headers, payload, p.reply, p.no_responders) catch |err| {
                        std.log.warn("publish error: {s}", .{@errorName(err)});
                    };

                    if (self.server.stats_enabled) self.server.recordPub(ctx.rule_publishes);
                    if (self.verbose) try proto.writeOk(gpa, &rconn.out);
                },
            }
        }

        fn onKick(ctx: *anyopaque) void {
            const self: *Self = @ptrCast(@alignCast(ctx));
            self.maybeKickWrite(self.server.loop);
        }

        pub fn maybeKickWrite(self: *Self, loop: *xev.Loop) void {
            if (self.closing or self.write_in_flight) return;
            if (self.router_conn.out.items.len == 0) return;

            // Slow-consumer cap: total pending = router buf + (already-flying) in_flight.
            const pending = self.router_conn.out.items.len + self.in_flight_buf.items.len;
            if (pending > self.server.max_pending_bytes) {
                const now = loop.now();
                std.log.warn(
                    "conn {d} slow consumer, closing: pending={d}B cap={d}B age={d}ms idle={d}ms hwm={d}B",
                    .{
                        self.router_conn.id,
                        pending,
                        self.server.max_pending_bytes,
                        now - self.connect_ms,
                        now - self.last_recv_ms,
                        self.router_conn.out_hwm,
                    },
                );
                self.requestClose();
                return;
            }

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
                Self,
                self,
                onWrite,
            );
        }

        fn onWrite(
            self_opt: ?*Self,
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
            if (self.closing) {
                self.write_in_flight = false;
                self.in_flight_buf.clearRetainingCapacity();
                self.beginClose(loop);
                return .disarm;
            }
            if (written < total) {
                // Resubmit the remainder. `buf.slice[written..]` stays valid
                // because `maybeKickWrite` won't touch `in_flight_buf` while
                // `write_in_flight` is true.
                self.tcp.write(
                    loop,
                    &self.write_completion,
                    .{ .slice = buf.slice[written..] },
                    Self,
                    self,
                    onWrite,
                );
                return .disarm;
            }

            self.write_in_flight = false;
            self.in_flight_buf.clearRetainingCapacity();
            if (self.closing) {
                self.beginClose(loop);
                return .disarm;
            }
            self.maybeKickWrite(loop);
            return .disarm;
        }

        fn beginClose(self: *Self, loop: *xev.Loop) void {
            self.closing = true;
            if (!self.close_cleanup_done) {
                std.log.info("conn {d} closed", .{self.router_conn.id});
                self.router_conn.markClosed();
                self.server.router.removeAllFor(self.router_conn);
                self.close_cleanup_done = true;
            }
            if (self.close_started or self.write_in_flight) return;
            self.close_started = true;
            self.tcp.close(loop, &self.close_completion, Self, self, onClose);
        }

        /// Idempotent close request safe to call from outside an i/o callback
        /// (timer ticks etc.) when a read completion is still armed. Shuts the
        /// fd down so the in-flight read returns EOF; onRead then drives the
        /// real close via beginClose.
        pub fn requestClose(self: *Self) void {
            if (self.closing or self.shutdown_requested) return;
            self.shutdown_requested = true;
            self.router_conn.markClosed();
            _ = std.c.shutdown(self.tcp.fd, std.c.SHUT.RDWR);
        }

        fn onClose(
            self_opt: ?*Self,
            _: *xev.Loop,
            _: *xev.Completion,
            _: xev.TCP,
            _: xev.CloseError!void,
        ) xev.CallbackAction {
            const self = self_opt.?;
            self.deinit();
            return .disarm;
        }

        /// Dispatch a patchbay-emitted publish back through the ruleset on a child
        /// Context with `depth + 1`. The depth cap lives in `Context.emit`.
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
                .wall_ms = parent.wall_ms,
                .trace = parent.trace,
                .depth = parent.depth + 1,
                .max_depth = parent.max_depth,
                .reentry_ctx = parent.reentry_ctx,
                .reentry_fn = parent.reentry_fn,
                .clock_hook_ctx = parent.clock_hook_ctx,
                .clock_hook_fn = parent.clock_hook_fn,
            };
            try self.rules.run(&child);
        }
    };
}

/// Wall-clock ms since 1970-01-01 UTC. Stamped per-PUB onto
/// `Context.wall_ms` for `date-now` / `hour-now`. Direct syscall:
/// `std.time.milliTimestamp` was removed in Zig 0.16.
fn wallClockMs() i64 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.REALTIME, &ts);
    return @as(i64, @intCast(ts.sec)) *| std.time.ms_per_s +| @divFloor(ts.nsec, std.time.ns_per_ms);
}
