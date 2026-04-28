//! Rule + per-rule state machinery shared by the evaluator and built-in ops.
//!
//! `Rule` owns a per-(rule, subject) state table whose values are the union
//! `StateEntry` (bytes for `squelch`, number for `deadband`, ring for
//! `moving-*`, ohlc for `bar`). The state-slot helpers at the bottom are
//! shared by all stateful built-ins.

const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");

pub const Value = sexpr.Value;

/// A compiled rule: a subject filter, a body to evaluate when the filter
/// matches an incoming publish, and a per-subject state table used by
/// stateful primitives (`squelch`, `deadband`).
pub const Rule = struct {
    filter: []const u8,
    body: Value,
    /// Per-subject memory for stateful primitives. Keyed by the concrete
    /// incoming subject; value is the last payload observed (or the last
    /// value of whatever expression the stateful op was called on).
    /// Keys and stored payloads are owned by `gpa` and freed in `deinit`.
    state: std.StringHashMapUnmanaged(StateEntry) = .empty,

    /// Cumulative totals since server start. Exposed via `$STATS.rules.<i>.*`.
    /// `emitted` bumps on each successful `publish` / `publish-to` from this
    /// rule; `suppressed` bumps whenever a gate (`squelch`, `deadband`,
    /// `changed?`, `rising-edge`, `falling-edge`) returns nil.
    publishes_emitted: u64 = 0,
    publishes_suppressed: u64 = 0,

    pub fn deinit(self: *Rule, gpa: Allocator) void {
        var it = self.state.iterator();
        while (it.next()) |e| {
            gpa.free(e.key_ptr.*);
            e.value_ptr.deinit(gpa);
        }
        self.state.deinit(gpa);
    }
};

/// Stored last-seen value for a (rule, subject) pair. Stateful ops store
/// whatever shape they need: a byte buffer (for `squelch`), a number
/// (for `deadband`'s anchor), or a ring of numeric samples (for the
/// `moving-*` family). Buffers are reused in place across messages,
/// clear + appendSlice instead of free + dupe.
pub const StateEntry = union(enum) {
    empty,
    bytes: std.ArrayList(u8),
    number: f64,
    ring: Ring,
    ohlc: Ohlc,

    pub fn deinit(self: *StateEntry, gpa: Allocator) void {
        switch (self.*) {
            .bytes => |*b| b.deinit(gpa),
            .ring => |*r| r.deinit(gpa),
            else => {},
        }
    }
};

/// Open / high / low bookkeeping for an in-progress OHLC bar. `close` is
/// always the most recent sample so we don't need to store it; instead we
/// fire the bar at the moment the Nth sample arrives. `cap` is the bar
/// width N captured at first call so it survives a snapshot reload even if
/// the new patchbay's literal `N` differs (we trust the saved bar over the
/// patchbay text).
pub const Ohlc = struct {
    open: f64,
    high: f64,
    low: f64,
    count: u32,
    cap: u32,
};

/// Fixed-capacity ring of f64 samples with a running sum and monotonic
/// deques for O(1)-amortized max/min. The deques hold indices into the
/// ring (index is a monotonically increasing logical counter, not a
/// buffer offset, modular math happens at read time).
///
/// Window size is fixed at first use per (rule, subject, op) slot. It's
/// a usage error to call different `moving-N` sizes on the same slot;
/// we just trust the first allocation.
pub const Ring = struct {
    buf: []f64,
    /// Total samples pushed ever. `len = min(counter, cap)`. Positions
    /// live at `buf[counter % cap]` on write.
    counter: u64 = 0,
    sum: f64 = 0,
    /// Indices (as `counter` values, not buffer offsets) of candidates
    /// still in the window, front holds the running max / min.
    max_deque: std.ArrayList(u64) = .empty,
    min_deque: std.ArrayList(u64) = .empty,

    pub fn init(gpa: Allocator, cap: usize) !Ring {
        return .{ .buf = try gpa.alloc(f64, cap) };
    }

    pub fn deinit(self: *Ring, gpa: Allocator) void {
        gpa.free(self.buf);
        self.max_deque.deinit(gpa);
        self.min_deque.deinit(gpa);
    }

    pub fn len(self: Ring) usize {
        const cap = self.buf.len;
        return if (self.counter < cap) @intCast(self.counter) else cap;
    }

    /// Push `x`. Evicts the oldest sample if the ring was full, updates
    /// sum and both monotonic deques.
    pub fn push(self: *Ring, gpa: Allocator, x: f64) !void {
        const cap = self.buf.len;
        const idx = self.counter;
        if (self.counter >= cap) {
            const evict_idx = self.counter - cap;
            self.sum -= self.buf[@intCast(evict_idx % cap)];
            if (self.max_deque.items.len > 0 and self.max_deque.items[0] == evict_idx) {
                _ = self.max_deque.orderedRemove(0);
            }
            if (self.min_deque.items.len > 0 and self.min_deque.items[0] == evict_idx) {
                _ = self.min_deque.orderedRemove(0);
            }
        }
        self.buf[@intCast(idx % cap)] = x;
        self.sum += x;

        while (self.max_deque.items.len > 0) {
            const tail = self.max_deque.items[self.max_deque.items.len - 1];
            if (self.buf[@intCast(tail % cap)] <= x) {
                _ = self.max_deque.pop();
            } else break;
        }
        try self.max_deque.append(gpa, idx);

        while (self.min_deque.items.len > 0) {
            const tail = self.min_deque.items[self.min_deque.items.len - 1];
            if (self.buf[@intCast(tail % cap)] >= x) {
                _ = self.min_deque.pop();
            } else break;
        }
        try self.min_deque.append(gpa, idx);

        self.counter += 1;
    }

    pub fn mean(self: Ring) f64 {
        const n = self.len();
        return self.sum / @as(f64, @floatFromInt(n));
    }

    pub fn max(self: Ring) f64 {
        const cap = self.buf.len;
        return self.buf[@intCast(self.max_deque.items[0] % cap)];
    }

    pub fn min(self: Ring) f64 {
        const cap = self.buf.len;
        return self.buf[@intCast(self.min_deque.items[0] % cap)];
    }
};

// --- State-slot helpers, shared by stateful built-ins -------------------

pub const StateError = error{TypeMismatch} || Allocator.Error;

/// Coerce a value into a stable byte representation for equality state.
/// Numbers are formatted canonically so `(squelch 1)` and `(squelch 1.0)`
/// compare equal.
pub fn encodeForState(arena: Allocator, v: Value) StateError![]const u8 {
    return switch (v) {
        .string, .symbol => |s| s,
        .number => |n| try std.fmt.allocPrint(arena, "{d}", .{n}),
        .boolean => |b| if (b) "true" else "false",
        .nil => "",
        .list, .keyword => error.TypeMismatch,
    };
}

/// Build a state-table key `"op:subject"` in the per-message arena. Every
/// stateful op namespaces its keys by op name so distinct ops on the same
/// subject cannot alias each other's state slot.
pub fn stateKey(arena: Allocator, op_name: []const u8, subject: []const u8) Allocator.Error![]const u8 {
    return std.fmt.allocPrint(arena, "{s}:{s}", .{ op_name, subject });
}

pub const StateSlot = struct {
    value_ptr: *StateEntry,
    found_existing: bool,
};

/// Look up (or create) a state slot. On insert the key is dup'd into `gpa`
/// before calling `getOrPut`, so the map never holds a pointer into the
/// caller's per-message arena (avoids a UAF if a later alloc fails between
/// `getOrPut` and an overwrite, and makes map iteration safe across the
/// next arena reset).
pub fn getOrPutStateSlot(gpa: Allocator, rule: *Rule, key: []const u8) Allocator.Error!StateSlot {
    const gop = try rule.state.getOrPutAdapted(gpa, key, std.hash_map.StringContext{});
    if (gop.found_existing) {
        return .{ .value_ptr = gop.value_ptr, .found_existing = true };
    }
    const owned_key = try gpa.dupe(u8, key);
    gop.key_ptr.* = owned_key;
    gop.value_ptr.* = .empty;
    return .{ .value_ptr = gop.value_ptr, .found_existing = false };
}

/// Look up (or create) a bytes-typed state slot. Returns true iff the slot
/// was absent or differed from `encoded`, and updates it to `encoded` in
/// either case. The backing ArrayList is reused across messages
/// (clear + appendSlice, not free + dupe).
pub fn stateEqualsOrStore(
    gpa: Allocator,
    rule: *Rule,
    key: []const u8,
    encoded: []const u8,
) Allocator.Error!bool {
    const slot = try getOrPutStateSlot(gpa, rule, key);
    if (!slot.found_existing) {
        var buf: std.ArrayList(u8) = .empty;
        try buf.appendSlice(gpa, encoded);
        slot.value_ptr.* = .{ .bytes = buf };
        return true;
    }
    const prev = &slot.value_ptr.bytes;
    if (std.mem.eql(u8, prev.items, encoded)) return false;
    prev.clearRetainingCapacity();
    try prev.appendSlice(gpa, encoded);
    return true;
}

pub fn asString(v: Value) StateError![]const u8 {
    return switch (v) {
        .string => |s| s,
        .symbol => |s| s,
        else => error.TypeMismatch,
    };
}

pub fn asNumber(v: Value) StateError!f64 {
    return switch (v) {
        .number => |n| n,
        .string => |s| std.fmt.parseFloat(f64, s) catch error.TypeMismatch,
        else => error.TypeMismatch,
    };
}

pub fn valueEql(a: Value, b: Value) bool {
    const TagA = @typeInfo(Value).@"union".tag_type.?;
    if (@as(TagA, a) != @as(TagA, b)) return false;
    return switch (a) {
        .nil => true,
        .boolean => |x| x == b.boolean,
        .number => |x| x == b.number,
        .symbol => |s| std.mem.eql(u8, s, b.symbol),
        .keyword => |s| std.mem.eql(u8, s, b.keyword),
        .string => |s| std.mem.eql(u8, s, b.string),
        .list => false, // lists aren't comparable in our dialect
    };
}
