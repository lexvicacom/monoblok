const std = @import("std");
const Allocator = std.mem.Allocator;

const patchbay = @import("patchbay");
const sexpr = patchbay.sexpr;
const subject_mod = patchbay.subject;
const router_mod = @import("router.zig");

pub const ConfigError = error{
    InvalidLvcForm,
} || sexpr.ParseError || subject_mod.Error || Allocator.Error;

/// Walk top-level forms looking for `(lvc ["filter" ...])` forms.
/// The older `(lvc "filter" ...)` spelling is still accepted.
/// Returns an empty slice when LVC is not configured.
pub fn loadFilters(arena: Allocator, source: []const u8) ConfigError![]const router_mod.Router.LvcFilter {
    const forms = try sexpr.parseAll(arena, source);
    var out: std.ArrayList(router_mod.Router.LvcFilter) = .empty;

    for (forms) |f| {
        if (f != .list or f.list.len == 0) continue;
        const head = f.list[0];
        if (head != .symbol or !std.mem.eql(u8, head.symbol, "lvc")) continue;
        if (f.list.len < 2) return error.InvalidLvcForm;

        const args = f.list[1..];
        if (args.len == 1 and (args[0] == .vector or args[0] == .list)) {
            const items = switch (args[0]) {
                .vector => |xs| xs,
                .list => |xs| xs,
                else => unreachable,
            };
            try appendFilters(arena, &out, items);
        } else {
            try appendFilters(arena, &out, args);
        }
    }

    return try out.toOwnedSlice(arena);
}

fn appendFilters(
    arena: Allocator,
    out: *std.ArrayList(router_mod.Router.LvcFilter),
    values: []const sexpr.Value,
) ConfigError!void {
    if (values.len == 0) return error.InvalidLvcForm;
    for (values) |v| {
        if (v != .string) return error.InvalidLvcForm;
        const filter = v.string;
        try subject_mod.validateFilter(filter);
        try out.append(arena, try router_mod.Router.LvcFilter.init(arena, filter));
    }
}

test "lvc config parses legacy vararg filters" {
    const testing = std.testing;
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const filters = try loadFilters(arena,
        \\(lvc "foo" "bar.>")
        \\(on "foo" (publish! "x" payload))
    );
    try testing.expectEqual(@as(usize, 2), filters.len);
    try testing.expectEqualStrings("foo", filters[0].filter);
    try testing.expect(filters[0].is_literal);
    try testing.expect(!filters[1].is_literal);
}

test "lvc config parses vector filters" {
    const testing = std.testing;
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const filters = try loadFilters(arena, "(lvc [\"foo\" \"bar.>\"])");
    try testing.expectEqual(@as(usize, 2), filters.len);
    try testing.expectEqualStrings("foo", filters[0].filter);
    try testing.expectEqualStrings("bar.>", filters[1].filter);
}

test "lvc config parses list filters" {
    const testing = std.testing;
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const filters = try loadFilters(arena, "(lvc (\"foo\" \"bar.>\"))");
    try testing.expectEqual(@as(usize, 2), filters.len);
    try testing.expectEqualStrings("foo", filters[0].filter);
    try testing.expectEqualStrings("bar.>", filters[1].filter);
}
