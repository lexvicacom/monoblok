// Stateless mixer. Spawns one monoblok worker per shard over a socketpair
// and forwards client PUBs to the worker owning the first subject token.
// SUB/UNSUB and worker->client MSGs are not routed in this skeleton.

const std = @import("std");
const builtin = @import("builtin");
const Allocator = std.mem.Allocator;
const xev = @import("xev");

const proto = @import("proto.zig");
const mixer_config = @import("mixer_config.zig");

extern "c" fn fork() std.posix.pid_t;
extern "c" fn execvp(file: [*:0]const u8, argv: [*:null]const ?[*:0]const u8) c_int;
extern "c" fn _exit(status: c_int) noreturn;
extern "c" fn socketpair(domain: c_int, ty: c_int, protocol: c_int, sv: *[2]c_int) c_int;
extern "c" fn kill(pid: std.posix.pid_t, sig: c_int) c_int;

const SIGTERM: c_int = 15;

var mixer_id_buf: [16]u8 = undefined;

pub const Mixer = struct {
    gpa: Allocator,
    loop: *xev.Loop,
    cfg: mixer_config.Config,

    listener: xev.TCP = undefined,
    accept_completion: xev.Completion = undefined,

    workers: []Worker = &.{},
    worker_by_shard: std.StringHashMapUnmanaged(*Worker) = .empty,

    next_conn_id: u64 = 1,

    shutdown_async: xev.Async = undefined,
    shutdown_completion: xev.Completion = undefined,

    pub fn start(self: *Mixer) !void {
        var ts: std.posix.timespec = undefined;
        _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
        const seed: u64 = @intCast(@as(i128, ts.sec) * std.time.ns_per_s + @as(i128, ts.nsec));
        var rng = std.Random.DefaultPrng.init(seed);
        var rb: [8]u8 = undefined;
        rng.random().bytes(&rb);
        for (rb, 0..) |b, j| {
            _ = std.fmt.bufPrint(mixer_id_buf[j * 2 ..][0..2], "{X:0>2}", .{b}) catch unreachable;
        }

        self.workers = try self.gpa.alloc(Worker, self.cfg.workers.len);
        for (self.cfg.workers, 0..) |w, idx| {
            self.workers[idx] = .{ .mixer = self, .cfg_idx = idx, .shard = w.shard };
        }
        try self.buildShardIndex();
        for (self.workers) |*w| try w.spawn(self.gpa, &self.cfg.workers[w.cfg_idx]);
        for (self.workers) |*w| w.startUpstreamRead(self.loop);

        const addr = try parseListen(self.cfg.listen);
        self.listener = try xev.TCP.init(addr);
        try self.listener.bind(addr);
        try self.listener.listen(128);
        self.listener.accept(self.loop, &self.accept_completion, Mixer, self, onAccept);
        std.log.info("mixer listening on {f} workers={d}", .{ addr, self.workers.len });

        self.shutdown_async = try xev.Async.init();
        self.shutdown_async.wait(self.loop, &self.shutdown_completion, Mixer, self, onShutdown);
    }

    pub fn deinit(self: *Mixer) void {
        for (self.workers) |*w| w.shutdown();
        self.worker_by_shard.deinit(self.gpa);
        self.gpa.free(self.workers);
    }

    pub fn requestShutdown(self: *Mixer) void {
        self.shutdown_async.notify() catch {};
    }

    fn onShutdown(self_opt: ?*Mixer, loop: *xev.Loop, _: *xev.Completion, r: xev.Async.WaitError!void) xev.CallbackAction {
        _ = r catch {};
        const self = self_opt.?;
        std.log.info("mixer: shutting down, signalling workers", .{});
        for (self.workers) |*w| w.terminate();
        loop.stop();
        return .disarm;
    }

    pub fn workerFor(self: *Mixer, subject: []const u8) *Worker {
        const dot = std.mem.indexOfScalar(u8, subject, '.') orelse subject.len;
        const tok = subject[0..dot];
        if (self.worker_by_shard.count() != 0) {
            return self.worker_by_shard.get(tok) orelse &self.workers[self.cfg.catch_all];
        }
        for (self.workers) |*w| {
            if (std.mem.eql(u8, w.shard, tok)) return w;
        }
        return &self.workers[self.cfg.catch_all];
    }

    fn buildShardIndex(self: *Mixer) !void {
        for (self.workers) |*w| {
            if (std.mem.eql(u8, w.shard, "*")) continue;
            try self.worker_by_shard.put(self.gpa, w.shard, w);
        }
    }

    fn onAccept(self_opt: ?*Mixer, loop: *xev.Loop, _: *xev.Completion, r: xev.AcceptError!xev.TCP) xev.CallbackAction {
        const self = self_opt.?;
        defer self.listener.accept(loop, &self.accept_completion, Mixer, self, onAccept);

        const tcp = r catch |err| {
            std.log.warn("mixer accept: {s}", .{@errorName(err)});
            return .disarm;
        };
        const c = ClientConn.init(self, tcp) catch |err| {
            std.log.warn("mixer client init: {s}", .{@errorName(err)});
            return .disarm;
        };
        std.log.info("mixer client {d} accepted", .{c.id});
        c.start(loop);
        return .disarm;
    }
};

pub const FilterEntry = struct {
    filter: []u8,
    /// Internal sid sent to the worker (decimal of an i64 counter, owned).
    internal_sid: []u8,
    internal_sid_num: u64,
    subscribers: std.ArrayList(Subscriber) = .empty,
};

pub const Subscriber = struct {
    client: *ClientConn,
    /// Client's own sid string (heap-owned, freed on UNSUB / disconnect).
    client_sid: []u8,
};

pub const Worker = struct {
    mixer: *Mixer,
    cfg_idx: usize,
    shard: []const u8,
    pid: std.posix.pid_t = 0,
    upstream: xev.TCP = undefined,
    fd: std.posix.fd_t = -1,

    read_completion: xev.Completion = undefined,
    read_buf: [16 * 1024]u8 = undefined,
    /// Sticky parser buffer for bytes coming back from the worker.
    rx: std.ArrayList(u8) = .empty,

    write_completion: xev.Completion = undefined,
    write_in_flight: bool = false,
    out: std.ArrayList(u8) = .empty,
    in_flight: std.ArrayList(u8) = .empty,

    /// Coalesce table: each unique filter has one entry, one upstream SUB.
    filter_by_str: std.StringHashMapUnmanaged(*FilterEntry) = .empty,
    /// Dense internal-sid index. Mixer-minted sids start at 1, so slot 0 stays
    /// unused and `entries_by_sid[n]` is the filter entry for worker sid `n`.
    entries_by_sid: std.ArrayList(?*FilterEntry) = .empty,
    next_internal_sid: u64 = 1,

    closing: bool = false,

    fn spawn(self: *Worker, gpa: Allocator, wcfg: *const mixer_config.Worker) !void {
        const SOCK_STREAM: c_int = std.posix.SOCK.STREAM;
        const AF_UNIX: c_int = std.posix.AF.UNIX;
        var sv: [2]c_int = .{ -1, -1 };
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, &sv) != 0) return error.SocketpairFailed;

        const child_fd = sv[1];
        const parent_fd = sv[0];

        // Child dup2s its socketpair end to fd 3 before exec.
        const child_inherit_fd: c_int = 3;

        var exe_buf: [4096]u8 = undefined;
        const exe = selfExe(&exe_buf) catch "monoblok";

        var argv_buf: std.ArrayList([]const u8) = .empty;
        defer argv_buf.deinit(gpa);
        try argv_buf.append(gpa, exe);
        try argv_buf.append(gpa, "--inherit-fd");
        try argv_buf.append(gpa, "3");
        if (wcfg.trace) try argv_buf.append(gpa, "--trace");
        try argv_buf.append(gpa, wcfg.patchbay);

        const argz = try gpa.allocSentinel(?[*:0]const u8, argv_buf.items.len, null);
        const argz_strings = try gpa.alloc([:0]u8, argv_buf.items.len);
        for (argv_buf.items, 0..) |s, i| {
            argz_strings[i] = try gpa.dupeZ(u8, s);
            argz[i] = argz_strings[i].ptr;
        }
        defer {
            for (argz_strings) |z| gpa.free(z);
            gpa.free(argz_strings);
            gpa.free(argz);
        }

        const exe_z = try gpa.dupeZ(u8, exe);
        defer gpa.free(exe_z);

        const pid = fork();
        if (pid < 0) return error.ForkFailed;
        if (pid == 0) {
            _ = std.c.close(parent_fd);
            if (child_fd != child_inherit_fd) {
                if (std.c.dup2(child_fd, child_inherit_fd) < 0) _exit(127);
                _ = std.c.close(child_fd);
            }
            if (wcfg.log) |lp| {
                const lp_z = gpa.dupeZ(u8, lp) catch _exit(127);
                const flags: std.c.O = .{
                    .ACCMODE = .WRONLY,
                    .CREAT = true,
                    .TRUNC = true,
                };
                const lf = std.c.open(lp_z.ptr, flags, @as(std.c.mode_t, 0o644));
                if (lf < 0) _exit(127);
                if (std.c.dup2(lf, 2) < 0) _exit(127);
                _ = std.c.close(lf);
            }
            _ = execvp(exe_z.ptr, argz.ptr);
            _exit(127);
        }

        _ = std.c.close(child_fd);
        self.pid = pid;
        self.fd = parent_fd;

        const flags = std.c.fcntl(parent_fd, std.posix.F.GETFL, @as(c_int, 0));
        if (flags >= 0) {
            const nonblock_bit: c_int = @bitCast(std.posix.O{ .NONBLOCK = true });
            _ = std.c.fcntl(parent_fd, std.posix.F.SETFL, @as(c_int, flags | nonblock_bit));
        }
        _ = std.c.fcntl(parent_fd, std.posix.F.SETFD, @as(c_int, std.posix.FD_CLOEXEC));
        self.upstream = xev.TCP.initFd(parent_fd);

        std.log.info("mixer spawned worker shard={s} pid={d} fd={d} patchbay={s}", .{
            wcfg.shard, pid, parent_fd, wcfg.patchbay,
        });
    }

    fn startUpstreamRead(self: *Worker, loop: *xev.Loop) void {
        self.upstream.read(loop, &self.read_completion, .{ .slice = &self.read_buf }, Worker, self, onUpstreamRead);
    }

    fn onUpstreamRead(
        self_opt: ?*Worker,
        loop: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        buf: xev.ReadBuffer,
        r: xev.ReadError!usize,
    ) xev.CallbackAction {
        const self = self_opt.?;
        const n = r catch |err| switch (err) {
            error.EOF => {
                std.log.warn("mixer: worker shard={s} pid={d} EOF; exiting", .{ self.shard, self.pid });
                loop.stop();
                return .disarm;
            },
            else => {
                std.log.warn("mixer: worker shard={s} read err: {s}; exiting", .{ self.shard, @errorName(err) });
                loop.stop();
                return .disarm;
            },
        };
        if (n == 0) {
            std.log.warn("mixer: worker shard={s} closed; exiting", .{self.shard});
            loop.stop();
            return .disarm;
        }
        self.rx.appendSlice(self.mixer.gpa, buf.slice[0..n]) catch {
            loop.stop();
            return .disarm;
        };
        self.processUpstream(loop) catch |err| {
            std.log.warn("mixer: upstream parse err: {s}; exiting", .{@errorName(err)});
            loop.stop();
            return .disarm;
        };
        return .rearm;
    }

    fn processUpstream(self: *Worker, loop: *xev.Loop) !void {
        _ = loop;
        var kicks: KickQueue = .{};

        var cursor: usize = 0;
        while (cursor < self.rx.items.len) {
            const slice = self.rx.items[cursor..];
            const result = proto.parseServerOp(slice) catch |err| switch (err) {
                error.NeedMoreData => break,
                else => return err,
            };
            switch (result.op) {
                .info => {},
                .pong, .ping, .ok => {},
                .err => |e| std.log.warn("mixer: worker shard={s} -ERR {s}", .{ self.shard, e }),
                .msg => |m| try self.dispatchMsg(m, slice[0..result.consumed], &kicks),
            }
            cursor += result.consumed;
        }
        if (cursor > 0) {
            const rest = self.rx.items.len - cursor;
            std.mem.copyForwards(u8, self.rx.items[0..rest], self.rx.items[cursor..]);
            self.rx.items.len = rest;
        }
        kicks.flush(self.mixer.loop);
    }

    fn dispatchMsg(self: *Worker, m: proto.ServerOp.Msg, wire: []const u8, kicks: *KickQueue) !void {
        const entry = self.entryForSid(m.sid) orelse {
            std.log.warn("mixer: MSG with unknown sid {s} from shard={s}", .{ m.sid, self.shard });
            return;
        };

        if (m.headers == null and m.reply == null and entry.subscribers.items.len == 1) {
            const sub = entry.subscribers.items[0];
            if (!sub.client.closing and std.mem.eql(u8, sub.client_sid, m.sid)) {
                try sub.client.out.appendSlice(self.mixer.gpa, wire);
                kicks.push(sub.client);
                return;
            }
        }

        for (entry.subscribers.items) |sub| {
            if (sub.client.closing) continue;
            if (m.headers) |h| {
                try proto.writeHmsg(self.mixer.gpa, &sub.client.out, m.subject, sub.client_sid, m.reply, h, m.payload);
            } else {
                try proto.writeMsg(self.mixer.gpa, &sub.client.out, m.subject, sub.client_sid, m.reply, m.payload);
            }
            kicks.push(sub.client);
        }
    }

    /// Add a client subscriber for `filter`. If this is the first subscriber
    /// for that filter on this worker, sends SUB upstream. Returns the
    /// FilterEntry so the client can record it for UNSUB / disconnect.
    pub fn addSubscriber(self: *Worker, loop: *xev.Loop, client: *ClientConn, filter: []const u8, client_sid: []const u8) !*FilterEntry {
        const gpa = self.mixer.gpa;
        if (self.filter_by_str.get(filter)) |entry| {
            const sid_owned = try gpa.dupe(u8, client_sid);
            errdefer gpa.free(sid_owned);
            try entry.subscribers.append(gpa, .{ .client = client, .client_sid = sid_owned });
            return entry;
        }

        const entry = try gpa.create(FilterEntry);
        errdefer gpa.destroy(entry);

        const filter_owned = try gpa.dupe(u8, filter);
        errdefer gpa.free(filter_owned);

        const sid_num = self.next_internal_sid;
        self.next_internal_sid += 1;
        const internal_sid = blk: {
            var buf: [24]u8 = undefined;
            const s = try std.fmt.bufPrint(&buf, "{d}", .{sid_num});
            break :blk try gpa.dupe(u8, s);
        };
        errdefer gpa.free(internal_sid);

        entry.* = .{ .filter = filter_owned, .internal_sid = internal_sid, .internal_sid_num = sid_num };

        const sid_owned = try gpa.dupe(u8, client_sid);
        var entry_owns_subs = false;
        errdefer if (!entry_owns_subs) gpa.free(sid_owned);
        try entry.subscribers.append(gpa, .{ .client = client, .client_sid = sid_owned });
        entry_owns_subs = true;
        errdefer if (entry_owns_subs) {
            for (entry.subscribers.items) |s| gpa.free(s.client_sid);
            entry.subscribers.deinit(gpa);
        };

        try self.filter_by_str.put(gpa, entry.filter, entry);
        var filter_registered = true;
        errdefer if (filter_registered) {
            _ = self.filter_by_str.remove(entry.filter);
        };

        try self.registerSidEntry(gpa, sid_num, entry);
        var sid_registered = true;
        errdefer if (sid_registered) self.unregisterSidEntry(sid_num);

        // Send SUB upstream.
        var buf: [proto.max_control_line]u8 = undefined;
        const line = try std.fmt.bufPrint(&buf, "SUB {s} {s}\r\n", .{ entry.filter, entry.internal_sid });
        try self.out.appendSlice(gpa, line);
        filter_registered = false;
        sid_registered = false;
        entry_owns_subs = false;
        self.maybeKickWrite(loop);
        return entry;
    }

    fn registerSidEntry(self: *Worker, gpa: Allocator, sid: u64, entry: *FilterEntry) !void {
        const idx: usize = @intCast(sid);
        if (self.entries_by_sid.items.len <= idx) {
            const old_len = self.entries_by_sid.items.len;
            try self.entries_by_sid.resize(gpa, idx + 1);
            @memset(self.entries_by_sid.items[old_len..], null);
        }
        self.entries_by_sid.items[idx] = entry;
    }

    fn unregisterSidEntry(self: *Worker, sid: u64) void {
        const idx: usize = @intCast(sid);
        if (idx < self.entries_by_sid.items.len) self.entries_by_sid.items[idx] = null;
    }

    fn entryForSid(self: *Worker, sid: []const u8) ?*FilterEntry {
        var n: usize = 0;
        for (sid) |c| {
            if (c < '0' or c > '9') return null;
            n = std.math.mul(usize, n, 10) catch return null;
            n = std.math.add(usize, n, @as(usize, @intCast(c - '0'))) catch return null;
        }
        if (n == 0 or n >= self.entries_by_sid.items.len) return null;
        return self.entries_by_sid.items[n];
    }

    /// Remove one subscriber from `entry`. If the list empties, send UNSUB
    /// upstream and free the entry.
    pub fn removeSubscriber(self: *Worker, loop: *xev.Loop, entry: *FilterEntry, client: *ClientConn, client_sid: []const u8) void {
        const gpa = self.mixer.gpa;
        var i: usize = 0;
        while (i < entry.subscribers.items.len) : (i += 1) {
            const s = entry.subscribers.items[i];
            if (s.client == client and std.mem.eql(u8, s.client_sid, client_sid)) {
                gpa.free(s.client_sid);
                _ = entry.subscribers.swapRemove(i);
                break;
            }
        }
        if (entry.subscribers.items.len > 0) return;

        // Last subscriber gone: send UNSUB and tear down the entry.
        var buf: [proto.max_control_line]u8 = undefined;
        const line = std.fmt.bufPrint(&buf, "UNSUB {s}\r\n", .{entry.internal_sid}) catch return;
        self.out.appendSlice(gpa, line) catch {};
        self.maybeKickWrite(loop);

        _ = self.filter_by_str.remove(entry.filter);
        self.unregisterSidEntry(entry.internal_sid_num);
        entry.subscribers.deinit(gpa);
        gpa.free(entry.filter);
        gpa.free(entry.internal_sid);
        gpa.destroy(entry);
    }

    pub fn forward(self: *Worker, loop: *xev.Loop, bytes: []const u8) !void {
        if (self.closing) return;
        try self.out.appendSlice(self.mixer.gpa, bytes);
        self.maybeKickWrite(loop);
    }

    fn maybeKickWrite(self: *Worker, loop: *xev.Loop) void {
        if (self.closing or self.write_in_flight) return;
        if (self.out.items.len == 0) return;
        std.mem.swap(std.ArrayList(u8), &self.out, &self.in_flight);
        self.write_in_flight = true;
        self.upstream.write(loop, &self.write_completion, .{ .slice = self.in_flight.items }, Worker, self, onWrite);
    }

    fn onWrite(
        self_opt: ?*Worker,
        loop: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        buf: xev.WriteBuffer,
        r: xev.WriteError!usize,
    ) xev.CallbackAction {
        const self = self_opt.?;
        const written = r catch |err| {
            std.log.warn("mixer: worker shard={s} write err: {s}; exiting", .{ self.shard, @errorName(err) });
            self.write_in_flight = false;
            loop.stop();
            return .disarm;
        };
        const total = buf.slice.len;
        if (written < total) {
            self.upstream.write(loop, &self.write_completion, .{ .slice = buf.slice[written..] }, Worker, self, onWrite);
            return .disarm;
        }
        self.write_in_flight = false;
        self.in_flight.clearRetainingCapacity();
        self.maybeKickWrite(loop);
        return .disarm;
    }

    fn terminate(self: *Worker) void {
        if (self.pid > 0) {
            _ = kill(self.pid, SIGTERM);
        }
    }

    fn shutdown(self: *Worker) void {
        const gpa = self.mixer.gpa;
        self.closing = true;
        self.out.deinit(gpa);
        self.in_flight.deinit(gpa);
        self.rx.deinit(gpa);

        var it = self.filter_by_str.iterator();
        while (it.next()) |e| {
            const entry = e.value_ptr.*;
            for (entry.subscribers.items) |s| gpa.free(s.client_sid);
            entry.subscribers.deinit(gpa);
            gpa.free(entry.filter);
            gpa.free(entry.internal_sid);
            gpa.destroy(entry);
        }
        self.filter_by_str.deinit(gpa);
        self.entries_by_sid.deinit(gpa);

        if (self.fd >= 0) _ = std.c.close(self.fd);
    }
};

pub const ClientSub = struct {
    entry: *FilterEntry,
    worker: *Worker,
};

pub const ClientConn = struct {
    mixer: *Mixer,
    id: u64,
    tcp: xev.TCP,

    rx: std.ArrayList(u8) = .empty,
    read_completion: xev.Completion = undefined,
    read_buf: [16 * 1024]u8 = undefined,

    write_completion: xev.Completion = undefined,
    write_in_flight: bool = false,
    out: std.ArrayList(u8) = .empty,
    in_flight: std.ArrayList(u8) = .empty,
    kick_queued: bool = false,
    kick_next: ?*ClientConn = null,

    /// client_sid -> (worker, FilterEntry). Owns its keys.
    subs: std.StringHashMapUnmanaged(ClientSub) = .empty,

    close_completion: xev.Completion = undefined,
    closing: bool = false,

    fn init(mixer: *Mixer, tcp: xev.TCP) !*ClientConn {
        const self = try mixer.gpa.create(ClientConn);
        const id = mixer.next_conn_id;
        mixer.next_conn_id += 1;
        self.* = .{
            .mixer = mixer,
            .id = id,
            .tcp = tcp,
        };
        try proto.writeInfo(mixer.gpa, &self.out, &mixer_id_buf, id, "0.0.0.0", 0, .mixer);
        return self;
    }

    fn deinit(self: *ClientConn) void {
        const gpa = self.mixer.gpa;
        // Caller (beginClose) must have already drained subs via cleanupSubs;
        // here we just free the map's own bookkeeping.
        var it = self.subs.iterator();
        while (it.next()) |e| gpa.free(e.key_ptr.*);
        self.subs.deinit(gpa);
        self.rx.deinit(gpa);
        self.out.deinit(gpa);
        self.in_flight.deinit(gpa);
        gpa.destroy(self);
    }

    fn cleanupSubs(self: *ClientConn, loop: *xev.Loop) void {
        var it = self.subs.iterator();
        while (it.next()) |e| {
            const sid = e.key_ptr.*;
            const sub = e.value_ptr.*;
            sub.worker.removeSubscriber(loop, sub.entry, self, sid);
        }
    }

    fn start(self: *ClientConn, loop: *xev.Loop) void {
        self.maybeKickWrite(loop);
        self.tcp.read(loop, &self.read_completion, .{ .slice = &self.read_buf }, ClientConn, self, onRead);
    }

    fn onRead(
        self_opt: ?*ClientConn,
        loop: *xev.Loop,
        _: *xev.Completion,
        _: xev.TCP,
        buf: xev.ReadBuffer,
        r: xev.ReadError!usize,
    ) xev.CallbackAction {
        const self = self_opt.?;
        const n = r catch {
            self.beginClose(loop);
            return .disarm;
        };
        if (n == 0) {
            self.beginClose(loop);
            return .disarm;
        }
        self.rx.appendSlice(self.mixer.gpa, buf.slice[0..n]) catch {
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

    fn processRx(self: *ClientConn, loop: *xev.Loop) !void {
        const gpa = self.mixer.gpa;
        var cursor: usize = 0;
        while (cursor < self.rx.items.len) {
            const slice = self.rx.items[cursor..];
            const result = proto.parseClientOp(slice) catch |err| switch (err) {
                error.NeedMoreData => break,
                else => {
                    try proto.writeErr(gpa, &self.out, "Protocol Violation");
                    self.closing = true;
                    break;
                },
            };
            switch (result.op) {
                .ping => try proto.writePong(gpa, &self.out),
                .pong, .connect => {},
                .pub_msg => |p| {
                    const w = self.mixer.workerFor(p.subject);
                    try w.forward(loop, slice[0..result.consumed]);
                },
                .sub => |s| try self.handleSub(loop, s),
                .unsub => |u| try self.handleUnsub(loop, u),
            }
            cursor += result.consumed;
        }
        if (cursor > 0) {
            const rest = self.rx.items.len - cursor;
            std.mem.copyForwards(u8, self.rx.items[0..rest], self.rx.items[cursor..]);
            self.rx.items.len = rest;
        }
    }

    fn handleSub(self: *ClientConn, loop: *xev.Loop, s: proto.ClientOp.Sub) !void {
        const gpa = self.mixer.gpa;

        // Reject wildcard first token. Concrete first tokens map deterministically
        // to one worker; without that the mixer would have to fan out and dedup,
        // which we explicitly opted out of.
        const dot = std.mem.indexOfScalar(u8, s.subject, '.') orelse s.subject.len;
        const first = s.subject[0..dot];
        if (first.len == 0 or std.mem.indexOfAny(u8, first, "*>") != null) {
            try proto.writeErr(gpa, &self.out, "Wildcard first token not supported");
            return;
        }

        // Duplicate-sid on this client is a protocol error; the existing server
        // tolerates it but real NATS doesn't. Reject defensively.
        if (self.subs.contains(s.sid)) {
            try proto.writeErr(gpa, &self.out, "Duplicate sid");
            return;
        }

        const worker = self.mixer.workerFor(s.subject);
        const entry = try worker.addSubscriber(loop, self, s.subject, s.sid);
        var worker_registered = true;
        errdefer if (worker_registered) worker.removeSubscriber(loop, entry, self, s.sid);

        const sid_owned = try gpa.dupe(u8, s.sid);
        errdefer gpa.free(sid_owned);
        try self.subs.put(gpa, sid_owned, .{ .entry = entry, .worker = worker });
        worker_registered = false;
    }

    fn handleUnsub(self: *ClientConn, loop: *xev.Loop, u: proto.ClientOp.Unsub) !void {
        // max_msgs (auto-unsub-after-N) is not implemented: would need per-sub
        // counters on the mixer. Plain UNSUB is enough for v1.
        const gpa = self.mixer.gpa;
        const kv = self.subs.fetchRemove(u.sid) orelse return;
        defer gpa.free(kv.key);
        kv.value.worker.removeSubscriber(loop, kv.value.entry, self, kv.key);
    }

    fn maybeKickWrite(self: *ClientConn, loop: *xev.Loop) void {
        if (self.closing or self.write_in_flight) return;
        if (self.out.items.len == 0) return;
        std.mem.swap(std.ArrayList(u8), &self.out, &self.in_flight);
        self.write_in_flight = true;
        self.tcp.write(loop, &self.write_completion, .{ .slice = self.in_flight.items }, ClientConn, self, onWrite);
    }

    fn onWrite(
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
            self.beginClose(loop);
            return .disarm;
        };
        const total = buf.slice.len;
        if (written < total) {
            self.tcp.write(loop, &self.write_completion, .{ .slice = buf.slice[written..] }, ClientConn, self, onWrite);
            return .disarm;
        }
        self.write_in_flight = false;
        self.in_flight.clearRetainingCapacity();
        self.maybeKickWrite(loop);
        return .disarm;
    }

    fn beginClose(self: *ClientConn, loop: *xev.Loop) void {
        if (self.closing) return;
        std.log.info("mixer client {d} closed", .{self.id});
        self.closing = true;
        self.cleanupSubs(loop);
        self.tcp.close(loop, &self.close_completion, ClientConn, self, onClose);
    }

    fn onClose(
        self_opt: ?*ClientConn,
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

const KickQueue = struct {
    head: ?*ClientConn = null,
    tail: ?*ClientConn = null,

    fn push(self: *KickQueue, c: *ClientConn) void {
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

    fn flush(self: *KickQueue, loop: *xev.Loop) void {
        var cur = self.head;
        while (cur) |c| {
            const next = c.kick_next;
            c.kick_next = null;
            c.kick_queued = false;
            c.maybeKickWrite(loop);
            cur = next;
        }
        self.head = null;
        self.tail = null;
    }
};

fn parseListen(spec: []const u8) !std.Io.net.IpAddress {
    const prefix = "tcp://";
    if (!std.mem.startsWith(u8, spec, prefix)) return error.UnsupportedListenScheme;
    const rest = spec[prefix.len..];
    const colon = std.mem.lastIndexOfScalar(u8, rest, ':') orelse return error.MissingPort;
    const host = rest[0..colon];
    const port_str = rest[colon + 1 ..];
    const port = std.fmt.parseInt(u16, port_str, 10) catch return error.InvalidPort;
    if (std.mem.eql(u8, host, "0.0.0.0") or host.len == 0) {
        return .{ .ip4 = .unspecified(port) };
    }
    var bytes: [4]u8 = undefined;
    var it = std.mem.splitScalar(u8, host, '.');
    var i: usize = 0;
    while (it.next()) |part| : (i += 1) {
        if (i >= 4) return error.InvalidHost;
        bytes[i] = std.fmt.parseInt(u8, part, 10) catch return error.InvalidHost;
    }
    if (i != 4) return error.InvalidHost;
    return .{ .ip4 = .{ .bytes = bytes, .port = port } };
}

fn selfExe(buf: []u8) ![]const u8 {
    if (builtin.os.tag == .linux) {
        const rc = std.os.linux.readlink("/proc/self/exe", buf.ptr, buf.len);
        switch (std.posix.errno(rc)) {
            .SUCCESS => return buf[0..@as(usize, @bitCast(rc))],
            else => return error.ReadLinkFailed,
        }
    }
    if (builtin.os.tag == .macos) {
        const NS = struct {
            extern "c" fn _NSGetExecutablePath(buf: [*]u8, size: *u32) c_int;
        };
        var size: u32 = @intCast(buf.len);
        if (NS._NSGetExecutablePath(buf.ptr, &size) != 0) return error.PathTooLong;
        const len = std.mem.indexOfScalar(u8, buf, 0) orelse buf.len;
        return buf[0..len];
    }
    return error.UnsupportedOs;
}
