const std = @import("std");

pub const max_tokens = 32;
pub const max_len = 256;

pub const Error = error{
    Empty,
    TooLong,
    TooManyTokens,
    EmptyToken,
    InvalidCharacter,
    WildcardInPublish,
    TailWildcardNotLast,
};

pub fn validatePublish(s: []const u8) Error!void {
    try validate(s, .publish);
}

pub fn validateFilter(s: []const u8) Error!void {
    try validate(s, .filter);
}

const Kind = enum { publish, filter };

fn validate(s: []const u8, kind: Kind) Error!void {
    if (s.len == 0) return error.Empty;
    if (s.len > max_len) return error.TooLong;

    var tokens: usize = 0;
    var it = std.mem.splitScalar(u8, s, '.');
    var saw_tail = false;
    while (it.next()) |tok| {
        tokens += 1;
        if (tokens > max_tokens) return error.TooManyTokens;
        if (saw_tail) return error.TailWildcardNotLast;
        if (tok.len == 0) return error.EmptyToken;

        if (tok.len == 1) switch (tok[0]) {
            '*' => {
                if (kind == .publish) return error.WildcardInPublish;
                continue;
            },
            '>' => {
                if (kind == .publish) return error.WildcardInPublish;
                saw_tail = true;
                continue;
            },
            else => {},
        };

        for (tok) |c| switch (c) {
            'A'...'Z', 'a'...'z', '0'...'9', '-', '_', '$', ':' => {},
            else => return error.InvalidCharacter,
        };
    }
}

/// Returns true if `subject` (a concrete publish subject) matches `filter`
/// (which may contain `*` and a trailing `>`). Callers must have validated
/// both sides beforehand.
pub fn matches(filter: []const u8, subject: []const u8) bool {
    var f_it = std.mem.splitScalar(u8, filter, '.');
    var s_it = std.mem.splitScalar(u8, subject, '.');
    while (true) {
        const f = f_it.next();
        const s = s_it.next();
        if (f == null and s == null) return true;
        if (f == null or s == null) return false;
        const ft = f.?;
        const st = s.?;
        if (std.mem.eql(u8, ft, ">")) return true;
        if (std.mem.eql(u8, ft, "*")) continue;
        if (!std.mem.eql(u8, ft, st)) return false;
    }
}

/// Token-based matcher for subjects pre-split into token slices. Used when
/// we want to avoid re-splitting on every router fan-out.
pub fn matchesTokens(filter_tokens: []const []const u8, subject_tokens: []const []const u8) bool {
    var i: usize = 0;
    while (i < filter_tokens.len) : (i += 1) {
        const ft = filter_tokens[i];
        if (ft.len == 1 and ft[0] == '>') return i < subject_tokens.len;
        if (i >= subject_tokens.len) return false;
        if (ft.len == 1 and ft[0] == '*') continue;
        if (!std.mem.eql(u8, ft, subject_tokens[i])) return false;
    }
    return i == subject_tokens.len;
}

test "validate publish subjects" {
    try validatePublish("foo");
    try validatePublish("foo.bar.baz");
    try validatePublish("foo-1_2.bar");
    try validatePublish("T.O:AAPL250620C00200000");
    try std.testing.expectError(error.Empty, validatePublish(""));
    try std.testing.expectError(error.EmptyToken, validatePublish("foo..bar"));
    try std.testing.expectError(error.EmptyToken, validatePublish("foo."));
    try std.testing.expectError(error.WildcardInPublish, validatePublish("foo.*"));
    try std.testing.expectError(error.WildcardInPublish, validatePublish("foo.>"));
    try std.testing.expectError(error.InvalidCharacter, validatePublish("foo bar"));
}

test "validate filter subjects" {
    try validateFilter("foo.*.baz");
    try validateFilter("foo.>");
    try validateFilter(">");
    try std.testing.expectError(error.TailWildcardNotLast, validateFilter("foo.>.baz"));
}

test "matches: exact" {
    try std.testing.expect(matches("foo.bar", "foo.bar"));
    try std.testing.expect(!matches("foo.bar", "foo.baz"));
    try std.testing.expect(!matches("foo", "foo.bar"));
    try std.testing.expect(!matches("foo.bar", "foo"));
}

test "matches: single token wildcard" {
    try std.testing.expect(matches("foo.*", "foo.bar"));
    try std.testing.expect(!matches("foo.*", "foo.bar.baz"));
    try std.testing.expect(matches("*.*", "a.b"));
    try std.testing.expect(matches("foo.*.baz", "foo.bar.baz"));
}

test "matches: tail wildcard" {
    try std.testing.expect(matches("foo.>", "foo.bar"));
    try std.testing.expect(matches("foo.>", "foo.bar.baz.qux"));
    try std.testing.expect(!matches("foo.>", "foo"));
    try std.testing.expect(matches(">", "anything.here"));
}
