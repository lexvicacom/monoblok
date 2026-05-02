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
    /// Set before `removeAllFor`; checked by fan-out and `writeMsg` to
    /// short-circuit work on a dying conn. Plain bool: only the loop thread
    /// reads or writes it.
    closed: bool = false,
    /// Largest `out.items.len` observed on this conn between drains. The
    /// server reads this after each PUB dispatch to detect slow-consumer
    /// blow-ups, and resets it when it kicks a write.
    out_hwm: usize = 0,

    pub fn kick(c: *Conn) void {
        if (c.kick_fn) |f| if (c.kick_ctx) |ctx| f(ctx);
    }

    /// Free `out` and destroy `c`. Called once, by the server's close path.
    pub fn deinit(c: *Conn) void {
        c.out.deinit(c.gpa);
        c.gpa.destroy(c);
    }

    pub fn markClosed(c: *Conn) void {
        c.closed = true;
    }

    pub fn isClosed(c: *const Conn) bool {
        return c.closed;
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
    /// Slice into `filter` (non-LVC) or `filter[lvc_prefix.len..]` (LVC).
    /// Don't free separately.
    match_filter: []const u8,
    tokens: [][]const u8,
    is_lvc: bool,
    is_literal: bool,
    max_msgs: ?u64,
    delivered: u64,
};

pub const Router = struct {
    gpa: Allocator,
    /// Literal-filter subs, keyed by `match_filter`. Bucket owns its key
    /// (duped on first insert, freed when the bucket goes empty).
    literal_subs: std.StringHashMapUnmanaged(LiteralBucket) = .empty,
    /// Wildcard-filter subs (`*` or `>`). Linear scan per publish.
    wildcard_subs: std.ArrayListUnmanaged(*Subscription) = .empty,
    lvc_enabled: bool,
    last_value: std.StringHashMap(std.ArrayList(u8)),
    /// Reusable scratch for `publish` (kicks list, to_drop list, LVC subject
    /// formatting). Reset with retain_capacity per publish so the steady
    /// state allocates nothing.
    publish_arena: std.heap.ArenaAllocator,
    /// Optional export hook (the NATS bridge). Called once per publish after
    /// local fan-out; the bridge does its own subject-filter match.
    bridge_ctx: ?*anyopaque = null,
    bridge_fn: ?*const fn (ctx: *anyopaque, subject: []const u8, payload: []const u8) void = null,

    pub const LiteralBucket = struct {
        subs: std.ArrayListUnmanaged(*Subscription) = .empty,
    };

    pub fn init(gpa: Allocator, lvc_enabled: bool) Router {
        return .{
            .gpa = gpa,
            .lvc_enabled = lvc_enabled,
            .last_value = .init(gpa),
            .publish_arena = .init(gpa),
        };
    }

    pub fn deinit(self: *Router) void {
        self.publish_arena.deinit();
        var it = self.literal_subs.iterator();
        while (it.next()) |e| {
            for (e.value_ptr.subs.items) |s| self.freeSub(s);
            e.value_ptr.subs.deinit(self.gpa);
            self.gpa.free(e.key_ptr.*);
        }
        self.literal_subs.deinit(self.gpa);
        for (self.wildcard_subs.items) |s| self.freeSub(s);
        self.wildcard_subs.deinit(self.gpa);
        var lv_it = self.last_value.iterator();
        while (lv_it.next()) |e| {
            self.gpa.free(e.key_ptr.*);
            e.value_ptr.deinit(self.gpa);
        }
        self.last_value.deinit();
    }

    fn freeSub(self: *Router, s: *Subscription) void {
        self.gpa.free(s.sid);
        self.gpa.free(s.filter);
        self.gpa.free(s.tokens);
        self.gpa.destroy(s);
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
        const match_filter_unowned = if (is_lvc) filter[lvc_prefix.len..] else filter;
        if (is_lvc) {
            if (!self.lvc_enabled) return error.LvcDisabled;
            if (match_filter_unowned.len == 0) return error.EmptyToken;
            try subject_mod.validateFilter(match_filter_unowned);
        }

        const sid_owned = try self.gpa.dupe(u8, sid);
        errdefer self.gpa.free(sid_owned);
        const filter_owned = try self.gpa.dupe(u8, filter);
        errdefer self.gpa.free(filter_owned);
        const match_filter: []const u8 = if (is_lvc) filter_owned[lvc_prefix.len..] else filter_owned;
        const tokens = try splitTokens(self.gpa, match_filter);
        errdefer self.gpa.free(tokens);

        const sub = try self.gpa.create(Subscription);
        errdefer self.gpa.destroy(sub);
        const is_literal = rules_mod.isLiteralFilter(match_filter);
        sub.* = .{
            .conn = conn,
            .sid = sid_owned,
            .filter = filter_owned,
            .match_filter = match_filter,
            .tokens = tokens,
            .is_lvc = is_lvc,
            .is_literal = is_literal,
            .max_msgs = null,
            .delivered = 0,
        };

        if (is_literal) {
            const gop = try self.literal_subs.getOrPut(self.gpa, match_filter);
            if (!gop.found_existing) {
                const key_owned = self.gpa.dupe(u8, match_filter) catch |err| {
                    _ = self.literal_subs.remove(match_filter);
                    return err;
                };
                gop.key_ptr.* = key_owned;
                gop.value_ptr.* = .{};
            }
            gop.value_ptr.subs.append(self.gpa, sub) catch |err| {
                if (!gop.found_existing) {
                    self.gpa.free(gop.key_ptr.*);
                    _ = self.literal_subs.remove(match_filter);
                }
                return err;
            };
            errdefer {
                _ = gop.value_ptr.subs.pop();
                if (!gop.found_existing) {
                    gop.value_ptr.subs.deinit(self.gpa);
                    self.gpa.free(gop.key_ptr.*);
                    _ = self.literal_subs.remove(match_filter);
                }
            }
            if (is_lvc) try self.emitCached(conn, match_filter, sid);
        } else {
            try self.wildcard_subs.append(self.gpa, sub);
            errdefer _ = self.wildcard_subs.pop();
            if (is_lvc) try self.emitCached(conn, match_filter, sid);
        }

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

    /// Remove `sub` from its dispatch bucket and free it. Drops the literal
    /// bucket (and its owned key) when it goes empty.
    fn dropSub(self: *Router, sub: *Subscription) void {
        if (sub.is_literal) {
            if (self.literal_subs.getEntry(sub.match_filter)) |entry| {
                const bucket = entry.value_ptr;
                var i: usize = 0;
                while (i < bucket.subs.items.len) : (i += 1) {
                    if (bucket.subs.items[i] == sub) {
                        _ = bucket.subs.swapRemove(i);
                        break;
                    }
                }
                if (bucket.subs.items.len == 0) {
                    bucket.subs.deinit(self.gpa);
                    const key = entry.key_ptr.*;
                    _ = self.literal_subs.remove(key);
                    self.gpa.free(key);
                }
            }
        } else {
            var i: usize = 0;
            while (i < self.wildcard_subs.items.len) : (i += 1) {
                if (self.wildcard_subs.items[i] == sub) {
                    _ = self.wildcard_subs.swapRemove(i);
                    break;
                }
            }
        }
        self.freeSub(sub);
    }

    pub fn unsubscribe(self: *Router, conn: *Conn, sid: []const u8, max_msgs: ?u64) !void {
        try self.scanLiteralAndPrune(scanModeUnsub(conn, sid, max_msgs));

        var i: usize = 0;
        while (i < self.wildcard_subs.items.len) {
            const s = self.wildcard_subs.items[i];
            if (s.conn == conn and std.mem.eql(u8, s.sid, sid)) {
                if (max_msgs) |m| {
                    if (s.delivered >= m) {
                        _ = self.wildcard_subs.swapRemove(i);
                        self.freeSub(s);
                        continue;
                    }
                    s.max_msgs = m;
                } else {
                    _ = self.wildcard_subs.swapRemove(i);
                    self.freeSub(s);
                    continue;
                }
            }
            i += 1;
        }
    }

    pub fn removeAllFor(self: *Router, conn: *Conn) void {
        // Best-effort: a scratch-alloc failure leaves stale subs, but the
        // conn is going away anyway.
        self.scanLiteralAndPrune(scanModeRemoveAll(conn)) catch {};

        var i: usize = 0;
        while (i < self.wildcard_subs.items.len) {
            const s = self.wildcard_subs.items[i];
            if (s.conn == conn) {
                _ = self.wildcard_subs.swapRemove(i);
                self.freeSub(s);
            } else i += 1;
        }
    }

    const ScanAction = enum { keep, drop, set_max };
    const ScanMode = struct {
        ctx: union(enum) {
            unsub: struct { conn: *Conn, sid: []const u8, max_msgs: ?u64 },
            remove_all: struct { conn: *Conn },
        },

        fn classify(m: ScanMode, s: *Subscription) struct { action: ScanAction, max_msgs: u64 } {
            switch (m.ctx) {
                .unsub => |u| {
                    if (s.conn != u.conn or !std.mem.eql(u8, s.sid, u.sid)) return .{ .action = .keep, .max_msgs = 0 };
                    if (u.max_msgs) |m2| {
                        if (s.delivered >= m2) return .{ .action = .drop, .max_msgs = 0 };
                        return .{ .action = .set_max, .max_msgs = m2 };
                    }
                    return .{ .action = .drop, .max_msgs = 0 };
                },
                .remove_all => |r| {
                    if (s.conn == r.conn) return .{ .action = .drop, .max_msgs = 0 };
                    return .{ .action = .keep, .max_msgs = 0 };
                },
            }
        }
    };

    fn scanModeUnsub(conn: *Conn, sid: []const u8, max_msgs: ?u64) ScanMode {
        return .{ .ctx = .{ .unsub = .{ .conn = conn, .sid = sid, .max_msgs = max_msgs } } };
    }

    fn scanModeRemoveAll(conn: *Conn) ScanMode {
        return .{ .ctx = .{ .remove_all = .{ .conn = conn } } };
    }

    /// Walk `literal_subs`, apply `mode`, drop empty buckets. Two-phase
    /// (collect, then remove) so we don't mutate mid-iteration.
    fn scanLiteralAndPrune(self: *Router, mode: ScanMode) !void {
        var arena_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer arena_state.deinit();
        const arena = arena_state.allocator();

        var empties: std.ArrayListUnmanaged([]const u8) = .empty;
        var lit_it = self.literal_subs.iterator();
        while (lit_it.next()) |e| {
            var i: usize = 0;
            while (i < e.value_ptr.subs.items.len) {
                const s = e.value_ptr.subs.items[i];
                const r = mode.classify(s);
                switch (r.action) {
                    .keep => i += 1,
                    .set_max => {
                        s.max_msgs = r.max_msgs;
                        i += 1;
                    },
                    .drop => {
                        _ = e.value_ptr.subs.swapRemove(i);
                        self.freeSub(s);
                    },
                }
            }
            if (e.value_ptr.subs.items.len == 0) {
                try empties.append(arena, e.key_ptr.*);
            }
        }
        for (empties.items) |key| {
            if (self.literal_subs.fetchRemove(key)) |kv| {
                var bucket_copy = kv.value;
                bucket_copy.subs.deinit(self.gpa);
                self.gpa.free(kv.key);
            }
        }
    }

    /// Fan-out a publish to matching subscribers.
    pub fn publish(self: *Router, subject: []const u8, payload: []const u8) !void {
        // $STATS.* is excluded from the LVC (tick-driven, stale by design).
        if (self.lvc_enabled and !std.mem.startsWith(u8, subject, stats_prefix)) {
            try self.storeLast(subject, payload);
        }

        const have_literal = self.literal_subs.count() != 0;
        const have_wildcard = self.wildcard_subs.items.len != 0;
        if (!have_literal and !have_wildcard) {
            if (self.bridge_fn) |f| if (self.bridge_ctx) |ctx| f(ctx, subject, payload);
            return;
        }

        defer _ = self.publish_arena.reset(.retain_capacity);
        const scratch = self.publish_arena.allocator();

        var kicks: std.ArrayListUnmanaged(*Conn) = .empty;
        // Subs that hit max_msgs or whose conn closed mid-fanout. Removed
        // after iteration so we don't mutate the bucket we're walking.
        var to_drop: std.ArrayListUnmanaged(*Subscription) = .empty;

        if (have_literal) {
            if (self.literal_subs.getPtr(subject)) |bucket| {
                for (bucket.subs.items) |s| {
                    try self.deliverOne(s, subject, payload, scratch, &kicks, &to_drop);
                }
            }
        }

        if (have_wildcard) {
            var sub_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
            const sub_tokens = splitInto(subject, &sub_tokens_buf);
            for (self.wildcard_subs.items) |s| {
                if (!subject_mod.matchesTokens(s.tokens, sub_tokens)) continue;
                try self.deliverOne(s, subject, payload, scratch, &kicks, &to_drop);
            }
        }

        for (to_drop.items) |s| self.dropSub(s);

        // Dedup kicks: each conn kicked at most once per publish. Sort+dedup
        // is O(n log n) and worth the work for fan-out workloads, but with 0
        // or 1 kick (the common 1-sub case) we skip it entirely.
        if (kicks.items.len <= 1) {
            if (kicks.items.len == 1) kicks.items[0].kick();
        } else {
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
        }

        // Bridge fires after local fan-out so a blocked remote can't starve
        // local subscribers.
        if (self.bridge_fn) |f| if (self.bridge_ctx) |ctx| f(ctx, subject, payload);
    }

    /// Append MSG bytes to the conn's outbound buffer, record a kick, queue
    /// the sub for removal on close / max_msgs. Per-sub writeMsg failures
    /// are silently dropped.
    fn deliverOne(
        self: *Router,
        s: *Subscription,
        subject: []const u8,
        payload: []const u8,
        scratch: Allocator,
        kicks: *std.ArrayListUnmanaged(*Conn),
        to_drop: *std.ArrayListUnmanaged(*Subscription),
    ) !void {
        _ = self;
        if (s.conn.isClosed()) {
            try to_drop.append(scratch, s);
            return;
        }
        if (s.is_lvc) {
            const out_subject = std.fmt.allocPrint(scratch, "{s}{s}", .{ lvc_prefix, subject }) catch return;
            s.conn.writeMsg(out_subject, s.sid, null, payload) catch {};
        } else {
            s.conn.writeMsg(subject, s.sid, null, payload) catch {};
        }
        try kicks.append(scratch, s.conn);
        s.delivered += 1;
        if (s.max_msgs != null and s.delivered >= s.max_msgs.?) {
            try to_drop.append(scratch, s);
        }
    }

    /// Public wrapper so `snapshot.zig` can populate the LVC at load time.
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

// --- Tests --------------------------------------------------------------

const testing = std.testing;

fn makeConn(gpa: Allocator, id: u64) !*Conn {
    const c = try gpa.create(Conn);
    c.* = .{ .id = id, .gpa = gpa };
    return c;
}

fn deliveredMsgCount(c: *Conn) usize {
    var n: usize = 0;
    var i: usize = 0;
    while (i < c.out.items.len) : (i += 1) {
        if (c.out.items[i] == '\n') n += 1;
    }
    // Each MSG occupies two lines (header + payload).
    return n / 2;
}

test "router: literal subs go in the literal bucket, wildcards in wildcard list" {
    var router: Router = .init(testing.allocator, true);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.bar", "1");
    try router.subscribe(a, "foo.*", "2");
    try router.subscribe(a, "baz.>", "3");
    try router.subscribe(a, "qux.zot", "4");

    try testing.expectEqual(@as(usize, 2), router.literal_subs.count());
    try testing.expectEqual(@as(usize, 2), router.wildcard_subs.items.len);
    try testing.expect(router.literal_subs.contains("foo.bar"));
    try testing.expect(router.literal_subs.contains("qux.zot"));
}

test "router: literal-only publish does not touch wildcard subs" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.bar", "1");
    try router.subscribe(b, "other.>", "2");

    try router.publish("foo.bar", "hi");

    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 0), deliveredMsgCount(b));
}

test "router: wildcard sub matches via wildcard path" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.*", "1");

    try router.publish("foo.bar", "x");
    try router.publish("foo.baz", "y");
    try router.publish("nope.bar", "z");

    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(a));
}

test "router: max_msgs auto-unsub drops sub from literal bucket" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.bar", "1");
    try router.unsubscribe(a, "1", 2);

    try router.publish("foo.bar", "x");
    try router.publish("foo.bar", "y");
    try router.publish("foo.bar", "z");

    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(a));
    // Bucket should be cleaned up after the second delivery removed the sub.
    try testing.expectEqual(@as(usize, 0), router.literal_subs.count());
}

test "router: max_msgs auto-unsub drops wildcard sub" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.*", "1");
    try router.unsubscribe(a, "1", 1);

    try router.publish("foo.bar", "x");
    try router.publish("foo.baz", "y");

    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 0), router.wildcard_subs.items.len);
}

test "router: removeAllFor cleans up across both paths" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.bar", "1");
    try router.subscribe(a, "foo.*", "2");
    try router.subscribe(b, "foo.bar", "3");
    try router.subscribe(b, "wild.>", "4");

    a.markClosed();
    router.removeAllFor(a);
    a.deinit();

    try testing.expectEqual(@as(usize, 1), router.literal_subs.count());
    try testing.expectEqual(@as(usize, 1), router.wildcard_subs.items.len);

    try router.publish("foo.bar", "x");
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(b));
}

test "router: empty bucket is reclaimed when last sub leaves" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.bar", "1");
    try testing.expectEqual(@as(usize, 1), router.literal_subs.count());

    try router.unsubscribe(a, "1", null);
    try testing.expectEqual(@as(usize, 0), router.literal_subs.count());
}

test "router: bucket key survives owner sub leaving while others remain" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.bar", "1");
    try router.subscribe(b, "foo.bar", "2");

    // Drop the first sub, which contributed the bucket key initially.
    try router.unsubscribe(a, "1", null);

    // Bucket should still exist and still match publishes.
    try testing.expectEqual(@as(usize, 1), router.literal_subs.count());
    try router.publish("foo.bar", "x");
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(b));
}
