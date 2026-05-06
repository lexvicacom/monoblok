//! Rule + per-rule state machinery shared by the evaluator and built-in ops.
//!
//! `Rule` owns a per-(rule, subject) state table whose values are the union
//! `StateEntry` (bytes for `squelch`, number for `deadband`, ring for
//! `moving-*`, ohlc for `bar`). The state-slot helpers at the bottom are
//! shared by all stateful built-ins.

const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");
const kernel = @import("kernel.zig");

pub const Value = sexpr.Value;

/// A compiled rule: filter + body + per-subject state table.
pub const Rule = struct {
    filter: []const u8,
    body: Value,
    /// Per-(rule, subject, op) state for stateful primitives. Keys and
    /// stored payloads are owned by `gpa`.
    state: std.StringHashMapUnmanaged(StateEntry) = .empty,

    /// Opt-in: when true, publishes this rule emits re-enter the patchbay
    /// and may match other rules. Default false avoids surprise rule-graph
    /// cascades (most rules don't need to feed each other). Set via
    /// `(on FILTER :reentrant true BODY)`.
    reentrant: bool = false,

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
    time_ring: TimeRing,
    ohlc: Ohlc,
    clocked: Clocked,

    pub fn deinit(self: *StateEntry, gpa: Allocator) void {
        switch (self.*) {
            .bytes => |*b| b.deinit(gpa),
            .ring => |*r| r.deinit(gpa),
            .time_ring => |*r| r.deinit(gpa),
            .clocked => |*c| c.deinit(gpa),
            else => {},
        }
    }
};

pub const ClockedKind = enum { on_silence, debounce, sample, aggregate };
pub const AggregateKind = enum { avg, sum, min, max, count, rate };

/// Host-clocked patchbay state. `body` points into the loaded rule arena
/// and is therefore not snapshot-persisted; these slots are rebuilt as
/// matching messages arrive after a restart.
pub const Clocked = struct {
    kind: ClockedKind,
    deadline_ms: ?i64 = null,
    period_ms: u64,
    aggregate_kind: AggregateKind = .avg,
    body: []const Value = &.{},
    subject: std.ArrayList(u8) = .empty,
    payload: std.ArrayList(u8) = .empty,
    samples: TimeRing = TimeRing.init(1),

    pub fn deinit(self: *Clocked, gpa: Allocator) void {
        self.subject.deinit(gpa);
        self.payload.deinit(gpa);
        self.samples.deinit(gpa);
    }

    pub fn nextDeadlineMs(self: Clocked) ?i64 {
        return self.deadline_ms;
    }

    pub fn setSubject(self: *Clocked, gpa: Allocator, subject: []const u8) !void {
        self.subject.clearRetainingCapacity();
        try self.subject.appendSlice(gpa, subject);
    }

    pub fn setPayload(self: *Clocked, gpa: Allocator, payload: []const u8) !void {
        self.payload.clearRetainingCapacity();
        try self.payload.appendSlice(gpa, payload);
    }
};

/// Transition logic + field layout live in `kernel.zig` (`Bar`); the host
/// stores instances by value and snapshots them. `cap != 0` => tick mode,
/// `window_ms != 0` => time mode.
pub const Ohlc = kernel.Bar;

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
        var r: Ring = .{ .buf = try gpa.alloc(f64, cap) };
        errdefer gpa.free(r.buf);
        errdefer r.max_deque.deinit(gpa);
        try r.max_deque.ensureTotalCapacity(gpa, cap);
        try r.min_deque.ensureTotalCapacity(gpa, cap);
        return r;
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
        _ = gpa;
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
        self.max_deque.appendAssumeCapacity(idx);

        while (self.min_deque.items.len > 0) {
            const tail = self.min_deque.items[self.min_deque.items.len - 1];
            if (self.buf[@intCast(tail % cap)] >= x) {
                _ = self.min_deque.pop();
            } else break;
        }
        self.min_deque.appendAssumeCapacity(idx);

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

/// Time-windowed sample buffer. Holds (value, ts_ms) pairs and evicts
/// entries older than `window_ms` from the front. Storage grows
/// unbounded with event rate; aggregate readers do an O(n) scan over
/// the live window. Trade-off vs. monotonic deques: simpler, snapshot-
/// friendly, and fine for the rates we expect (sensors / tickers).
pub const TimeRing = struct {
    pub const Sample = struct { value: f64, ts_ms: i64 };

    samples: std.ArrayList(Sample) = .empty,
    /// Window duration in ms. Captured at first push; reload keeps it.
    window_ms: u64,

    pub fn init(window_ms: u64) TimeRing {
        return .{ .window_ms = window_ms };
    }

    pub fn deinit(self: *TimeRing, gpa: Allocator) void {
        self.samples.deinit(gpa);
    }

    pub fn len(self: TimeRing) usize {
        return self.samples.items.len;
    }

    /// Drop samples whose `ts_ms` is older than `now - window_ms`.
    pub fn evict(self: *TimeRing, now_ms: i64) void {
        const cutoff = now_ms - @as(i64, @intCast(self.window_ms));
        var keep_from: usize = 0;
        for (self.samples.items) |s| {
            if (s.ts_ms < cutoff) keep_from += 1 else break;
        }
        if (keep_from > 0) {
            const rest = self.samples.items.len - keep_from;
            std.mem.copyForwards(
                Sample,
                self.samples.items[0..rest],
                self.samples.items[keep_from..],
            );
            self.samples.items.len = rest;
        }
    }

    pub fn push(self: *TimeRing, gpa: Allocator, value: f64, ts_ms: i64) !void {
        self.evict(ts_ms);
        try self.samples.append(gpa, .{ .value = value, .ts_ms = ts_ms });
    }

    /// Wall-clock ms at which `evict` would drop the oldest sample. Null
    /// when the ring is empty (no eviction pending). The host schedules a
    /// one-shot timer for this moment and re-queries after each fire.
    pub fn nextDeadlineMs(self: TimeRing) ?i64 {
        if (self.samples.items.len == 0) return null;
        return self.samples.items[0].ts_ms + @as(i64, @intCast(self.window_ms));
    }

    pub fn sum(self: TimeRing) f64 {
        var s: f64 = 0;
        for (self.samples.items) |x| s += x.value;
        return s;
    }

    pub fn mean(self: TimeRing) f64 {
        const n = self.samples.items.len;
        if (n == 0) return 0;
        return self.sum() / @as(f64, @floatFromInt(n));
    }

    pub fn max(self: TimeRing) f64 {
        var m: f64 = self.samples.items[0].value;
        for (self.samples.items[1..]) |x| {
            if (x.value > m) m = x.value;
        }
        return m;
    }

    pub fn min(self: TimeRing) f64 {
        var m: f64 = self.samples.items[0].value;
        for (self.samples.items[1..]) |x| {
            if (x.value < m) m = x.value;
        }
        return m;
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
        .list, .vector, .keyword => error.TypeMismatch,
    };
}

/// Per-(rule, op, window-kind, subject) state-table key. Plain bytes so
/// the HashMap hashes/eqs them as-is and the snapshot file format is
/// unchanged. Kind is `n` for non-windowed ops, `t` for ticks, `m` for
/// time-ms; encoded in one byte so there's nothing to ambiguate.
///
/// On-the-wire shape: `"<op_name>/<kind>:<subject>"` (e.g. `"squelch/n:foo"`,
/// `"moving-avg/t:foo.bar"`, `"bar/m:MARKET.AAPL"`). Construct via
/// `keyForOp` / `keyForWindow`. Hosts that need to extract the subject
/// from a slot key (e.g. the clock registry) do their own prefix-strip
/// against the known op_name. Don't build keys ad-hoc with `allocPrint`;
/// the format lives here.
pub const KeyKind = enum { none, ticks, time_ms };

fn kindByte(k: KeyKind) u8 {
    return switch (k) {
        .none => 'n',
        .ticks => 't',
        .time_ms => 'm',
    };
}

pub fn keyForOp(arena: Allocator, op_name: []const u8, subject: []const u8) Allocator.Error![]const u8 {
    return keyFor(arena, op_name, .none, subject);
}

pub fn keyForWindow(
    arena: Allocator,
    op_name: []const u8,
    kind: KeyKind,
    subject: []const u8,
) Allocator.Error![]const u8 {
    std.debug.assert(kind != .none);
    return keyFor(arena, op_name, kind, subject);
}

fn keyFor(arena: Allocator, op_name: []const u8, kind: KeyKind, subject: []const u8) Allocator.Error![]const u8 {
    return std.fmt.allocPrint(arena, "{s}/{c}:{s}", .{ op_name, kindByte(kind), subject });
}

pub const StateSlot = struct {
    value_ptr: *StateEntry,
    /// Stable, gpa-owned key string the rule's hashmap holds. Safe for the
    /// host to keep across publishes (lives until the slot is removed or
    /// the rule is deinit'd). Use this rather than the arena-owned key the
    /// caller built — the latter is reset between PUBs.
    key_ptr: []const u8,
    found_existing: bool,
};

/// Look up or create a state slot. Key is dup'd into `gpa` on insert so
/// the map never holds an arena-owned pointer.
pub fn getOrPutStateSlot(gpa: Allocator, rule: *Rule, key: []const u8) Allocator.Error!StateSlot {
    const gop = try rule.state.getOrPutAdapted(gpa, key, std.hash_map.StringContext{});
    if (gop.found_existing) {
        return .{ .value_ptr = gop.value_ptr, .key_ptr = gop.key_ptr.*, .found_existing = true };
    }
    errdefer _ = rule.state.remove(key);
    const owned_key = try gpa.dupe(u8, key);
    gop.key_ptr.* = owned_key;
    gop.value_ptr.* = .empty;
    return .{ .value_ptr = gop.value_ptr, .key_ptr = owned_key, .found_existing = false };
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
        .list, .vector => false, // collections aren't comparable as whole values
    };
}

test "TimeRing.nextDeadlineMs tracks the oldest sample" {
    const gpa = std.testing.allocator;
    var r = TimeRing.init(1000);
    defer r.deinit(gpa);

    try std.testing.expectEqual(@as(?i64, null), r.nextDeadlineMs());

    try r.push(gpa, 1.0, 100);
    try std.testing.expectEqual(@as(?i64, 1100), r.nextDeadlineMs());

    try r.push(gpa, 2.0, 500);
    try std.testing.expectEqual(@as(?i64, 1100), r.nextDeadlineMs());

    // Push at 1200 evicts the ts=100 entry (cutoff=200), so the new oldest
    // is ts=500 and the deadline shifts to 1500.
    try r.push(gpa, 3.0, 1200);
    try std.testing.expectEqual(@as(?i64, 1500), r.nextDeadlineMs());

    // Drain via evict-only (no push). After cutoff > 1200, ring is empty.
    r.evict(2300);
    try std.testing.expectEqual(@as(?i64, null), r.nextDeadlineMs());
}
