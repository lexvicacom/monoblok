//! Snapshot file format (v3).
//!
//! v3 changed the per-rule state-key format: every key now carries an
//! explicit window-kind byte (`/n`, `/t`, or `/m`), so v2 keys won't
//! match. v2 snapshots aren't read; a version bump on a fresh
//! `--snapshot` path just means starting empty.
//!
//! Layout:
//!   magic     "MBLK"   (4 bytes)
//!   version   u8       (currently 3)
//!   reserved  [3]u8    (zero)
//!   records   repeated tagged entries:
//!     kind u8
//!       0x01 = lvc_entry:
//!         subject_len u32  subject bytes
//!         payload_len u32  payload bytes
//!       0x02 = rule_state:
//!         rule_idx   u32
//!         filter_len u32  filter bytes       (disambiguates if rule order changes)
//!         key_len    u32  key bytes           ("op:subject" or "op/kind:subject")
//!         variant    u8   (see StateEntry variants below)
//!         variant-specific body
//!   EOF
//!
//! StateEntry variants (matches lib/patchbay/src/state.zig StateEntry union):
//!   0x00 empty:
//!     (no body)
//!   0x01 bytes:
//!     data_len u32  data bytes
//!   0x02 number:
//!     f64 (little-endian bit pattern, 8 bytes)
//!   0x03 ring:
//!     cap      u32
//!     counter  u64
//!     sum      f64
//!     cap     × f64            (buf, in slot order)
//!     max_len  u32
//!     max_len × u64            (max_deque)
//!     min_len  u32
//!     min_len × u64            (min_deque)
//!   0x04 ohlc:
//!     open            f64
//!     high            f64
//!     low             f64
//!     count           u32
//!     cap             u32
//!     window_ms       u64
//!     window_start_ms i64
//!     last_close      f64
//!   0x05 time_ring:
//!     window_ms u64
//!     n         u32           number of samples
//!     n × { value f64; ts_ms i64 }
//!
//! No checksum, no compression. If anything changes, bump `version`.

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;
const router_mod = @import("router.zig");
const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;

pub const magic = "MBLK";
pub const version: u8 = 3;
pub const header_len = 8;

pub const max_field_len: u32 = 16 * 1024 * 1024;

pub const kind_lvc: u8 = 0x01;
pub const kind_rule_state: u8 = 0x02;

pub const variant_empty: u8 = 0x00;
pub const variant_bytes: u8 = 0x01;
pub const variant_number: u8 = 0x02;
pub const variant_ring: u8 = 0x03;
pub const variant_ohlc: u8 = 0x04;
pub const variant_time_ring: u8 = 0x05;

pub const LvcEntry = struct {
    subject: []const u8,
    payload: []const u8,
};

pub const RuleStateEntry = struct {
    rule_idx: u32,
    filter: []const u8,
    key: []const u8,
    value: rules_mod.StateEntry,
};

/// Point-in-time copy produced on the loop thread and handed to the worker.
pub const Snapshot = struct {
    lvc: []const LvcEntry = &.{},
    rule_state: []const RuleStateEntry = &.{},
};

pub const LoadError = error{
    BadMagic,
    UnsupportedVersion,
    Truncated,
    FieldTooLarge,
    UnknownKind,
    UnknownVariant,
} || Allocator.Error;

// --- Dump --------------------------------------------------------------

pub fn dumpInto(
    gpa: Allocator,
    out: *std.ArrayList(u8),
    snap: Snapshot,
) !void {
    try out.appendSlice(gpa, magic);
    try out.append(gpa, version);
    try out.appendSlice(gpa, &[_]u8{ 0, 0, 0 });

    for (snap.lvc) |e| try writeLvc(gpa, out, e);
    for (snap.rule_state) |e| try writeRuleState(gpa, out, e);
}

fn writeLvc(gpa: Allocator, out: *std.ArrayList(u8), e: LvcEntry) !void {
    std.debug.assert(e.subject.len <= max_field_len);
    std.debug.assert(e.payload.len <= max_field_len);
    try out.append(gpa, kind_lvc);
    try writeLenPrefixed(gpa, out, e.subject);
    try writeLenPrefixed(gpa, out, e.payload);
}

fn writeRuleState(gpa: Allocator, out: *std.ArrayList(u8), e: RuleStateEntry) !void {
    try out.append(gpa, kind_rule_state);
    try writeU32(gpa, out, e.rule_idx);
    try writeLenPrefixed(gpa, out, e.filter);
    try writeLenPrefixed(gpa, out, e.key);

    switch (e.value) {
        .empty => try out.append(gpa, variant_empty),
        .bytes => |b| {
            try out.append(gpa, variant_bytes);
            try writeLenPrefixed(gpa, out, b.items);
        },
        .number => |n| {
            try out.append(gpa, variant_number);
            try writeF64(gpa, out, n);
        },
        .ring => |r| {
            try out.append(gpa, variant_ring);
            try writeU32(gpa, out, @intCast(r.buf.len));
            try writeU64(gpa, out, r.counter);
            try writeF64(gpa, out, r.sum);
            for (r.buf) |v| try writeF64(gpa, out, v);
            try writeU32(gpa, out, @intCast(r.max_deque.items.len));
            for (r.max_deque.items) |v| try writeU64(gpa, out, v);
            try writeU32(gpa, out, @intCast(r.min_deque.items.len));
            for (r.min_deque.items) |v| try writeU64(gpa, out, v);
        },
        .ohlc => |b| {
            try out.append(gpa, variant_ohlc);
            try writeF64(gpa, out, b.open);
            try writeF64(gpa, out, b.high);
            try writeF64(gpa, out, b.low);
            try writeU32(gpa, out, b.count);
            try writeU32(gpa, out, b.cap);
            try writeU64(gpa, out, b.window_ms);
            try writeI64(gpa, out, b.window_start_ms);
            try writeF64(gpa, out, b.last_close);
        },
        .time_ring => |r| {
            try out.append(gpa, variant_time_ring);
            try writeU64(gpa, out, r.window_ms);
            try writeU32(gpa, out, @intCast(r.samples.items.len));
            for (r.samples.items) |s| {
                try writeF64(gpa, out, s.value);
                try writeI64(gpa, out, s.ts_ms);
            }
        },
        .clocked => try out.append(gpa, variant_empty),
    }
}

fn writeI64(gpa: Allocator, out: *std.ArrayList(u8), n: i64) !void {
    try writeU64(gpa, out, @bitCast(n));
}

fn writeLenPrefixed(gpa: Allocator, out: *std.ArrayList(u8), s: []const u8) !void {
    std.debug.assert(s.len <= max_field_len);
    try writeU32(gpa, out, @intCast(s.len));
    try out.appendSlice(gpa, s);
}

fn writeU32(gpa: Allocator, out: *std.ArrayList(u8), n: u32) !void {
    var buf: [4]u8 = undefined;
    std.mem.writeInt(u32, &buf, n, .little);
    try out.appendSlice(gpa, &buf);
}

fn writeU64(gpa: Allocator, out: *std.ArrayList(u8), n: u64) !void {
    var buf: [8]u8 = undefined;
    std.mem.writeInt(u64, &buf, n, .little);
    try out.appendSlice(gpa, &buf);
}

fn writeF64(gpa: Allocator, out: *std.ArrayList(u8), x: f64) !void {
    try writeU64(gpa, out, @bitCast(x));
}

// --- Parse -------------------------------------------------------------

/// Visit callback shape: caller provides a context with `onLvc` and
/// `onRuleState` methods. Both receive slices into `bytes` (no copy).
pub fn parse(bytes: []const u8, ctx: anytype) !void {
    if (bytes.len < header_len) return LoadError.Truncated;
    if (!std.mem.eql(u8, bytes[0..4], magic)) return LoadError.BadMagic;
    if (bytes[4] != version) return LoadError.UnsupportedVersion;

    var i: usize = header_len;
    while (i < bytes.len) {
        const kind = try readU8(bytes, &i);
        switch (kind) {
            kind_lvc => {
                const subject = try readLenPrefixed(bytes, &i);
                const payload = try readLenPrefixed(bytes, &i);
                try ctx.onLvc(subject, payload);
            },
            kind_rule_state => {
                const rule_idx = try readU32(bytes, &i);
                const filter = try readLenPrefixed(bytes, &i);
                const key = try readLenPrefixed(bytes, &i);
                const variant = try readU8(bytes, &i);

                switch (variant) {
                    variant_empty => try ctx.onRuleState(rule_idx, filter, key, .empty),
                    variant_bytes => {
                        const data = try readLenPrefixed(bytes, &i);
                        try ctx.onRuleStateBytes(rule_idx, filter, key, data);
                    },
                    variant_number => {
                        const n = try readF64(bytes, &i);
                        try ctx.onRuleState(rule_idx, filter, key, .{ .number = n });
                    },
                    variant_ring => {
                        const cap = try readU32(bytes, &i);
                        const counter = try readU64(bytes, &i);
                        const sum = try readF64(bytes, &i);
                        if (i + @as(usize, cap) * 8 > bytes.len) return LoadError.Truncated;
                        const buf = bytes[i .. i + @as(usize, cap) * 8];
                        i += @as(usize, cap) * 8;
                        const max_len = try readU32(bytes, &i);
                        if (i + @as(usize, max_len) * 8 > bytes.len) return LoadError.Truncated;
                        const max_bytes = bytes[i .. i + @as(usize, max_len) * 8];
                        i += @as(usize, max_len) * 8;
                        const min_len = try readU32(bytes, &i);
                        if (i + @as(usize, min_len) * 8 > bytes.len) return LoadError.Truncated;
                        const min_bytes = bytes[i .. i + @as(usize, min_len) * 8];
                        i += @as(usize, min_len) * 8;
                        try ctx.onRuleStateRing(rule_idx, filter, key, .{
                            .cap = cap,
                            .counter = counter,
                            .sum = sum,
                            .buf_bytes = buf,
                            .max_bytes = max_bytes,
                            .min_bytes = min_bytes,
                        });
                    },
                    variant_ohlc => {
                        const open = try readF64(bytes, &i);
                        const high = try readF64(bytes, &i);
                        const low = try readF64(bytes, &i);
                        const count = try readU32(bytes, &i);
                        const cap = try readU32(bytes, &i);
                        const window_ms = try readU64(bytes, &i);
                        const window_start_ms = try readI64(bytes, &i);
                        const last_close = try readF64(bytes, &i);
                        try ctx.onRuleState(rule_idx, filter, key, .{
                            .ohlc = .{
                                .open = open,
                                .high = high,
                                .low = low,
                                .count = count,
                                .cap = cap,
                                .window_ms = window_ms,
                                .window_start_ms = window_start_ms,
                                .last_close = last_close,
                            },
                        });
                    },
                    variant_time_ring => {
                        const window_ms = try readU64(bytes, &i);
                        const n = try readU32(bytes, &i);
                        // Each sample is 16 bytes (f64 value + i64 ts).
                        if (i + @as(usize, n) * 16 > bytes.len) return LoadError.Truncated;
                        const samples_bytes = bytes[i .. i + @as(usize, n) * 16];
                        i += @as(usize, n) * 16;
                        try ctx.onRuleStateTimeRing(rule_idx, filter, key, .{
                            .window_ms = window_ms,
                            .n = n,
                            .samples_bytes = samples_bytes,
                        });
                    },
                    else => return LoadError.UnknownVariant,
                }
            },
            else => return LoadError.UnknownKind,
        }
    }
}

pub const RingWireView = struct {
    cap: u32,
    counter: u64,
    sum: f64,
    buf_bytes: []const u8,
    max_bytes: []const u8,
    min_bytes: []const u8,
};

pub const TimeRingWireView = struct {
    window_ms: u64,
    n: u32,
    /// Tightly packed: each sample is 8 bytes f64 + 8 bytes i64.
    samples_bytes: []const u8,
};

fn readU8(bytes: []const u8, i: *usize) !u8 {
    if (i.* + 1 > bytes.len) return LoadError.Truncated;
    const b = bytes[i.*];
    i.* += 1;
    return b;
}

fn readU32(bytes: []const u8, i: *usize) !u32 {
    if (i.* + 4 > bytes.len) return LoadError.Truncated;
    const n = std.mem.readInt(u32, bytes[i.*..][0..4], .little);
    i.* += 4;
    return n;
}

fn readU64(bytes: []const u8, i: *usize) !u64 {
    if (i.* + 8 > bytes.len) return LoadError.Truncated;
    const n = std.mem.readInt(u64, bytes[i.*..][0..8], .little);
    i.* += 8;
    return n;
}

fn readF64(bytes: []const u8, i: *usize) !f64 {
    return @bitCast(try readU64(bytes, i));
}

fn readI64(bytes: []const u8, i: *usize) !i64 {
    return @bitCast(try readU64(bytes, i));
}

fn readLenPrefixed(bytes: []const u8, i: *usize) ![]const u8 {
    const n = try readU32(bytes, i);
    if (n > max_field_len) return LoadError.FieldTooLarge;
    if (i.* + n > bytes.len) return LoadError.Truncated;
    const s = bytes[i.* .. i.* + n];
    i.* += n;
    return s;
}

// --- Router / rules integration -----------------------------------------

/// Collect a point-in-time snapshot into `arena`. Must be called on the
/// loop thread so the deep copy is race-free against live publishes.
pub fn collect(
    arena: Allocator,
    router: *router_mod.Router,
    rules: []const rules_mod.Rule,
) !Snapshot {
    var lvc: std.ArrayList(LvcEntry) = .empty;
    var it = router.lvc.last_value.iterator();
    while (it.next()) |e| {
        try lvc.append(arena, .{
            .subject = try arena.dupe(u8, e.key_ptr.*),
            .payload = try arena.dupe(u8, e.value_ptr.items),
        });
    }

    var rs: std.ArrayList(RuleStateEntry) = .empty;
    for (rules, 0..) |*rule, idx| {
        var sit = rule.state.iterator();
        while (sit.next()) |se| {
            if (se.value_ptr.* == .clocked) continue;
            try rs.append(arena, .{
                .rule_idx = @intCast(idx),
                .filter = try arena.dupe(u8, rule.filter),
                .key = try arena.dupe(u8, se.key_ptr.*),
                .value = try dupStateEntry(arena, se.value_ptr.*),
            });
        }
    }

    return .{
        .lvc = try lvc.toOwnedSlice(arena),
        .rule_state = try rs.toOwnedSlice(arena),
    };
}

fn dupStateEntry(arena: Allocator, src: rules_mod.StateEntry) !rules_mod.StateEntry {
    return switch (src) {
        .empty => .empty,
        .number => |n| .{ .number = n },
        .bytes => |b| blk: {
            var copy: std.ArrayList(u8) = .empty;
            try copy.appendSlice(arena, b.items);
            break :blk .{ .bytes = copy };
        },
        .ring => |r| blk: {
            const buf_copy = try arena.dupe(f64, r.buf);
            var max_copy: std.ArrayList(u64) = .empty;
            try max_copy.appendSlice(arena, r.max_deque.items);
            var min_copy: std.ArrayList(u64) = .empty;
            try min_copy.appendSlice(arena, r.min_deque.items);
            break :blk .{ .ring = .{
                .buf = buf_copy,
                .counter = r.counter,
                .sum = r.sum,
                .max_deque = max_copy,
                .min_deque = min_copy,
            } };
        },
        .time_ring => |r| blk: {
            var samples: std.ArrayList(rules_mod.TimeRing.Sample) = .empty;
            try samples.appendSlice(arena, r.samples.items);
            break :blk .{ .time_ring = .{
                .samples = samples,
                .window_ms = r.window_ms,
            } };
        },
        .ohlc => |b| .{ .ohlc = b },
        .clocked => .empty,
    };
}

/// Load a snapshot into `router`'s LVC and `rules[*].state`. Rule-state
/// entries whose `rule_idx` is out of range or whose recorded `filter`
/// doesn't match the current rule are skipped with a warning (patchbay
/// changed since the snapshot was written).
pub fn loadInto(
    gpa: Allocator,
    bytes: []const u8,
    router: *router_mod.Router,
    rules: []rules_mod.Rule,
) !void {
    const Ctx = struct {
        gpa: Allocator,
        router: *router_mod.Router,
        rules: []rules_mod.Rule,

        fn onLvc(self: *@This(), subj: []const u8, payload: []const u8) !void {
            try self.router.storeLastPublic(subj, payload);
        }

        fn onRuleState(
            self: *@This(),
            rule_idx: u32,
            filter: []const u8,
            key: []const u8,
            value: rules_mod.StateEntry,
        ) !void {
            const rule = self.resolveRule(rule_idx, filter) orelse return;
            try installSimple(self.gpa, rule, key, value);
        }

        fn onRuleStateBytes(
            self: *@This(),
            rule_idx: u32,
            filter: []const u8,
            key: []const u8,
            data: []const u8,
        ) !void {
            const rule = self.resolveRule(rule_idx, filter) orelse return;
            try installBytes(self.gpa, rule, key, data);
        }

        fn onRuleStateRing(
            self: *@This(),
            rule_idx: u32,
            filter: []const u8,
            key: []const u8,
            view: RingWireView,
        ) !void {
            const rule = self.resolveRule(rule_idx, filter) orelse return;
            try installRing(self.gpa, rule, key, view);
        }

        fn onRuleStateTimeRing(
            self: *@This(),
            rule_idx: u32,
            filter: []const u8,
            key: []const u8,
            view: TimeRingWireView,
        ) !void {
            const rule = self.resolveRule(rule_idx, filter) orelse return;
            try installTimeRing(self.gpa, rule, key, view);
        }

        fn resolveRule(self: *@This(), rule_idx: u32, filter: []const u8) ?*rules_mod.Rule {
            if (rule_idx >= self.rules.len) {
                std.log.warn("snapshot: rule_idx {d} out of range ({d} rules); skipping state", .{ rule_idx, self.rules.len });
                return null;
            }
            const rule = &self.rules[rule_idx];
            if (!std.mem.eql(u8, rule.filter, filter)) {
                std.log.warn(
                    "snapshot: rule {d} filter mismatch (snap='{s}' now='{s}'); skipping state",
                    .{ rule_idx, filter, rule.filter },
                );
                return null;
            }
            return rule;
        }
    };

    var ctx: Ctx = .{ .gpa = gpa, .router = router, .rules = rules };
    try parse(bytes, &ctx);
}

fn installSimple(
    gpa: Allocator,
    rule: *rules_mod.Rule,
    key: []const u8,
    value: rules_mod.StateEntry,
) !void {
    const owned = try gpa.dupe(u8, key);
    errdefer gpa.free(owned);
    const gop = try rule.state.getOrPut(gpa, owned);
    if (gop.found_existing) {
        gpa.free(owned);
        gop.value_ptr.deinit(gpa);
    } else {
        gop.key_ptr.* = owned;
    }
    gop.value_ptr.* = value;
}

fn installBytes(
    gpa: Allocator,
    rule: *rules_mod.Rule,
    key: []const u8,
    data: []const u8,
) !void {
    var list: std.ArrayList(u8) = .empty;
    errdefer list.deinit(gpa);
    try list.appendSlice(gpa, data);
    try installSimple(gpa, rule, key, .{ .bytes = list });
}

fn installRing(
    gpa: Allocator,
    rule: *rules_mod.Rule,
    key: []const u8,
    view: RingWireView,
) !void {
    const cap: usize = view.cap;
    const buf = try gpa.alloc(f64, cap);
    errdefer gpa.free(buf);
    for (0..cap) |i| {
        buf[i] = @bitCast(std.mem.readInt(u64, view.buf_bytes[i * 8 ..][0..8], .little));
    }

    var max_deque: std.ArrayList(u64) = .empty;
    errdefer max_deque.deinit(gpa);
    try max_deque.ensureTotalCapacity(gpa, view.max_bytes.len / 8);
    var mi: usize = 0;
    while (mi < view.max_bytes.len) : (mi += 8) {
        max_deque.appendAssumeCapacity(std.mem.readInt(u64, view.max_bytes[mi..][0..8], .little));
    }

    var min_deque: std.ArrayList(u64) = .empty;
    errdefer min_deque.deinit(gpa);
    try min_deque.ensureTotalCapacity(gpa, view.min_bytes.len / 8);
    var ni: usize = 0;
    while (ni < view.min_bytes.len) : (ni += 8) {
        min_deque.appendAssumeCapacity(std.mem.readInt(u64, view.min_bytes[ni..][0..8], .little));
    }

    try installSimple(gpa, rule, key, .{ .ring = .{
        .buf = buf,
        .counter = view.counter,
        .sum = view.sum,
        .max_deque = max_deque,
        .min_deque = min_deque,
    } });
}

fn installTimeRing(
    gpa: Allocator,
    rule: *rules_mod.Rule,
    key: []const u8,
    view: TimeRingWireView,
) !void {
    var samples: std.ArrayList(rules_mod.TimeRing.Sample) = .empty;
    errdefer samples.deinit(gpa);
    try samples.ensureTotalCapacity(gpa, view.n);
    var off: usize = 0;
    while (off < view.samples_bytes.len) : (off += 16) {
        const value: f64 = @bitCast(std.mem.readInt(u64, view.samples_bytes[off..][0..8], .little));
        const ts_ms: i64 = @bitCast(std.mem.readInt(u64, view.samples_bytes[off + 8 ..][0..8], .little));
        samples.appendAssumeCapacity(.{ .value = value, .ts_ms = ts_ms });
    }
    try installSimple(gpa, rule, key, .{ .time_ring = .{
        .samples = samples,
        .window_ms = view.window_ms,
    } });
}

// --- Atomic file write --------------------------------------------------

pub fn writeFileAtomic(
    gpa: Allocator,
    io: Io,
    path: []const u8,
    snap: Snapshot,
) !void {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(gpa);
    try dumpInto(gpa, &buf, snap);

    const cwd = Io.Dir.cwd();
    var atomic = try cwd.createFileAtomic(io, path, .{ .replace = true });
    defer atomic.deinit(io);

    var write_buf: [4096]u8 = undefined;
    var fw = atomic.file.writer(io, &write_buf);
    try fw.interface.writeAll(buf.items);
    try fw.interface.flush();
    try atomic.replace(io);
}

// --- Tests --------------------------------------------------------------

test {
    _ = @import("snapshot/tests.zig");
}
