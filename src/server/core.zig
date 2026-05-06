//! libxev-based TCP server. Single-threaded event loop; per-connection state
//! machine: on each read completion, parse as many complete ops as possible
//! from the rx buffer, dispatch them, then re-arm read. Writes are serialised
//! per-connection via a single-in-flight guard (`write_in_flight`); see the
//! comment on the write-side fields for why we don't use xev.WriteQueue.
const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;
const xev = @import("xev");

const proto = @import("../proto.zig");
const router_mod = @import("../router.zig");
const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;
const snapshot_mod = @import("../snapshot.zig");
const clock_mod = @import("../clock.zig");

const Conn = @import("conn.zig").Conn(Server);

/// Hard cap on a conn's pending outbound bytes (router buf + in_flight).
/// Past this, send `-ERR 'Slow Consumer'` and close. Matches nats-server's
/// MAX_PENDING_SIZE.
pub const default_max_pending_bytes: usize = 64 * 1024 * 1024;

/// Server-side PING cadence. 0 disables PING + stale-conn reaper.
pub const default_ping_interval_ms: u64 = 120_000;

/// `--stats` flushes a summary line every this many inbound PUBs.
const stats_interval: u64 = 10_000;

/// Default wall-clock tick for `$STATS.*` publishes. Override via
/// `Server.stats_tick_ms` (CLI flag `--stats-tick-ms`).
pub const default_stats_tick_ms: u64 = 60_000;

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

    /// Per-slot clock registry (replaces the periodic walker). Built at
    /// `listen` time iff at least one rule body uses a `:ms` window. The
    /// registry installs a Context hook so time-windowed builtins
    /// (re)schedule a one-shot timer at each slot's exact next deadline.
    clock_registry: ?*clock_mod.Registry = null,

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

    pub fn recordPub(self: *Server, rule_publishes: u32) void {
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

        if (rules_mod.rulesUseTimeWindows(self.rules.rules)) {
            const reg = try self.gpa.create(clock_mod.Registry);
            reg.* = try clock_mod.Registry.init(
                self.gpa,
                self.loop,
                self.rules.rules,
                router_mod.rulesPublisher(self.router),
            );
            self.clock_registry = reg;
            const resumed = reg.armExisting();
            if (resumed.armed > 0) {
                std.log.info(
                    "patchbay clock registry enabled (per-slot deadlines, resumed {d} slot(s), {d} due now)",
                    .{ resumed.armed, resumed.due_now },
                );
            } else {
                std.log.info("patchbay clock registry enabled (per-slot deadlines)", .{});
            }
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
        self.waitSnapshotIdle();
        self.stats_timer.deinit();
        if (self.ping_interval_ms > 0 and self.mode != .worker) self.ping_timer.deinit();
        if (self.clock_registry) |reg| {
            reg.deinit();
            self.gpa.destroy(reg);
            self.clock_registry = null;
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
        _: ?*Server,
        loop: *xev.Loop,
        _: *xev.Completion,
        r: xev.Async.WaitError!void,
    ) xev.CallbackAction {
        _ = r catch {};
        // Snapshot runs after `loop.run` returns (in main.zig), not here.
        // Keeps this callback to a single non-blocking action.
        loop.stop();
        return .disarm;
    }

    /// Synchronous snapshot. Called after the loop stops, on the same
    /// (loop) thread, while router + rule state are still alive.
    pub fn snapshotSync(self: *Server) !void {
        const path = self.snapshot_path orelse return;
        const io = self.snapshot_io orelse return;

        var arena_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer arena_state.deinit();
        const arena = arena_state.allocator();

        const snap = try snapshot_mod.collect(arena, self.router, self.rules.rules);
        try snapshot_mod.writeFileAtomic(self.gpa, io, path, snap);
        std.log.info(
            "shutdown: snapshot written ({d} lvc / {d} rule-state entries) to {s}",
            .{ snap.lvc.len, snap.rule_state.len, path },
        );
    }

    pub fn waitSnapshotIdle(self: *Server) void {
        while (self.snapshot_in_flight.load(.acquire)) {
            std.Thread.yield() catch {};
        }
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
                const server = job.server;
                const gpa = server.gpa;
                snapshot_mod.writeFileAtomic(gpa, job.io, job.path, job.snap) catch |err| {
                    std.log.warn("snapshot: write failed: {s}", .{@errorName(err)});
                };
                std.log.info(
                    "snapshot: wrote {d} lvc / {d} rule-state entries",
                    .{ job.snap.lvc.len, job.snap.rule_state.len },
                );

                const ap = job.arena_ptr;
                ap.deinit();
                gpa.destroy(ap);
                gpa.destroy(job);
                server.snapshot_in_flight.store(false, .release);
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
        try self.router.publish(subject, null, payload);
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

        // TCP_NODELAY on the AF_INET socket. Without it, Nagle coalesces
        // small MSGs in the 1-pub/1-sub path and throughput drops by an
        // order of magnitude. Skip on AF_UNIX (option doesn't apply).
        if (c != &self.unix_accept_completion) {
            const enable: c_int = 1;
            std.posix.setsockopt(
                tcp.fd,
                std.posix.IPPROTO.TCP,
                std.posix.TCP.NODELAY,
                std.mem.asBytes(&enable),
            ) catch |err| {
                std.log.warn("TCP_NODELAY failed: {s}", .{@errorName(err)});
            };
        }

        const conn_state = Conn.init(self, tcp) catch |err| {
            std.log.warn("conn init failed: {s}", .{@errorName(err)});
            return .disarm;
        };
        std.log.info("conn {d} accepted", .{conn_state.router_conn.id});
        conn_state.start(loop);
        return .disarm;
    }
};
