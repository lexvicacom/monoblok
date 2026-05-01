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

/// Hard cap on a conn's pending outbound bytes (router buf + in_flight).
/// Past this, send `-ERR 'Slow Consumer'` and close. Matches nats-server's
/// MAX_PENDING_SIZE.
pub const default_max_pending_bytes: usize = 64 * 1024 * 1024;

/// Server-side PING cadence. 0 disables PING + stale-conn reaper.
pub const default_ping_interval_ms: u64 = 120_000;

/// `--stats` flushes a summary line every this many inbound PUBs.
const stats_interval: u64 = 10_000;

/// Reserved prefix for the stats stream. Client PUBs are rejected; server
/// emits on a wall-clock tick. Excluded from the LVC (see router.publish).
const stats_prefix = "$STATS.";

/// Default wall-clock tick for `$STATS.*` publishes. Override via
/// `Server.stats_tick_ms` (CLI flag `--stats-tick-ms`).
pub const default_stats_tick_ms: u64 = 60_000;

/// Default wall-clock tick for the patchbay clock walker. The walker runs
/// `eval.tickClocks` over every rule's state to close any time-windowed
/// `bar` whose window elapsed without a closing PUB, and to evict stale
/// samples from `:ms`-windowed `moving-*` rings. 2s is a reasonable
/// idle-CPU vs latency tradeoff: time-bar closes land within ~half-tick
/// of their boundary, and the walker only does real work for `time_ring`
/// / time-bar slots. Override via `Server.clock_tick_ms` (CLI flag
/// `--clock-tick-ms`).
pub const default_clock_tick_ms: u64 = 2_000;

/// Shared-layout view of `bridge.Stats`. Server reads via `*const BridgeStats`
/// so it doesn't need to import bridge.zig directly.
pub const BridgeStats = extern struct {
    published: u64 = 0,
    dropped: u64 = 0,
};

pub const Server = struct {
    gpa: Allocator,
    loop: *xev.Loop,
    router: *router_mod.Router,
    rules: rules_mod.RuleSet,
    /// TCP listener (when `tcp_enabled`). xev.TCP is just a fd wrapper, so
    /// the unix listener below also reuses it.
    listener: xev.TCP = undefined,
    accept_completion: xev.Completion = undefined,
    tcp_enabled: bool = true,

    /// Optional AF_UNIX listener. Same accept/read/write flow as TCP
    /// (xev.TCP wraps any stream fd).
    unix_enabled: bool = false,
    unix_listener: xev.TCP = undefined,
    unix_accept_completion: xev.Completion = undefined,
    /// Path we bound to; unlinked at deinit so a restart won't trip EADDRINUSE.
    unix_path: ?[]const u8 = null,

    next_conn_id: u64 = 1,
    /// Process-wide unique id, generated once at startup. Sent in every
    /// INFO line's `server_id` / `server_name` fields.
    server_id: []const u8,
    listen_host: []const u8,
    listen_port: u16,
    /// Process role advertised in INFO. main.zig sets this to .worker when
    /// running with --inherit-fd; otherwise .standalone.
    mode: proto.Mode = .standalone,

    /// `--trace`: copied into `rules.Context.trace` per PUB.
    trace_enabled: bool = false,

    /// `--stats`: print running-max summaries every `stats_interval` PUBs.
    stats_enabled: bool = false,
    stats_pubs: u64 = 0,
    stats_max_rule_publishes: u32 = 0,
    stats_max_out_hwm: usize = 0,

    /// Cumulative inbound PUBs. Published on the `$STATS.*` tick.
    total_pubs: u64 = 0,

    /// Optional pointer to the bridge's counters; read on the stats tick.
    bridge_stats: ?*const BridgeStats = null,

    /// Periodic `$STATS.*` publisher; timer re-arms from its callback.
    stats_timer: xev.Timer = undefined,
    stats_completion: xev.Completion = undefined,
    stats_tick_ms: u64 = default_stats_tick_ms,

    /// Slow-consumer cap (per conn, total of out + in_flight).
    max_pending_bytes: usize = default_max_pending_bytes,

    /// Server-PING / stale-conn reaper cadence. 0 disables.
    ping_interval_ms: u64 = default_ping_interval_ms,
    ping_timer: xev.Timer = undefined,
    ping_completion: xev.Completion = undefined,
    /// Doubly-linked list of live conns, walked on each ping tick.
    conns_head: ?*Conn = null,

    /// Periodic patchbay clock walker; timer re-arms from its callback.
    /// Only armed if at least one rule body uses a `:ms` window. Without
    /// time-windowed ops the walker has nothing to do, so we skip it
    /// entirely (decided at `listen` time via `rulesUseTimeWindows`).
    clock_enabled: bool = false,
    clock_timer: xev.Timer = undefined,
    clock_completion: xev.Completion = undefined,
    clock_tick_ms: u64 = default_clock_tick_ms,
    /// Reset on each clock tick. Walker emits + ring eviction allocate
    /// here.
    clock_arena: std.heap.ArenaAllocator = undefined,

    /// LVC snapshot config. `snapshot_every_ms > 0` enables periodic dumps;
    /// `snapshot_io` is required when dumping.
    snapshot_path: ?[]const u8 = null,
    snapshot_every_ms: u64 = 0,
    snapshot_io: ?Io = null,
    snapshot_timer: xev.Timer = undefined,
    snapshot_completion: xev.Completion = undefined,
    /// Serialises dumps so a slow disk can't stack up multiple threads.
    snapshot_in_flight: std.atomic.Value(bool) = .init(false),

    /// Signal handler calls `shutdown_async.notify()`, firing `onShutdown`
    /// on the loop thread. Initialised only if `shutdown_enabled`.
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
        if (self.tcp_enabled) {
            self.listener = try xev.TCP.init(address);
            try self.listener.bind(address);
            try self.listener.listen(128);
            self.listen_port = address.getPort();
            self.listener.accept(self.loop, &self.accept_completion, Server, self, onAccept);
        }

        self.stats_timer = try xev.Timer.init();
        self.stats_timer.run(self.loop, &self.stats_completion, self.stats_tick_ms, Server, self, onStatsTick);

        // Worker mode (mixer-spawned) skips PING: the mixer doesn't speak it
        // and the inherited socketpair can't go stale.
        if (self.ping_interval_ms > 0 and self.mode != .worker) {
            self.ping_timer = try xev.Timer.init();
            // Tick at half the interval so we catch stale conns within ~1.5x.
            const tick = @max(self.ping_interval_ms / 2, 1);
            self.ping_timer.run(self.loop, &self.ping_completion, tick, Server, self, onPingTick);
        }

        self.clock_enabled = rules_mod.rulesUseTimeWindows(self.rules.rules);
        if (self.clock_enabled) {
            self.clock_arena = .init(self.gpa);
            self.clock_timer = try xev.Timer.init();
            self.clock_timer.run(self.loop, &self.clock_completion, self.clock_tick_ms, Server, self, onClockTick);
            std.log.info(
                "patchbay clock walker enabled, tick={d}ms (tune with --clock-tick-ms)",
                .{self.clock_tick_ms},
            );
        }

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

    /// Worker mode: take a pre-connected socketpair fd from the mixer and
    /// serve one Conn on it. No listen, no accept; on EOF the worker exits.
    pub fn serveInheritedFd(self: *Server, fd: std.posix.fd_t) !void {
        // Apply CLOEXEC + NONBLOCK ourselves; the mixer's posix_spawn passes
        // the fd as-is, so it's almost certainly blocking and unflagged.
        if (std.c.fcntl(fd, std.posix.F.SETFD, @as(c_int, std.posix.FD_CLOEXEC)) < 0) {
            return error.FcntlFailed;
        }
        const flags = std.c.fcntl(fd, std.posix.F.GETFL, @as(c_int, 0));
        if (flags < 0) return error.FcntlFailed;
        const nonblock_bit: c_int = @bitCast(std.posix.O{ .NONBLOCK = true });
        if (std.c.fcntl(fd, std.posix.F.SETFL, @as(c_int, flags | nonblock_bit)) < 0) {
            return error.FcntlFailed;
        }

        const tcp = xev.TCP.initFd(fd);
        const conn_state = try Conn.init(self, tcp);
        std.log.info("conn {d} accepted (inherited fd {d})", .{ conn_state.router_conn.id, fd });
        conn_state.start(self.loop);
    }

    /// Bind a listening AF_UNIX stream socket at `path` and feed accepted
    /// fds through the same `onAccept` path the TCP listener uses. Must be
    /// called before `loop.run` (during `Server` setup, after `listen`).
    /// `path` must outlive `Server` (we keep a slice, not a copy, to match
    /// `listen_host`). `io` is used only for the startup stat check.
    pub fn listenUnix(self: *Server, io: Io, path: []const u8) !void {
        var addr: std.posix.sockaddr.un = std.mem.zeroes(std.posix.sockaddr.un);
        addr.family = std.posix.AF.UNIX;
        if (path.len >= addr.path.len) return error.PathTooLong;
        @memcpy(addr.path[0..path.len], path);

        const path_z = try self.gpa.dupeZ(u8, path);
        defer self.gpa.free(path_z);

        // Best-effort cleanup of a stale socket file from a prior run. We
        // only unlink AF_UNIX socket files; a regular file at this path
        // probably means the user typo'd, so we refuse rather than nuke it.
        if (Io.Dir.cwd().statFile(io, path, .{})) |st| {
            if (st.kind == .unix_domain_socket) {
                _ = std.c.unlink(path_z.ptr);
            } else {
                return error.PathExistsAndIsNotASocket;
            }
        } else |err| switch (err) {
            error.FileNotFound => {},
            else => return err,
        }

        // SOCK.CLOEXEC is a Zig shim on Darwin (not a real socket(2) flag),
        // so create the socket plain and apply CLOEXEC + NONBLOCK via fcntl
        // afterwards. The listener fd must be non-blocking; libxev's accept
        // path expects the kernel to return EAGAIN rather than block.
        const fd_rc = std.c.socket(std.posix.AF.UNIX, std.posix.SOCK.STREAM, 0);
        if (fd_rc < 0) return error.SocketCreateFailed;
        const fd: std.posix.fd_t = @intCast(fd_rc);
        errdefer _ = std.c.close(fd);

        if (std.c.fcntl(fd, std.posix.F.SETFD, @as(c_int, std.posix.FD_CLOEXEC)) < 0) {
            return error.FcntlFailed;
        }
        const flags = std.c.fcntl(fd, std.posix.F.GETFL, @as(c_int, 0));
        if (flags < 0) return error.FcntlFailed;
        const nonblock_bit: c_int = @bitCast(std.posix.O{ .NONBLOCK = true });
        if (std.c.fcntl(fd, std.posix.F.SETFL, @as(c_int, flags | nonblock_bit)) < 0) {
            return error.FcntlFailed;
        }

        const sa: *const std.posix.sockaddr = @ptrCast(&addr);
        if (std.c.bind(fd, sa, @sizeOf(std.posix.sockaddr.un)) != 0) {
            return error.UnixBindFailed;
        }
        // Mode 0600 by default. Users who need group access can chmod from
        // a wrapper script; we don't want to surprise anyone with a
        // world-writable socket.
        if (std.c.chmod(path_z.ptr, 0o600) != 0) {
            std.log.warn("unix-socket: chmod 0600 {s} failed", .{path});
        }
        if (std.c.listen(fd, 128) != 0) return error.UnixListenFailed;

        self.unix_listener = xev.TCP.initFd(fd);
        self.unix_enabled = true;
        self.unix_path = path;
        self.unix_listener.accept(
            self.loop,
            &self.unix_accept_completion,
            Server,
            self,
            onAccept,
        );
    }

    pub fn deinit(self: *Server) void {
        self.stats_timer.deinit();
        if (self.ping_interval_ms > 0 and self.mode != .worker) self.ping_timer.deinit();
        if (self.clock_enabled) {
            self.clock_timer.deinit();
            self.clock_arena.deinit();
        }
        if (self.snapshot_path != null and self.snapshot_every_ms > 0) {
            self.snapshot_timer.deinit();
        }
        if (self.shutdown_enabled) self.shutdown_async.deinit();
        if (self.unix_path) |p| {
            const path_z = self.gpa.dupeZ(u8, p) catch return;
            defer self.gpa.free(path_z);
            _ = std.c.unlink(path_z.ptr);
        }
    }

    /// Async-signal-safe: `notify()` is a non-blocking write to an
    /// eventfd / mach port.
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
        // Loop is paused while we run; in-flight PUB bytes drop on exit,
        // same as any ungraceful NATS shutdown.
        std.log.info("shutdown: writing final snapshot...", .{});
        self.snapshotSync() catch |err| {
            std.log.warn("shutdown: snapshot failed: {s}", .{@errorName(err)});
        };
        loop.stop();
        return .disarm;
    }

    /// Synchronous snapshot. Shutdown-only; blocks the loop.
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

        self.stats_timer.run(loop, &self.stats_completion, self.stats_tick_ms, Server, self, onStatsTick);
        return .disarm;
    }

    fn onPingTick(
        self_opt: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.Timer.RunError!void,
    ) xev.CallbackAction {
        _ = r catch {};
        const self = self_opt.?;

        const now = loop.now();
        const idle_thresh: i64 = @intCast(self.ping_interval_ms);
        const stale_thresh: i64 = idle_thresh * 2;

        // Walk conn list. Closing a conn unlinks via deinit→onClose, but that
        // happens asynchronously, so iterating self.next is safe here.
        var c_opt = self.conns_head;
        while (c_opt) |c| {
            const next = c.next;
            if (!c.closing) {
                const idle = now - c.last_recv_ms;
                if (c.ping_outstanding and idle >= stale_thresh) {
                    std.log.info("conn {d} stale ({d}ms idle), closing", .{ c.router_conn.id, idle });
                    c.requestClose();
                } else if (!c.ping_outstanding and idle >= idle_thresh) {
                    proto.writePing(self.gpa, &c.router_conn.out) catch {};
                    c.ping_outstanding = true;
                    c.maybeKickWrite(loop);
                }
            }
            c_opt = next;
        }

        const tick = @max(self.ping_interval_ms / 2, 1);
        self.ping_timer.run(loop, &self.ping_completion, tick, Server, self, onPingTick);
        return .disarm;
    }

    fn onClockTick(
        self_opt: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.Timer.RunError!void,
    ) xev.CallbackAction {
        _ = r catch {};
        const self = self_opt.?;

        _ = self.clock_arena.reset(.retain_capacity);
        const arena = self.clock_arena.allocator();
        const publisher = router_mod.rulesPublisher(self.router);
        rules_mod.tickClocks(arena, self.rules.rules, self.loop.now(), publisher) catch |err| {
            std.log.warn("patchbay clock tick failed: {s}", .{@errorName(err)});
        };

        self.clock_timer.run(loop, &self.clock_completion, self.clock_tick_ms, Server, self, onClockTick);
        return .disarm;
    }

    /// Publish cumulative totals to `$STATS.*`. `router.publish` copies
    /// subject and payload, so the stack buffers are fine to reuse.
    fn emitStats(self: *Server) !void {
        try self.publishStat("$STATS.global.pubs", self.total_pubs);

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

    /// Loop-thread deep-copy into an arena, then hand off to a worker
    /// thread for serialise + atomic rename. Skips if a prior dump is still
    /// running. Worker only touches its private arena, so router/rule
    /// mutations after `collect` returns are safe.
    fn kickSnapshot(self: *Server) !void {
        const path = self.snapshot_path orelse return;
        const io = self.snapshot_io orelse return;

        if (self.snapshot_in_flight.swap(true, .acquire)) {
            std.log.warn("snapshot: previous dump still running, skipping tick", .{});
            return;
        }
        errdefer self.snapshot_in_flight.store(false, .release);

        // Worker owns this arena and frees it when done.
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
        c: *xev.Completion,
        r: xev.AcceptError!xev.TCP,
    ) xev.CallbackAction {
        const self = self_opt.?;
        // Re-arm the listener that fired. The completion pointer
        // disambiguates TCP vs unix.
        defer if (c == &self.unix_accept_completion) {
            self.unix_listener.accept(loop, &self.unix_accept_completion, Server, self, onAccept);
        } else {
            self.listener.accept(loop, &self.accept_completion, Server, self, onAccept);
        };

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
    prev: ?*Conn = null,
    next: ?*Conn = null,

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
            .last_recv_ms = server.loop.now(),
            .connect_ms = server.loop.now(),
        };

        rconn.kick_ctx = self;
        rconn.kick_fn = onKick;

        // Link into the server's conn list (head insert).
        self.next = server.conns_head;
        if (server.conns_head) |h| h.prev = self;
        server.conns_head = self;

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

        return self;
    }

    fn deinit(self: *Conn) void {
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

        self.maybeKickWrite(loop);
        return if (self.closing) .disarm else .rearm;
    }

    fn processRx(self: *Conn, loop: *xev.Loop) !void {
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

    fn handleOp(self: *Conn, op: proto.ClientOp) !void {
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
                router.subscribe(rconn, s.subject, s.sid) catch |err| switch (err) {
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
                        "patchbay amplification: subject={s} exploded into {d} publishes",
                        .{ subject, ctx.rule_publishes },
                    );
                }

                router.publish(subject, payload) catch |err| {
                    std.log.warn("publish error: {s}", .{@errorName(err)});
                };

                if (self.server.stats_enabled) self.server.recordPub(ctx.rule_publishes);
                if (self.verbose) try proto.writeOk(gpa, &rconn.out);
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
            // Resubmit the remainder. `buf.slice[written..]` stays valid
            // because `maybeKickWrite` won't touch `in_flight_buf` while
            // `write_in_flight` is true.
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

    /// Idempotent close request safe to call from outside an i/o callback
    /// (timer ticks etc.) when a read completion is still armed. Shuts the
    /// fd down so the in-flight read returns EOF; onRead then drives the
    /// real close via beginClose.
    fn requestClose(self: *Conn) void {
        if (self.closing or self.shutdown_requested) return;
        self.shutdown_requested = true;
        self.router_conn.markClosed();
        _ = std.c.shutdown(self.tcp.fd, std.c.SHUT.RDWR);
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
        .trace = parent.trace,
        .depth = parent.depth + 1,
        .max_depth = parent.max_depth,
        .reentry_ctx = parent.reentry_ctx,
        .reentry_fn = parent.reentry_fn,
    };
    try self.rules.run(&child);
}
