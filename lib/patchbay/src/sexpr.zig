const std = @import("std");
const Allocator = std.mem.Allocator;

pub const Value = union(enum) {
    nil,
    boolean: bool,
    number: f64,
    symbol: []const u8,
    keyword: []const u8, // EDN-style :foo (used in config forms like (bridge :servers [...]))
    string: []const u8,
    list: []const Value,

    pub fn isTruthy(v: Value) bool {
        return switch (v) {
            .nil => false,
            .boolean => |b| b,
            else => true,
        };
    }
};

pub const ParseError = error{
    UnexpectedEof,
    UnexpectedRParen,
    UnterminatedString,
    InvalidEscape,
    InvalidNumber,
} || Allocator.Error;

/// Parse a full source text into a slice of top-level expressions.
/// All returned Values reference memory owned by `arena`.
pub fn parseAll(arena: Allocator, source: []const u8) ParseError![]const Value {
    var discard: usize = 0;
    return parseAllReporting(arena, source, &discard);
}

/// Same as `parseAll` but writes the byte offset of a parse failure into
/// `err_offset` when the error is one of `ParseError`'s parser variants
/// (i.e., not an OutOfMemory). Callers turn that offset into line:col for
/// human-readable diagnostics. On success `err_offset` is left untouched.
pub fn parseAllReporting(arena: Allocator, source: []const u8, err_offset: *usize) ParseError![]const Value {
    var p: Parser = .{ .src = source, .pos = 0, .arena = arena };
    var list: std.ArrayList(Value) = .empty;
    while (true) {
        p.skipWhitespaceAndComments();
        if (p.pos >= p.src.len) break;
        const v = p.parseOne() catch |err| {
            err_offset.* = p.pos;
            return err;
        };
        try list.append(arena, v);
    }
    return try list.toOwnedSlice(arena);
}

const Parser = struct {
    src: []const u8,
    pos: usize,
    arena: Allocator,

    fn skipWhitespaceAndComments(p: *Parser) void {
        while (p.pos < p.src.len) {
            const c = p.src[p.pos];
            if (c == ' ' or c == '\t' or c == '\r' or c == '\n') {
                p.pos += 1;
            } else if (c == ';') {
                while (p.pos < p.src.len and p.src[p.pos] != '\n') : (p.pos += 1) {}
            } else break;
        }
    }

    fn parseOne(p: *Parser) ParseError!Value {
        p.skipWhitespaceAndComments();
        if (p.pos >= p.src.len) return error.UnexpectedEof;
        const c = p.src[p.pos];
        return switch (c) {
            '(' => p.parseList(),
            ')' => error.UnexpectedRParen,
            '"' => p.parseString(),
            else => p.parseAtom(),
        };
    }

    fn parseList(p: *Parser) ParseError!Value {
        p.pos += 1; // consume '('
        var items: std.ArrayList(Value) = .empty;
        while (true) {
            p.skipWhitespaceAndComments();
            if (p.pos >= p.src.len) {
                // Diagnostic points at EOF, not the unclosed '('; the latter
                // reads as "this token is wrong" for multi-line forms.
                return error.UnexpectedEof;
            }
            if (p.src[p.pos] == ')') {
                p.pos += 1;
                return .{ .list = try items.toOwnedSlice(p.arena) };
            }
            const v = try p.parseOne();
            try items.append(p.arena, v);
        }
    }

    fn parseString(p: *Parser) ParseError!Value {
        const open_pos = p.pos;
        p.pos += 1; // consume opening quote
        var out: std.ArrayList(u8) = .empty;
        while (p.pos < p.src.len) {
            const c = p.src[p.pos];
            if (c == '"') {
                p.pos += 1;
                return .{ .string = try out.toOwnedSlice(p.arena) };
            }
            if (c == '\\') {
                p.pos += 1;
                if (p.pos >= p.src.len) {
                    p.pos = open_pos;
                    return error.UnterminatedString;
                }
                const esc = p.src[p.pos];
                const decoded: u8 = switch (esc) {
                    'n' => '\n',
                    'r' => '\r',
                    't' => '\t',
                    '\\' => '\\',
                    '"' => '"',
                    else => return error.InvalidEscape,
                };
                try out.append(p.arena, decoded);
                p.pos += 1;
                continue;
            }
            try out.append(p.arena, c);
            p.pos += 1;
        }
        p.pos = open_pos;
        return error.UnterminatedString;
    }

    fn parseAtom(p: *Parser) ParseError!Value {
        const start = p.pos;
        while (p.pos < p.src.len) {
            const c = p.src[p.pos];
            if (c == ' ' or c == '\t' or c == '\r' or c == '\n' or c == '(' or c == ')' or c == ';') break;
            p.pos += 1;
        }
        const tok = p.src[start..p.pos];
        if (tok.len == 0) return error.UnexpectedEof;

        if (std.mem.eql(u8, tok, "nil")) return .nil;
        if (std.mem.eql(u8, tok, "true")) return .{ .boolean = true };
        if (std.mem.eql(u8, tok, "false")) return .{ .boolean = false };

        if (parseNumberToken(tok)) |n| return .{ .number = n };
        if (tok[0] == ':' and tok.len > 1) return .{ .keyword = tok[1..] };
        return .{ .symbol = tok };
    }
};

fn parseNumberToken(tok: []const u8) ?f64 {
    if (tok.len == 0) return null;
    const first = tok[0];
    const looks_numeric = (first >= '0' and first <= '9') or
        ((first == '-' or first == '+') and tok.len > 1 and tok[1] >= '0' and tok[1] <= '9') or
        first == '.';
    if (!looks_numeric) return null;
    return std.fmt.parseFloat(f64, tok) catch null;
}

// --- Tests ---------------------------------------------------------------

const testing = std.testing;

test "parse atoms" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const vs = try parseAll(arena, "42 -3.5 foo \"bar\" true false nil");
    try testing.expectEqual(@as(usize, 7), vs.len);
    try testing.expectEqual(@as(f64, 42), vs[0].number);
    try testing.expectEqual(@as(f64, -3.5), vs[1].number);
    try testing.expectEqualStrings("foo", vs[2].symbol);
    try testing.expectEqualStrings("bar", vs[3].string);
    try testing.expect(vs[4].boolean);
    try testing.expect(!vs[5].boolean);
    try testing.expectEqual(Value.nil, vs[6]);
}

test "parse keywords" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const vs = try parseAll(arena, "(bridge :servers \"a\" :tls true)");
    const top = vs[0].list;
    try testing.expectEqualStrings("bridge", top[0].symbol);
    try testing.expectEqualStrings("servers", top[1].keyword);
    try testing.expectEqualStrings("a", top[2].string);
    try testing.expectEqualStrings("tls", top[3].keyword);
    try testing.expect(top[4].boolean);
}

test "parse nested list" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const vs = try parseAll(arena, "(on \"foo.*\" (publish (subject-append \"hi\") payload))");
    try testing.expectEqual(@as(usize, 1), vs.len);
    const top = vs[0].list;
    try testing.expectEqualStrings("on", top[0].symbol);
    try testing.expectEqualStrings("foo.*", top[1].string);
    const body = top[2].list;
    try testing.expectEqualStrings("publish", body[0].symbol);
    const arg0 = body[1].list;
    try testing.expectEqualStrings("subject-append", arg0[0].symbol);
    try testing.expectEqualStrings("hi", arg0[1].string);
    try testing.expectEqualStrings("payload", body[2].symbol);
}

test "comments are ignored" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const vs = try parseAll(arena,
        \\; header comment
        \\(a b) ; inline
        \\; trailing
    );
    try testing.expectEqual(@as(usize, 1), vs.len);
    try testing.expectEqual(@as(usize, 2), vs[0].list.len);
}

test "string escapes" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const vs = try parseAll(arena, "\"a\\nb\\\"c\"");
    try testing.expectEqualStrings("a\nb\"c", vs[0].string);
}

test "unbalanced paren errors" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    try testing.expectError(error.UnexpectedEof, parseAll(arena, "(a b"));
    try testing.expectError(error.UnexpectedRParen, parseAll(arena, ")"));
}

test "missing rparen reports offset at end-of-input, not at unclosed paren" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src = "(on \"foo\"\n  (publish bar baz)\n";
    var off: usize = 0;
    try testing.expectError(error.UnexpectedEof, parseAllReporting(arena, src, &off));
    try testing.expectEqual(src.len, off);
}
