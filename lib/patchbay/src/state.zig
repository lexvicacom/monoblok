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

/// A compiled rule: filter + body + per-subject state table.
pub const Rule = struct {
    filter: []const u8,
    body: Value,
    /// Per-(rule, subject, op) state for stateful primitives. Keys and
    /// stored payloads are owned by `gpa`.
    state: std.StringHashMapUnmanaged(StateEntry) = .empty,

    /// Cumulative totals, exposed via `$STATS.rules.<i>.*`.
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

/// Stored value for a state slot. Buffers are reused in place across
/// messages (clear + appendSlice, not free + dupe).
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

/// In-progress OHLC bar. `close` is the latest sample, so the bar fires
/// when the Nth sample arrives. `cap` is captured at first call so reload
/// keeps the saved bar even if the patchbay's literal `N` changed.
pub const Ohlc = struct {
    open: f64,
    high: f64,
    low: f64,
    count: u32,
    cap: u32,
};

/// Fixed-capacity ring of f64 samples with running sum and monotonic
/// deques for O(1)-amortised max/min. Deque entries are logical counter
/// values, not buffer offsets; modular math happens at read time. Window
/// size is fixed at first use; differing `moving-N` on the same slot
/// trusts the first allocation.
pub const Ring = struct {
    buf: []f64,
    /// Total samples pushed; positions live at `buf[counter % cap]`.
    counter: u64 = 0,
    sum: f64 = 0,
    /// Front holds the running max / min as a logical counter index.
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

    /// Push `x`, evicting the oldest sample if the ring is full.
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

/// Build a state-table key `"op:subject"` so distinct ops on the same
/// subject don't alias each other's slot.
pub fn stateKey(arena: Allocator, op_name: []const u8, subject: []const u8) Allocator.Error![]const u8 {
    return std.fmt.allocPrint(arena, "{s}:{s}", .{ op_name, subject });
}

pub const StateSlot = struct {
    value_ptr: *StateEntry,
    found_existing: bool,
};

/// Look up or create a state slot. Key is dup'd into `gpa` on insert so
/// the map never holds an arena-owned pointer.
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

/// Look up or create a bytes-typed state slot. Returns true iff the slot
/// was absent or differed from `encoded`; updates it in either case.
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
