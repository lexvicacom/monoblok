const std = @import("std");
const Allocator = std.mem.Allocator;

const proto = @import("proto.zig");
const patchbay = @import("patchbay");
const subject_mod = patchbay.subject;
const rules_mod = patchbay.eval;
const lvc_mod = @import("router/lvc.zig");

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
        headers: ?[]const u8,
        payload: []const u8,
    ) !void {
        if (c.isClosed()) return;
        if (headers) |h| {
            try proto.writeHmsg(c.gpa, &c.out, subject, sid, reply, h, payload);
        } else {
            try proto.writeMsg(c.gpa, &c.out, subject, sid, reply, payload);
        }
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
    /// Queue group name (NATS QGROUP). When set, this sub competes with other
    /// subs sharing the same `(match_filter, queue_group)` for each matching
    /// publish: exactly one of them is picked round-robin per message. When
    /// null, the sub gets every matching message (the default fan-out).
    queue_group: ?[]u8,
};

pub const Router = struct {
    gpa: Allocator,
    /// Literal-filter subs, keyed by `match_filter`. Bucket owns its key
    /// (duped on first insert, freed when the bucket goes empty).
    literal_subs: std.StringHashMapUnmanaged(LiteralBucket) = .empty,
    /// Wildcard-filter subs whose first token is a literal (`foo.*`,
    /// `T.*.p`), bucketed by that first token so a publish only scans
    /// subs whose first token matches the subject's. Bucket owns its key.
    wildcard_buckets: std.StringHashMapUnmanaged(WildcardBucket) = .empty,
    /// Wildcard-filter subs whose first token is itself a wildcard
    /// (`*.foo`, `>`). These have to be considered for every publish, so
    /// they stay in a single linear list.
    wildcard_global: std.ArrayListUnmanaged(*Subscription) = .empty,
    lvc: lvc_mod.Store,
    /// Reusable scratch for `publish` (kicks list, to_drop list, LVC subject
    /// formatting). Reset with retain_capacity per publish so the steady
    /// state allocates nothing.
    publish_arena: std.heap.ArenaAllocator,
    /// Optional export hook (the NATS bridge). Called once per publish after
    /// local fan-out; the bridge does its own subject-filter match.
    bridge_ctx: ?*anyopaque = null,
    bridge_fn: ?*const fn (ctx: *anyopaque, subject: []const u8, payload: []const u8) void = null,
    /// Round-robin cursors for queue groups. Keyed by "<match_filter>\x00<group>"
    /// (NUL separator avoids collisions when a filter or group name contains a
    /// dot). Owned keys; lazily inserted on first publish to a group.
    qg_rr: std.StringHashMapUnmanaged(u64) = .empty,

    pub const LiteralBucket = struct {
        subs: std.ArrayListUnmanaged(*Subscription) = .empty,
    };

    pub const WildcardBucket = struct {
        subs: std.ArrayListUnmanaged(*Subscription) = .empty,
    };

    pub const LvcFilter = lvc_mod.Filter;

    /// Returns the first dot-separated token of `s`, or all of `s` if no dot.
    fn firstToken(s: []const u8) []const u8 {
        const i = std.mem.indexOfScalar(u8, s, '.') orelse return s;
        return s[0..i];
    }

    /// True if `tok` is a wildcard token (`*` or `>`).
    fn isWildcardToken(tok: []const u8) bool {
        return tok.len == 1 and (tok[0] == '*' or tok[0] == '>');
    }

    pub fn init(gpa: Allocator, lvc_enabled: bool) Router {
        return .{
            .gpa = gpa,
            .lvc = .init(gpa, lvc_enabled),
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
        var wb_it = self.wildcard_buckets.iterator();
        while (wb_it.next()) |e| {
            for (e.value_ptr.subs.items) |s| self.freeSub(s);
            e.value_ptr.subs.deinit(self.gpa);
            self.gpa.free(e.key_ptr.*);
        }
        self.wildcard_buckets.deinit(self.gpa);
        for (self.wildcard_global.items) |s| self.freeSub(s);
        self.wildcard_global.deinit(self.gpa);
        var qg_it = self.qg_rr.iterator();
        while (qg_it.next()) |e| self.gpa.free(e.key_ptr.*);
        self.qg_rr.deinit(self.gpa);
        self.lvc.deinit();
    }

    pub fn configureLvc(self: *Router, filters: []const LvcFilter) !void {
        try self.lvc.configure(filters);
    }

    fn freeSub(self: *Router, s: *Subscription) void {
        self.gpa.free(s.sid);
        self.gpa.free(s.filter);
        self.gpa.free(s.tokens);
        if (s.queue_group) |qg| self.gpa.free(qg);
        self.gpa.destroy(s);
    }

    pub const lvc_prefix = lvc_mod.prefix;
    pub const stats_prefix = "$STATS.";

    pub fn subscribe(
        self: *Router,
        conn: *Conn,
        filter: []const u8,
        sid: []const u8,
        queue_group: ?[]const u8,
    ) !void {
        try subject_mod.validateFilter(filter);

        const is_lvc = std.mem.startsWith(u8, filter, lvc_prefix);
        const match_filter_unowned = if (is_lvc) filter[lvc_prefix.len..] else filter;
        if (is_lvc) {
            if (!self.lvc.enabled) return error.LvcDisabled;
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
        const qg_owned: ?[]u8 = if (queue_group) |qg| try self.gpa.dupe(u8, qg) else null;
        errdefer if (qg_owned) |qg| self.gpa.free(qg);

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
            .queue_group = qg_owned,
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
            const first_tok = firstToken(match_filter);
            if (isWildcardToken(first_tok)) {
                try self.wildcard_global.append(self.gpa, sub);
                errdefer _ = self.wildcard_global.pop();
                if (is_lvc) try self.emitCached(conn, match_filter, sid);
            } else {
                const gop = try self.wildcard_buckets.getOrPut(self.gpa, first_tok);
                if (!gop.found_existing) {
                    const key_owned = self.gpa.dupe(u8, first_tok) catch |err| {
                        _ = self.wildcard_buckets.remove(first_tok);
                        return err;
                    };
                    gop.key_ptr.* = key_owned;
                    gop.value_ptr.* = .{};
                }
                gop.value_ptr.subs.append(self.gpa, sub) catch |err| {
                    if (!gop.found_existing) {
                        self.gpa.free(gop.key_ptr.*);
                        _ = self.wildcard_buckets.remove(first_tok);
                    }
                    return err;
                };
                errdefer {
                    _ = gop.value_ptr.subs.pop();
                    if (!gop.found_existing) {
                        gop.value_ptr.subs.deinit(self.gpa);
                        self.gpa.free(gop.key_ptr.*);
                        _ = self.wildcard_buckets.remove(first_tok);
                    }
                }
                if (is_lvc) try self.emitCached(conn, match_filter, sid);
            }
        }

        conn.kick();
    }

    fn emitCached(self: *Router, conn: *Conn, match_filter: []const u8, sid: []const u8) !void {
        var filter_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
        const filter_tokens = splitInto(match_filter, &filter_tokens_buf);

        var scratch_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer scratch_state.deinit();
        const scratch = scratch_state.allocator();

        var it = self.lvc.last_value.iterator();
        while (it.next()) |e| {
            const cached_subject = e.key_ptr.*;
            var cached_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
            const cached_tokens = splitInto(cached_subject, &cached_tokens_buf);
            if (!subject_mod.matchesTokens(filter_tokens, cached_tokens)) continue;

            const out_subject = try std.fmt.allocPrint(scratch, "{s}{s}", .{ lvc_prefix, cached_subject });
            conn.writeMsg(out_subject, sid, null, null, e.value_ptr.items) catch {};
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
            const first_tok = firstToken(sub.match_filter);
            if (isWildcardToken(first_tok)) {
                var i: usize = 0;
                while (i < self.wildcard_global.items.len) : (i += 1) {
                    if (self.wildcard_global.items[i] == sub) {
                        _ = self.wildcard_global.swapRemove(i);
                        break;
                    }
                }
            } else if (self.wildcard_buckets.getEntry(first_tok)) |entry| {
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
                    _ = self.wildcard_buckets.remove(key);
                    self.gpa.free(key);
                }
            }
        }
        self.freeSub(sub);
    }

    pub fn unsubscribe(self: *Router, conn: *Conn, sid: []const u8, max_msgs: ?u64) !void {
        const mode = scanModeUnsub(conn, sid, max_msgs);
        try self.scanLiteralAndPrune(mode);
        try self.scanWildcardBucketsAndPrune(mode);
        self.scanGlobalList(&self.wildcard_global, mode);
    }

    pub fn removeAllFor(self: *Router, conn: *Conn) void {
        const mode = scanModeRemoveAll(conn);
        // Close cleanup must not allocate: stale subscriptions contain raw
        // Conn pointers, so leaving them behind would make later fan-out touch
        // freed connection state. Prune empty buckets with a non-allocating
        // repeated scan after the stale subscriptions are gone.
        self.scanLiteralNoPrune(mode);
        self.scanWildcardBucketsNoPrune(mode);
        self.scanGlobalList(&self.wildcard_global, mode);
        self.pruneEmptyLiteralBuckets();
        self.pruneEmptyWildcardBuckets();
    }

    /// Apply `mode` to every sub in a flat list. No bucket bookkeeping.
    fn scanGlobalList(self: *Router, list: *std.ArrayListUnmanaged(*Subscription), mode: ScanMode) void {
        var i: usize = 0;
        while (i < list.items.len) {
            const s = list.items[i];
            const r = mode.classify(s);
            switch (r.action) {
                .keep => i += 1,
                .set_max => {
                    s.max_msgs = r.max_msgs;
                    i += 1;
                },
                .drop => {
                    _ = list.swapRemove(i);
                    self.freeSub(s);
                },
            }
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

    fn scanLiteralNoPrune(self: *Router, mode: ScanMode) void {
        var lit_it = self.literal_subs.iterator();
        while (lit_it.next()) |e| {
            scanBucketNoPrune(self, &e.value_ptr.subs, mode);
        }
    }

    fn pruneEmptyLiteralBuckets(self: *Router) void {
        while (true) {
            var empty_key: ?[]const u8 = null;
            var it = self.literal_subs.iterator();
            while (it.next()) |e| {
                if (e.value_ptr.subs.items.len == 0) {
                    empty_key = e.key_ptr.*;
                    break;
                }
            }
            const key = empty_key orelse return;
            if (self.literal_subs.fetchRemove(key)) |kv| {
                var bucket_copy = kv.value;
                bucket_copy.subs.deinit(self.gpa);
                self.gpa.free(kv.key);
            }
        }
    }

    /// Mirror of `scanLiteralAndPrune` for the first-token-bucketed wildcard map.
    fn scanWildcardBucketsAndPrune(self: *Router, mode: ScanMode) !void {
        var arena_state: std.heap.ArenaAllocator = .init(self.gpa);
        defer arena_state.deinit();
        const arena = arena_state.allocator();

        var empties: std.ArrayListUnmanaged([]const u8) = .empty;
        var wb_it = self.wildcard_buckets.iterator();
        while (wb_it.next()) |e| {
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
            if (self.wildcard_buckets.fetchRemove(key)) |kv| {
                var bucket_copy = kv.value;
                bucket_copy.subs.deinit(self.gpa);
                self.gpa.free(kv.key);
            }
        }
    }

    fn scanWildcardBucketsNoPrune(self: *Router, mode: ScanMode) void {
        var wb_it = self.wildcard_buckets.iterator();
        while (wb_it.next()) |e| {
            scanBucketNoPrune(self, &e.value_ptr.subs, mode);
        }
    }

    fn pruneEmptyWildcardBuckets(self: *Router) void {
        while (true) {
            var empty_key: ?[]const u8 = null;
            var it = self.wildcard_buckets.iterator();
            while (it.next()) |e| {
                if (e.value_ptr.subs.items.len == 0) {
                    empty_key = e.key_ptr.*;
                    break;
                }
            }
            const key = empty_key orelse return;
            if (self.wildcard_buckets.fetchRemove(key)) |kv| {
                var bucket_copy = kv.value;
                bucket_copy.subs.deinit(self.gpa);
                self.gpa.free(kv.key);
            }
        }
    }

    fn scanBucketNoPrune(self: *Router, subs: *std.ArrayListUnmanaged(*Subscription), mode: ScanMode) void {
        var i: usize = 0;
        while (i < subs.items.len) {
            const s = subs.items[i];
            const r = mode.classify(s);
            switch (r.action) {
                .keep => i += 1,
                .set_max => {
                    s.max_msgs = r.max_msgs;
                    i += 1;
                },
                .drop => {
                    _ = subs.swapRemove(i);
                    self.freeSub(s);
                },
            }
        }
    }

    /// Fan-out a publish to matching subscribers. `headers`, when non-null,
    /// is the opaque NATS-v1 header chunk forwarded verbatim to subscribers
    /// via HMSG. The LVC stores payload only (headers are dropped on cache).
    pub fn publish(self: *Router, subject: []const u8, headers: ?[]const u8, payload: []const u8) !void {
        return self.publishInner(subject, headers, payload, null, false);
    }

    /// Like `publish`, but caller can pass the parsed `no_responders` flag and
    /// `reply` subject so this publish can synthesize a 503 to the reply
    /// subject when zero subscribers match. Used by the server when it
    /// dispatches an HPUB carrying `Nats-Request-No-Responders: true`.
    pub fn publishRequest(
        self: *Router,
        subject: []const u8,
        headers: ?[]const u8,
        payload: []const u8,
        reply: ?[]const u8,
        no_responders: bool,
    ) !void {
        return self.publishInner(subject, headers, payload, reply, no_responders);
    }

    fn publishInner(
        self: *Router,
        subject: []const u8,
        headers: ?[]const u8,
        payload: []const u8,
        reply: ?[]const u8,
        no_responders: bool,
    ) !void {
        // $STATS.* is excluded from the LVC (tick-driven, stale by design).
        if (self.lvc.shouldStore(subject, stats_prefix)) {
            try self.lvc.store(subject, payload);
        }

        const have_literal = self.literal_subs.count() != 0;
        const have_wildcard_buckets = self.wildcard_buckets.count() != 0;
        const have_wildcard_global = self.wildcard_global.items.len != 0;
        if (!have_literal and !have_wildcard_buckets and !have_wildcard_global) {
            if (self.bridge_fn) |f| if (self.bridge_ctx) |ctx| f(ctx, subject, payload);
            // Zero subs at all: still synthesize the 503 if requested.
            if (no_responders) if (reply) |r| try self.emitNoResponders(r);
            return;
        }

        defer _ = self.publish_arena.reset(.retain_capacity);
        const scratch = self.publish_arena.allocator();

        var kicks: std.ArrayListUnmanaged(*Conn) = .empty;
        // Subs that hit max_msgs or whose conn closed mid-fanout. Removed
        // after iteration so we don't mutate the bucket we're walking.
        var to_drop: std.ArrayListUnmanaged(*Subscription) = .empty;
        // Queue-group buckets: one entry per (filter, group) seen this publish.
        // Keyed by the same composite key used by qg_rr ("<filter>\x00<group>").
        // Filled during the matching loops below; resolved once at the end.
        var qg_buckets: std.StringHashMapUnmanaged(std.ArrayListUnmanaged(*Subscription)) = .empty;

        if (have_literal) {
            if (self.literal_subs.getPtr(subject)) |bucket| {
                for (bucket.subs.items) |s| {
                    if (s.queue_group) |qg| {
                        try addToQgBucket(&qg_buckets, scratch, s.match_filter, qg, s);
                    } else {
                        try self.deliverOne(s, subject, reply, headers, payload, scratch, &kicks, &to_drop);
                    }
                }
            }
        }

        if (have_wildcard_buckets or have_wildcard_global) {
            var sub_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
            const sub_tokens = splitInto(subject, &sub_tokens_buf);
            const subj_first = firstToken(subject);

            if (have_wildcard_buckets) {
                if (self.wildcard_buckets.getPtr(subj_first)) |bucket| {
                    for (bucket.subs.items) |s| {
                        if (!subject_mod.matchesTokens(s.tokens, sub_tokens)) continue;
                        if (s.queue_group) |qg| {
                            try addToQgBucket(&qg_buckets, scratch, s.match_filter, qg, s);
                        } else {
                            try self.deliverOne(s, subject, reply, headers, payload, scratch, &kicks, &to_drop);
                        }
                    }
                }
            }

            if (have_wildcard_global) {
                for (self.wildcard_global.items) |s| {
                    if (!subject_mod.matchesTokens(s.tokens, sub_tokens)) continue;
                    if (s.queue_group) |qg| {
                        try addToQgBucket(&qg_buckets, scratch, s.match_filter, qg, s);
                    } else {
                        try self.deliverOne(s, subject, reply, headers, payload, scratch, &kicks, &to_drop);
                    }
                }
            }
        }

        // Resolve each queue-group bucket: pick one subscriber via the persistent
        // round-robin cursor on `qg_rr`.
        if (qg_buckets.count() != 0) {
            var qg_it = qg_buckets.iterator();
            while (qg_it.next()) |e| {
                const candidates = e.value_ptr.items;
                if (candidates.len == 0) continue;
                const cursor = try self.qgCursor(e.key_ptr.*);
                const idx = cursor.* % candidates.len;
                cursor.* +%= 1;
                try self.deliverOne(candidates[idx], subject, reply, headers, payload, scratch, &kicks, &to_drop);
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

        // No-responders signal: if the publisher asked for it and zero
        // subscribers actually received the message, synthesize an empty
        // HMSG with NATS/1.0 503 to the reply subject so the requestor's
        // _INBOX.* sub fails fast instead of timing out.
        if (no_responders and kicks.items.len == 0) {
            if (reply) |r| try self.emitNoResponders(r);
        }
    }

    /// Synthesize an empty HMSG with `NATS/1.0 503\r\n\r\n` headers to
    /// `reply_subject`. Goes through the normal publish path so the
    /// requestor's _INBOX.* subscription gets it via fan-out. Explicit
    /// `anyerror` to break the inferred-error-set cycle with publishInner.
    fn emitNoResponders(self: *Router, reply_subject: []const u8) anyerror!void {
        const status_headers = "NATS/1.0 503\r\n\r\n";
        // Recurse without the no_responders flag to avoid an infinite loop
        // if the reply subject also has no subscribers.
        try self.publishInner(reply_subject, status_headers, "", null, false);
    }

    /// Get-or-create a round-robin cursor for `(filter, group)`. Key is owned
    /// by `qg_rr`; cursor wraps via `+%=` so it never traps.
    fn qgCursor(self: *Router, composite_key: []const u8) !*u64 {
        const gop = try self.qg_rr.getOrPut(self.gpa, composite_key);
        if (!gop.found_existing) {
            const owned = self.gpa.dupe(u8, composite_key) catch |err| {
                _ = self.qg_rr.remove(composite_key);
                return err;
            };
            gop.key_ptr.* = owned;
            gop.value_ptr.* = 0;
        }
        return gop.value_ptr;
    }

    /// Append MSG bytes to the conn's outbound buffer, record a kick, queue
    /// the sub for removal on close / max_msgs. Per-sub writeMsg failures
    /// are silently dropped. LVC subscribers always get plain MSG: cached
    /// values have no headers in this version.
    fn deliverOne(
        self: *Router,
        s: *Subscription,
        subject: []const u8,
        reply: ?[]const u8,
        headers: ?[]const u8,
        payload: []const u8,
        scratch: Allocator,
        kicks: *std.ArrayListUnmanaged(*Conn),
        to_drop: *std.ArrayListUnmanaged(*Subscription),
    ) !void {
        if (s.conn.isClosed()) {
            try to_drop.append(scratch, s);
            return;
        }
        if (s.is_lvc) {
            if (!self.lvc.subjectEnabled(subject)) return;
            // LVC replays carry no reply-to: cached values are stale by the time
            // they're delivered, and the original requestor is long gone.
            const out_subject = std.fmt.allocPrint(scratch, "{s}{s}", .{ lvc_prefix, subject }) catch return;
            s.conn.writeMsg(out_subject, s.sid, null, null, payload) catch {};
        } else {
            s.conn.writeMsg(subject, s.sid, reply, headers, payload) catch {};
        }
        try kicks.append(scratch, s.conn);
        s.delivered += 1;
        if (s.max_msgs != null and s.delivered >= s.max_msgs.?) {
            try to_drop.append(scratch, s);
        }
    }

    /// Public wrapper so `snapshot.zig` can populate the LVC at load time.
    pub fn storeLastPublic(self: *Router, subject: []const u8, payload: []const u8) !void {
        if (!self.lvc.shouldStore(subject, stats_prefix)) return;
        return self.lvc.store(subject, payload);
    }
};

/// Add `s` to the queue-group bucket keyed by `(filter, group)`. Composite
/// key uses NUL as separator; allocated in `scratch` (per-publish arena).
fn addToQgBucket(
    qg_buckets: *std.StringHashMapUnmanaged(std.ArrayListUnmanaged(*Subscription)),
    scratch: Allocator,
    filter: []const u8,
    group: []const u8,
    sub: *Subscription,
) !void {
    const key = try std.fmt.allocPrint(scratch, "{s}\x00{s}", .{ filter, group });
    const gop = try qg_buckets.getOrPut(scratch, key);
    if (!gop.found_existing) gop.value_ptr.* = .empty;
    try gop.value_ptr.append(scratch, sub);
}

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
    try r.publish(subj, null, payload);
}

// --- Tests --------------------------------------------------------------

test {
    _ = @import("router/tests.zig");
}
