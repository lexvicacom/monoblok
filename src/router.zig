const std = @import("std");
const Allocator = std.mem.Allocator;

const proto = @import("proto.zig");
const patchbay = @import("patchbay");
const subject_mod = patchbay.subject;
const rules_mod = patchbay.eval;

/// One connection's router-facing state. The server owns the TCP socket and
/// the read/write machinery; the router only needs to append bytes to the
/// outbound buffer and ask the server to kick a write.
pub const Conn = struct {
    id: u64,
    gpa: Allocator,
    /// Outbound bytes pending write to the client. Appended by writeMsg /
    /// writePong / writeErr; drained by the server's write loop.
    out: std.ArrayList(u8) = .empty,
    /// Opaque kick callback: set by the server to notify when `out` has new
    /// bytes that need to be written. Single-threaded, no locking needed.
    kick_ctx: ?*anyopaque = null,
    kick_fn: ?*const fn (ctx: *anyopaque) void = null,
    /// Refcount for lifetime across fan-out. Starts at 1; fan-out does
    /// not retain in the current single-threaded model (fan-out finishes
    /// before the loop returns to the handler that might close the
    /// conn), but `refs` is atomic so that a future sharded / worker
    /// model can retain across thread boundaries without a refactor.
    refs: std.atomic.Value(u32) = .init(1),
    /// Set by the owning loop before `removeAllFor`; read by fan-out and
    /// `writeMsg` to short-circuit work on a dying conn. Atomic so the
    /// close signal is visible across threads if fan-out ever moves off
    /// the loop thread.
    closed: std.atomic.Value(bool) = .init(false),
    /// Largest `out.items.len` observed on this conn between drains. The
    /// server reads this after each PUB dispatch to detect slow-consumer
    /// blow-ups, and resets it when it kicks a write.
    out_hwm: usize = 0,

    pub fn kick(c: *Conn) void {
        if (c.kick_fn) |f| if (c.kick_ctx) |ctx| f(ctx);
    }

    pub fn retain(c: *Conn) void {
        _ = c.refs.fetchAdd(1, .monotonic);
    }

    pub fn release(c: *Conn) void {
        // Release-before / acquire-on-last-drop pairs the writer's stores
        // on the last release with the reader's dealloc — standard refcount
        // fence idiom so non-atomic fields touched by other threads
        // happen-before the `destroy`.
        if (c.refs.fetchSub(1, .release) == 1) {
            _ = c.refs.load(.acquire);
            c.out.deinit(c.gpa);
            c.gpa.destroy(c);
        }
    }

    pub fn markClosed(c: *Conn) void {
        c.closed.store(true, .release);
    }

    pub fn isClosed(c: *const Conn) bool {
        return c.closed.load(.acquire);
    }

    pub fn writeMsg(
        c: *Conn,
        subject: []const u8,
        sid: []const u8,
        reply: ?[]const u8,
        payload: []const u8,
    ) !void {
        if (c.isClosed()) return;
        try proto.writeMsg(c.gpa, &c.out, subject, sid, reply, payload);
        if (c.out.items.len > c.out_hwm) c.out_hwm = c.out.items.len;
    }
};

const Subscription = struct {
    conn: *Conn,
    sid: []u8,
    filter: []u8,
    tokens: [][]const u8,
    is_lvc: bool,
    max_msgs: ?u64,
    delivered: u64,
};

pub const Router = struct {
    gpa: Allocator,
    subs: std.ArrayList(Subscription) = .empty,
    lvc_enabled: bool,
    last_value: std.StringHashMap(std.ArrayList(u8)),
    /// Optional export hook (the NATS bridge). Called once per publish after
    /// normal fan-out. The bridge decides internally whether the subject
    /// matches any export filter; router doesn't care.
    bridge_ctx: ?*anyopaque = null,
    bridge_fn: ?*const fn (ctx: *anyopaque, subject: []const u8, payload: []const u8) void = null,

    pub fn init(gpa: Allocator, lvc_enabled: bool) Router {
        return .{
            .gpa = gpa,
            .lvc_enabled = lvc_enabled,
            .last_value = .init(gpa),
        };
    }

    pub fn deinit(self: *Router) void {
        for (self.subs.items) |*s| self.freeSub(s);
        self.subs.deinit(self.gpa);
        var it = self.last_value.iterator();
        while (it.next()) |e| {
            self.gpa.free(e.key_ptr.*);
            e.value_ptr.deinit(self.gpa);
        }
        self.last_value.deinit();
    }

    fn freeSub(self: *Router, s: *Subscription) void {
        self.gpa.free(s.sid);
        self.gpa.free(s.filter);
        self.gpa.free(s.tokens);
    }

    pub const lvc_prefix = "$LVC.";
    pub const stats_prefix = "$STATS.";

    pub fn subscribe(
        self: *Router,
        conn: *Conn,
        filter: []const u8,
        sid: []const u8,
    ) !void {
        try subject_mod.validateFilter(filter);

        const is_lvc = std.mem.startsWith(u8, filter, lvc_prefix);
        const match_filter = if (is_lvc) filter[lvc_prefix.len..] else filter;
        if (is_lvc) {
            if (!self.lvc_enabled) return error.LvcDisabled;
            if (match_filter.len == 0) return error.EmptyToken;
            try subject_mod.validateFilter(match_filter);
        }

        const sid_owned = try self.gpa.dupe(u8, sid);
        errdefer self.gpa.free(sid_owned);
        const filter_owned = try self.gpa.dupe(u8, filter);
        errdefer self.gpa.free(filter_owned);
        const tokens = try splitTokens(self.gpa, if (is_lvc) filter_owned[lvc_prefix.len..] else filter_owned);
        errdefer self.gpa.free(tokens);

        try self.subs.append(self.gpa, .{
            .conn = conn,
            .sid = sid_owned,
            .filter = filter_owned,
            .tokens = tokens,
            .is_lvc = is_lvc,
            .max_msgs = null,
            .delivered = 0,
        });
        // If anything after the append fails (e.g. emitCached OOM), roll the
        // subscription back so the client doesn't end up silently subscribed
        // to something after we reported failure. swapRemove is O(1) and
        // safe on the just-appended tail.
        errdefer _ = self.subs.pop();

        if (is_lvc) try self.emitCached(conn, match_filter, sid);

        conn.kick();
    }

    fn emitCached(self: *Router, conn: *Conn, match_filter: []const u8, sid: []const u8) !void {
        var filter_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
        const filter_tokens = splitInto(match_filter, &filter_tokens_buf);

        var scratch_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer scratch_state.deinit();
        const scratch = scratch_state.allocator();

        var it = self.last_value.iterator();
        while (it.next()) |e| {
            const cached_subject = e.key_ptr.*;
            var cached_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
            const cached_tokens = splitInto(cached_subject, &cached_tokens_buf);
            if (!subject_mod.matchesTokens(filter_tokens, cached_tokens)) continue;

            const out_subject = try std.fmt.allocPrint(scratch, "{s}{s}", .{ lvc_prefix, cached_subject });
            conn.writeMsg(out_subject, sid, null, e.value_ptr.items) catch {};
        }
    }

    pub fn unsubscribe(self: *Router, conn: *Conn, sid: []const u8, max_msgs: ?u64) !void {
        var i: usize = 0;
        while (i < self.subs.items.len) {
            var s = &self.subs.items[i];
            if (s.conn == conn and std.mem.eql(u8, s.sid, sid)) {
                if (max_msgs) |m| {
                    if (s.delivered >= m) {
                        self.freeSub(s);
                        _ = self.subs.swapRemove(i);
                        continue;
                    }
                    s.max_msgs = m;
                } else {
                    self.freeSub(s);
                    _ = self.subs.swapRemove(i);
                    continue;
                }
            }
            i += 1;
        }
    }

    pub fn removeAllFor(self: *Router, conn: *Conn) void {
        var i: usize = 0;
        while (i < self.subs.items.len) {
            if (self.subs.items[i].conn == conn) {
                self.freeSub(&self.subs.items[i]);
                _ = self.subs.swapRemove(i);
            } else i += 1;
        }
    }

    /// Fan-out a publish to matching subscribers. Single-threaded: we iterate
    /// and append to each conn's outbound buffer directly, then kick each.
    pub fn publish(self: *Router, subject: []const u8, payload: []const u8) !void {
        var sub_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
        const sub_tokens = splitInto(subject, &sub_tokens_buf);

        // $STATS.* is excluded from the LVC: it's a periodic system stream
        // (one burst every stats_tick_ms), caching it only bloats the cache
        // and pins a snapshot that's stale the moment the next tick fires.
        if (self.lvc_enabled and !std.mem.startsWith(u8, subject, stats_prefix)) {
            try self.storeLast(subject, payload);
        }

        // Scratch for the LVC-subject prefixing; freed in one shot.
        var scratch_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer scratch_state.deinit();
        const scratch = scratch_state.allocator();

        // Track which conns we appended to so we can kick them once each.
        var kicks: std.ArrayList(*Conn) = .empty;
        defer kicks.deinit(scratch);

        var i: usize = 0;
        while (i < self.subs.items.len) {
            var s = &self.subs.items[i];
            if (s.conn.isClosed()) {
                self.freeSub(s);
                _ = self.subs.swapRemove(i);
                continue;
            }
            if (subject_mod.matchesTokens(s.tokens, sub_tokens)) {
                if (s.is_lvc) {
                    const out_subject = std.fmt.allocPrint(scratch, "{s}{s}", .{ lvc_prefix, subject }) catch {
                        i += 1;
                        continue;
                    };
                    s.conn.writeMsg(out_subject, s.sid, null, payload) catch {};
                } else {
                    s.conn.writeMsg(subject, s.sid, null, payload) catch {};
                }
                kicks.append(scratch, s.conn) catch {};
                s.delivered += 1;
                if (s.max_msgs != null and s.delivered >= s.max_msgs.?) {
                    self.freeSub(s);
                    _ = self.subs.swapRemove(i);
                    continue;
                }
            }
            i += 1;
        }

        // Dedup kicks: each conn kicked at most once per publish.
        std.mem.sort(*Conn, kicks.items, {}, struct {
            fn lt(_: void, a: *Conn, b: *Conn) bool {
                return @intFromPtr(a) < @intFromPtr(b);
            }
        }.lt);
        var last: ?*Conn = null;
        for (kicks.items) |c| {
            if (last == c) continue;
            c.kick();
            last = c;
        }

        // Bridge: export-side fan-out. Bridge filters are checked inside the
        // hook; we call it once per publish regardless of what matched
        // locally. Kept after local fan-out so a blocked remote can't
        // starve local subscribers.
        if (self.bridge_fn) |f| if (self.bridge_ctx) |ctx| f(ctx, subject, payload);
    }

    /// Public wrapper for snapshot loaders. Same semantics as the internal
    /// publish-path call; exposed so `snapshot.zig` can populate the cache
    /// without duplicating the dup+append logic.
    pub fn storeLastPublic(self: *Router, subject: []const u8, payload: []const u8) !void {
        return self.storeLast(subject, payload);
    }

    fn storeLast(self: *Router, subject: []const u8, payload: []const u8) !void {
        const gop = try self.last_value.getOrPut(subject);
        if (!gop.found_existing) {
            gop.key_ptr.* = try self.gpa.dupe(u8, subject);
            gop.value_ptr.* = .empty;
        }
        const list = gop.value_ptr;
        list.clearRetainingCapacity();
        try list.appendSlice(self.gpa, payload);
    }
};

fn splitTokens(gpa: Allocator, s: []const u8) ![][]const u8 {
    var count: usize = 1;
    for (s) |c| if (c == '.') {
        count += 1;
    };
    const out = try gpa.alloc([]const u8, count);
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |tok| : (i += 1) out[i] = tok;
    return out;
}

fn splitInto(s: []const u8, buf: *[subject_mod.max_tokens][]const u8) [][]const u8 {
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |tok| : (i += 1) {
        if (i >= buf.len) break;
        buf[i] = tok;
    }
    return buf[0..i];
}

// --- Publisher adapter for rules ----------------------------------------

pub fn rulesPublisher(r: *Router) rules_mod.Publisher {
    return .{
        .ctx = r,
        .publish_fn = rulesPublish,
    };
}

fn rulesPublish(ctx: *anyopaque, subj: []const u8, payload: []const u8) anyerror!void {
    const r: *Router = @ptrCast(@alignCast(ctx));
    try r.publish(subj, payload);
}
