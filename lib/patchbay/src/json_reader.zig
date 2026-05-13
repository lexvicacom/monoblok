//! JSON reader for patchbay forms. This is deliberately a reader for the
//! existing `sexpr.Value` AST, not a separate JSON-shaped DSL.

const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");

const JsonValue = std.json.Value;

pub const Error = error{
    InvalidJsonPatchbay,
    InvalidJsonForm,
    InvalidJsonSymbol,
    InvalidJsonString,
    InvalidJsonKeyword,
    InvalidJsonVector,
};

/// Parse a JSON patchbay into the same top-level forms produced by
/// `sexpr.parseAll`.
///
/// Shape:
///   - top-level JSON value is an array of forms
///   - each form is an array whose first element is the operator string
///   - nested arrays in form arguments are call forms
///   - plain objects in form arguments expand to `:keyword value` pairs
///   - known bound names (`payload`, `payload-float`, `payload-int`,
///     `subject`) become symbols in expression positions
///   - `{"sym":"name"}` marks any other symbol outside operator position
///   - `{"str":"payload"}` forces a string literal when it matches a bound name
///   - `{"kw":"ms"}` marks a standalone keyword
///   - `{"vec":[...]}` marks a literal vector
pub fn parseAll(arena: Allocator, source: []const u8) ![]const sexpr.Value {
    const root = try std.json.parseFromSliceLeaky(JsonValue, arena, source, .{});
    if (root != .array) return error.InvalidJsonPatchbay;

    const items = root.array.items;
    if (items.len == 0) return &.{};

    // Permit a single bare form as a convenience:
    //   ["on", "foo", ["publish!", "bar", "payload"]]
    if (items[0] == .string) {
        const out = try arena.alloc(sexpr.Value, 1);
        out[0] = try convertFormArray(arena, root.array);
        return out;
    }

    const out = try arena.alloc(sexpr.Value, items.len);
    for (items, 0..) |item, i| {
        if (item != .array) return error.InvalidJsonPatchbay;
        out[i] = try convertFormArray(arena, item.array);
    }
    return out;
}

fn convertFormArray(arena: Allocator, array: std.json.Array) anyerror!sexpr.Value {
    if (array.items.len == 0 or array.items[0] != .string) return error.InvalidJsonForm;

    var out: std.ArrayList(sexpr.Value) = .empty;
    try out.append(arena, .{ .symbol = array.items[0].string });

    for (array.items[1..]) |arg| {
        if (try specialObject(arena, arg)) |v| {
            try out.append(arena, v);
        } else if (arg == .object) {
            try appendKeywordPairs(arena, &out, arg.object);
        } else {
            try out.append(arena, try convertExprValue(arena, arg));
        }
    }

    return .{ .list = try out.toOwnedSlice(arena) };
}

fn appendKeywordPairs(
    arena: Allocator,
    out: *std.ArrayList(sexpr.Value),
    object: std.json.ObjectMap,
) !void {
    var it = object.iterator();
    while (it.next()) |entry| {
        try out.append(arena, .{ .keyword = keywordName(entry.key_ptr.*) });
        try out.append(arena, try convertConfigValue(arena, entry.value_ptr.*));
    }
}

fn convertExprValue(arena: Allocator, v: JsonValue) anyerror!sexpr.Value {
    if (try specialObject(arena, v)) |special| return special;

    return switch (v) {
        .null => .nil,
        .bool => |b| .{ .boolean = b },
        .integer => |n| .{ .number = @floatFromInt(n) },
        .float => |n| .{ .number = n },
        .number_string => |s| .{ .number = try std.fmt.parseFloat(f64, s) },
        .string => |s| stringExprValue(s),
        .array => |a| try convertFormArray(arena, a),
        .object => error.InvalidJsonForm,
    };
}

fn convertConfigValue(arena: Allocator, v: JsonValue) anyerror!sexpr.Value {
    if (try specialObject(arena, v)) |special| return special;

    return switch (v) {
        .null => .nil,
        .bool => |b| .{ .boolean = b },
        .integer => |n| .{ .number = @floatFromInt(n) },
        .float => |n| .{ .number = n },
        .number_string => |s| .{ .number = try std.fmt.parseFloat(f64, s) },
        .string => |s| .{ .string = s },
        .array => |a| blk: {
            const out = try arena.alloc(sexpr.Value, a.items.len);
            for (a.items, 0..) |item, i| out[i] = try convertConfigValue(arena, item);
            break :blk .{ .vector = out };
        },
        .object => error.InvalidJsonForm,
    };
}

fn specialObject(arena: Allocator, v: JsonValue) anyerror!?sexpr.Value {
    if (v != .object) return null;
    const object = v.object;
    if (object.count() != 1) return null;

    if (object.get("sym")) |inner| {
        if (inner != .string) return error.InvalidJsonSymbol;
        return .{ .symbol = inner.string };
    }
    if (object.get("str")) |inner| {
        if (inner != .string) return error.InvalidJsonString;
        return .{ .string = inner.string };
    }
    if (object.get("kw")) |inner| {
        if (inner != .string) return error.InvalidJsonKeyword;
        return .{ .keyword = keywordName(inner.string) };
    }
    if (object.get("vec")) |inner| {
        if (inner != .array) return error.InvalidJsonVector;
        const out = try arena.alloc(sexpr.Value, inner.array.items.len);
        for (inner.array.items, 0..) |item, i| out[i] = try convertExprValue(arena, item);
        return .{ .vector = out };
    }
    return null;
}

fn keywordName(s: []const u8) []const u8 {
    return if (s.len > 0 and s[0] == ':') s[1..] else s;
}

fn stringExprValue(s: []const u8) sexpr.Value {
    if (isBoundSymbol(s)) return .{ .symbol = s };
    return .{ .string = s };
}

fn isBoundSymbol(s: []const u8) bool {
    return std.mem.eql(u8, s, "subject") or
        std.mem.eql(u8, s, "payload") or
        std.mem.eql(u8, s, "payload-float") or
        std.mem.eql(u8, s, "payload-int");
}

const testing = std.testing;

test "parse json patchbay form with inferred bound symbol and keyword object" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const forms = try parseAll(arena,
        \\[
        \\  ["on", "demo.sensors.*", {"reentrant": true},
        \\    ["bar!", 60, "payload-float"]]
        \\]
    );
    try testing.expectEqual(@as(usize, 1), forms.len);
    const top = forms[0].list;
    try testing.expectEqualStrings("on", top[0].symbol);
    try testing.expectEqualStrings("demo.sensors.*", top[1].string);
    try testing.expectEqualStrings("reentrant", top[2].keyword);
    try testing.expect(top[3].boolean);
    try testing.expectEqualStrings("bar!", top[4].list[0].symbol);
    try testing.expectEqualStrings("payload-float", top[4].list[2].symbol);
}

test "parse json can force string that matches a bound symbol" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const forms = try parseAll(arena,
        \\[["on", "demo.log", ["publish!", "demo.out", {"str": "payload"}]]]
    );
    const publish = forms[0].list[2].list;
    try testing.expectEqualStrings("payload", publish[2].string);
}

test "parse json config object as keyword pairs with vectors" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const forms = try parseAll(arena,
        \\[
        \\  ["bridge", {
        \\    "servers": ["nats://127.0.0.1:4223"],
        \\    "export": ["demo.alerts.>"]
        \\  }]
        \\]
    );
    const top = forms[0].list;
    try testing.expectEqualStrings("bridge", top[0].symbol);
    try testing.expectEqualStrings("servers", top[1].keyword);
    try testing.expect(top[2] == .vector);
    try testing.expectEqualStrings("nats://127.0.0.1:4223", top[2].vector[0].string);
    try testing.expectEqualStrings("export", top[3].keyword);
}
