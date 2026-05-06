const std = @import("std");
const Allocator = std.mem.Allocator;

const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;
const snapshot = @import("../snapshot.zig");
const testing = std.testing;

const LvcEntry = snapshot.LvcEntry;
const LoadError = snapshot.LoadError;
const RingWireView = snapshot.RingWireView;
const RuleStateEntry = snapshot.RuleStateEntry;
const TimeRingWireView = snapshot.TimeRingWireView;
const dumpInto = snapshot.dumpInto;
const header_len = snapshot.header_len;
const parse = snapshot.parse;

test "empty snapshot round-trips" {
    const gpa = testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try dumpInto(gpa, &out, .{});
    try testing.expectEqual(@as(usize, header_len), out.items.len);

    const Counter = struct {
        lvc: usize = 0,
        rs: usize = 0,
        pub fn onLvc(self: *@This(), _: []const u8, _: []const u8) !void {
            self.lvc += 1;
        }
        pub fn onRuleState(self: *@This(), _: u32, _: []const u8, _: []const u8, _: rules_mod.StateEntry) !void {
            self.rs += 1;
        }
        pub fn onRuleStateBytes(self: *@This(), _: u32, _: []const u8, _: []const u8, _: []const u8) !void {
            self.rs += 1;
        }
        pub fn onRuleStateRing(self: *@This(), _: u32, _: []const u8, _: []const u8, _: RingWireView) !void {
            self.rs += 1;
        }
        pub fn onRuleStateTimeRing(self: *@This(), _: u32, _: []const u8, _: []const u8, _: TimeRingWireView) !void {
            self.rs += 1;
        }
    };
    var c: Counter = .{};
    try parse(out.items, &c);
    try testing.expectEqual(@as(usize, 0), c.lvc);
    try testing.expectEqual(@as(usize, 0), c.rs);
}

test "lvc round-trip preserves bytes" {
    const gpa = testing.allocator;

    const lvc = [_]LvcEntry{
        .{ .subject = "sensor.temp", .payload = "21.5" },
        .{ .subject = "sensor.humidity", .payload = "" },
        .{ .subject = "x.y.z", .payload = "\x00\xff\x01\x02binary" },
    };

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try dumpInto(gpa, &out, .{ .lvc = &lvc });

    const Collector = struct {
        gpa: Allocator,
        got: std.ArrayList(LvcEntry) = .empty,
        pub fn onLvc(self: *@This(), subj: []const u8, payload: []const u8) !void {
            try self.got.append(self.gpa, .{
                .subject = try self.gpa.dupe(u8, subj),
                .payload = try self.gpa.dupe(u8, payload),
            });
        }
        pub fn onRuleState(_: *@This(), _: u32, _: []const u8, _: []const u8, _: rules_mod.StateEntry) !void {}
        pub fn onRuleStateBytes(_: *@This(), _: u32, _: []const u8, _: []const u8, _: []const u8) !void {}
        pub fn onRuleStateRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: RingWireView) !void {}
        pub fn onRuleStateTimeRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: TimeRingWireView) !void {}
    };
    var c: Collector = .{ .gpa = gpa };
    defer {
        for (c.got.items) |e| {
            gpa.free(e.subject);
            gpa.free(e.payload);
        }
        c.got.deinit(gpa);
    }
    try parse(out.items, &c);
    try testing.expectEqual(lvc.len, c.got.items.len);
    for (lvc, c.got.items) |in, got| {
        try testing.expectEqualStrings(in.subject, got.subject);
        try testing.expectEqualStrings(in.payload, got.payload);
    }
}

test "rule state variants round-trip" {
    const gpa = testing.allocator;

    var bytes_entry: std.ArrayList(u8) = .empty;
    defer bytes_entry.deinit(gpa);
    try bytes_entry.appendSlice(gpa, "42.0");

    const buf = try gpa.alloc(f64, 3);
    defer gpa.free(buf);
    buf[0] = 1.0;
    buf[1] = 2.5;
    buf[2] = 3.25;
    var max_deque: std.ArrayList(u64) = .empty;
    defer max_deque.deinit(gpa);
    try max_deque.append(gpa, 2);
    var min_deque: std.ArrayList(u64) = .empty;
    defer min_deque.deinit(gpa);
    try min_deque.append(gpa, 0);
    try min_deque.append(gpa, 1);

    const rs = [_]RuleStateEntry{
        .{ .rule_idx = 0, .filter = "a.>", .key = "empty_k", .value = .empty },
        .{ .rule_idx = 1, .filter = "b.*", .key = "squelch/n:x.y", .value = .{ .bytes = bytes_entry } },
        .{ .rule_idx = 2, .filter = "c", .key = "deadband/n:foo", .value = .{ .number = 3.14 } },
        .{ .rule_idx = 3, .filter = "d", .key = "moving-avg/t:q", .value = .{ .ring = .{
            .buf = buf,
            .counter = 7,
            .sum = 6.75,
            .max_deque = max_deque,
            .min_deque = min_deque,
        } } },
    };

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try dumpInto(gpa, &out, .{ .rule_state = &rs });

    const Collector = struct {
        gpa: Allocator,
        empties: usize = 0,
        got_bytes: ?[]u8 = null,
        got_number: ?f64 = null,
        got_ring: ?struct {
            cap: u32,
            counter: u64,
            sum: f64,
            buf: []f64,
            max: []u64,
            min: []u64,
        } = null,

        pub fn onLvc(_: *@This(), _: []const u8, _: []const u8) !void {}

        pub fn onRuleState(self: *@This(), _: u32, _: []const u8, _: []const u8, value: rules_mod.StateEntry) !void {
            switch (value) {
                .empty => self.empties += 1,
                .number => |n| self.got_number = n,
                else => unreachable,
            }
        }

        pub fn onRuleStateBytes(self: *@This(), _: u32, _: []const u8, _: []const u8, data: []const u8) !void {
            self.got_bytes = try self.gpa.dupe(u8, data);
        }

        pub fn onRuleStateRing(self: *@This(), _: u32, _: []const u8, _: []const u8, view: RingWireView) !void {
            const cap = view.cap;
            const b = try self.gpa.alloc(f64, cap);
            for (0..cap) |i| {
                b[i] = @bitCast(std.mem.readInt(u64, view.buf_bytes[i * 8 ..][0..8], .little));
            }
            const m = try self.gpa.alloc(u64, view.max_bytes.len / 8);
            for (0..m.len) |i| {
                m[i] = std.mem.readInt(u64, view.max_bytes[i * 8 ..][0..8], .little);
            }
            const mn = try self.gpa.alloc(u64, view.min_bytes.len / 8);
            for (0..mn.len) |i| {
                mn[i] = std.mem.readInt(u64, view.min_bytes[i * 8 ..][0..8], .little);
            }
            self.got_ring = .{
                .cap = cap,
                .counter = view.counter,
                .sum = view.sum,
                .buf = b,
                .max = m,
                .min = mn,
            };
        }
        pub fn onRuleStateTimeRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: TimeRingWireView) !void {}
    };
    var c: Collector = .{ .gpa = gpa };
    defer {
        if (c.got_bytes) |b| gpa.free(b);
        if (c.got_ring) |r| {
            gpa.free(r.buf);
            gpa.free(r.max);
            gpa.free(r.min);
        }
    }
    try parse(out.items, &c);
    try testing.expectEqual(@as(usize, 1), c.empties);
    try testing.expect(c.got_bytes != null);
    try testing.expectEqualStrings("42.0", c.got_bytes.?);
    try testing.expectEqual(@as(?f64, 3.14), c.got_number);
    try testing.expect(c.got_ring != null);
    try testing.expectEqual(@as(u32, 3), c.got_ring.?.cap);
    try testing.expectEqual(@as(u64, 7), c.got_ring.?.counter);
    try testing.expectEqual(@as(f64, 6.75), c.got_ring.?.sum);
    try testing.expectEqualSlices(f64, &.{ 1.0, 2.5, 3.25 }, c.got_ring.?.buf);
    try testing.expectEqualSlices(u64, &.{2}, c.got_ring.?.max);
    try testing.expectEqualSlices(u64, &.{ 0, 1 }, c.got_ring.?.min);
}

test "bad magic is rejected" {
    const bogus = "XXXX\x01\x00\x00\x00";
    const Noop = struct {
        pub fn onLvc(_: *@This(), _: []const u8, _: []const u8) !void {}
        pub fn onRuleState(_: *@This(), _: u32, _: []const u8, _: []const u8, _: rules_mod.StateEntry) !void {}
        pub fn onRuleStateBytes(_: *@This(), _: u32, _: []const u8, _: []const u8, _: []const u8) !void {}
        pub fn onRuleStateRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: RingWireView) !void {}
        pub fn onRuleStateTimeRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: TimeRingWireView) !void {}
    };
    var n: Noop = .{};
    try testing.expectError(LoadError.BadMagic, parse(bogus, &n));
}

test "unsupported version is rejected" {
    const bogus = "MBLK\xffAAA";
    const Noop = struct {
        pub fn onLvc(_: *@This(), _: []const u8, _: []const u8) !void {}
        pub fn onRuleState(_: *@This(), _: u32, _: []const u8, _: []const u8, _: rules_mod.StateEntry) !void {}
        pub fn onRuleStateBytes(_: *@This(), _: u32, _: []const u8, _: []const u8, _: []const u8) !void {}
        pub fn onRuleStateRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: RingWireView) !void {}
        pub fn onRuleStateTimeRing(_: *@This(), _: u32, _: []const u8, _: []const u8, _: TimeRingWireView) !void {}
    };
    var n: Noop = .{};
    try testing.expectError(LoadError.UnsupportedVersion, parse(bogus, &n));
}
