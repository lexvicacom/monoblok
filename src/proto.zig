//! NATS wire protocol parser and serializer.
//!
//! The parser takes a byte slice (a per-connection buffered read buffer) and
//! returns either a complete ClientOp + number of bytes consumed, or
//! `error.NeedMoreData` to signal the caller should wait for more bytes
//! before calling again. The serializer appends directly into a caller-owned
//! ArrayList(u8), which maps onto the per-connection outbound buffer.
const std = @import("std");
const builtin = @import("builtin");
const Allocator = std.mem.Allocator;

const package_version = @import("manifest").version;

pub const max_control_line = 4096;
pub const max_payload = 1 * 1024 * 1024;

pub const ParseError = error{
    NeedMoreData,
    MalformedOp,
    UnknownOp,
    InvalidArgs,
    PayloadTooLarge,
    ControlLineTooLong,
};

pub const ClientOp = union(enum) {
    connect: []const u8, // raw JSON body, unused for now
    pub_msg: Pub,
    sub: Sub,
    unsub: Unsub,
    ping,
    pong,

    pub const Pub = struct {
        subject: []const u8,
        reply: ?[]const u8,
        payload: []const u8,
    };
    pub const Sub = struct {
        subject: []const u8,
        queue: ?[]const u8,
        sid: []const u8,
    };
    pub const Unsub = struct {
        sid: []const u8,
        max_msgs: ?u64,
    };
};

pub const ParseResult = struct {
    op: ClientOp,
    consumed: usize,
};

/// Attempt to parse one client op from `buf`. Returned slices alias into
/// `buf`; the caller must consume or copy before shifting / overwriting the
/// buffer.
pub fn parseClientOp(buf: []const u8) ParseError!ParseResult {
    const nl = std.mem.indexOfScalar(u8, buf, '\n') orelse {
        if (buf.len > max_control_line) return error.ControlLineTooLong;
        return error.NeedMoreData;
    };
    if (nl > max_control_line) return error.ControlLineTooLong;

    const line_end = nl + 1; // include the LF
    const line = buf[0..nl];
    const trimmed = std.mem.trimEnd(u8, line, "\r");
    if (trimmed.len == 0) return error.MalformedOp;

    const verb_end = std.mem.indexOfAny(u8, trimmed, " \t") orelse trimmed.len;
    const verb = trimmed[0..verb_end];
    const rest_raw = if (verb_end == trimmed.len) trimmed[0..0] else trimmed[verb_end + 1 ..];
    const rest = std.mem.trim(u8, rest_raw, " \t");

    if (eqIgnoreCase(verb, "PING")) return .{ .op = .ping, .consumed = line_end };
    if (eqIgnoreCase(verb, "PONG")) return .{ .op = .pong, .consumed = line_end };
    if (eqIgnoreCase(verb, "CONNECT")) return .{ .op = .{ .connect = rest }, .consumed = line_end };
    if (eqIgnoreCase(verb, "SUB")) return .{ .op = try parseSub(rest), .consumed = line_end };
    if (eqIgnoreCase(verb, "UNSUB")) return .{ .op = try parseUnsub(rest), .consumed = line_end };
    if (eqIgnoreCase(verb, "PUB")) return parsePub(buf, line_end, rest);
    return error.UnknownOp;
}

fn parseSub(rest: []const u8) ParseError!ClientOp {
    var it = std.mem.tokenizeAny(u8, rest, " \t");
    const subject = it.next() orelse return error.InvalidArgs;
    const a = it.next() orelse return error.InvalidArgs;
    const b = it.next();
    if (it.next() != null) return error.InvalidArgs;
    if (b) |sid| return .{ .sub = .{ .subject = subject, .queue = a, .sid = sid } };
    return .{ .sub = .{ .subject = subject, .queue = null, .sid = a } };
}

fn parseUnsub(rest: []const u8) ParseError!ClientOp {
    var it = std.mem.tokenizeAny(u8, rest, " \t");
    const sid = it.next() orelse return error.InvalidArgs;
    const max_str = it.next();
    if (it.next() != null) return error.InvalidArgs;
    const max_msgs = if (max_str) |m|
        std.fmt.parseInt(u64, m, 10) catch return error.InvalidArgs
    else
        null;
    return .{ .unsub = .{ .sid = sid, .max_msgs = max_msgs } };
}

fn parsePub(buf: []const u8, line_end: usize, rest: []const u8) ParseError!ParseResult {
    var it = std.mem.tokenizeAny(u8, rest, " \t");
    const subject = it.next() orelse return error.InvalidArgs;
    const a = it.next() orelse return error.InvalidArgs;
    const b = it.next();
    if (it.next() != null) return error.InvalidArgs;

    var reply: ?[]const u8 = null;
    const nbytes_str = blk: {
        if (b) |last| {
            reply = a;
            break :blk last;
        }
        break :blk a;
    };
    const nbytes = std.fmt.parseInt(usize, nbytes_str, 10) catch return error.InvalidArgs;
    if (nbytes > max_payload) return error.PayloadTooLarge;

    // Need payload bytes + trailing CRLF (or at least LF — we're lenient).
    const after_header = buf[line_end..];
    if (after_header.len < nbytes + 1) return error.NeedMoreData;
    const payload = after_header[0..nbytes];
    const tail = after_header[nbytes..];
    const trailer_len: usize = switch (tail[0]) {
        '\r' => if (tail.len < 2) return error.NeedMoreData
                else if (tail[1] == '\n') 2
                else return error.MalformedOp,
        '\n' => 1,
        else => return error.MalformedOp,
    };

    return .{
        .op = .{ .pub_msg = .{ .subject = subject, .reply = reply, .payload = payload } },
        .consumed = line_end + nbytes + trailer_len,
    };
}

fn eqIgnoreCase(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| if (std.ascii.toUpper(x) != std.ascii.toUpper(y)) return false;
    return true;
}

// --- Server-to-client writers -------------------------------------------
// These append into a caller-owned ArrayList(u8) (the per-conn outbound buffer).

pub fn writeInfo(
    gpa: Allocator,
    out: *std.ArrayList(u8),
    server_id: []const u8,
    client_id: u64,
    host: []const u8,
    port: u16,
) !void {
    try out.print(gpa,
        "INFO {{" ++
            "\"server_id\":\"{s}\"," ++
            "\"server_name\":\"{s}\"," ++
            "\"version\":\"{s}\"," ++
            "\"zig_version\":\"{s}\"," ++
            "\"proto\":1," ++
            "\"host\":\"{s}\"," ++
            "\"port\":{d}," ++
            "\"max_payload\":{d}," ++
            "\"client_id\":{d}," ++
            "\"headers\":false," ++
            "\"auth_required\":false," ++
            "\"tls_required\":false" ++
        "}}\r\n",
        .{ server_id, server_id, package_version, builtin.zig_version_string, host, port, max_payload, client_id },
    );
}

pub fn writeErr(gpa: Allocator, out: *std.ArrayList(u8), msg: []const u8) !void {
    try out.print(gpa, "-ERR '{s}'\r\n", .{msg});
}

pub fn writePong(gpa: Allocator, out: *std.ArrayList(u8)) !void {
    try out.appendSlice(gpa, "PONG\r\n");
}

pub fn writeMsg(
    gpa: Allocator,
    out: *std.ArrayList(u8),
    subject: []const u8,
    sid: []const u8,
    reply: ?[]const u8,
    payload: []const u8,
) !void {
    if (reply) |r| {
        try out.print(gpa, "MSG {s} {s} {s} {d}\r\n", .{ subject, sid, r, payload.len });
    } else {
        try out.print(gpa, "MSG {s} {s} {d}\r\n", .{ subject, sid, payload.len });
    }
    try out.appendSlice(gpa, payload);
    try out.appendSlice(gpa, "\r\n");
}

// --- Tests ---------------------------------------------------------------

const testing = std.testing;

test "parse PING and PONG" {
    const buf = "PING\r\nPONG\r\n";
    const r1 = try parseClientOp(buf);
    try testing.expectEqual(ClientOp.ping, r1.op);
    try testing.expectEqual(@as(usize, 6), r1.consumed);
    const r2 = try parseClientOp(buf[r1.consumed..]);
    try testing.expectEqual(ClientOp.pong, r2.op);
    try testing.expectEqual(@as(usize, 6), r2.consumed);
}

test "parse CONNECT ignores body" {
    const r = try parseClientOp("CONNECT {\"verbose\":false}\r\n");
    try testing.expect(r.op == .connect);
    try testing.expectEqualStrings("{\"verbose\":false}", r.op.connect);
}

test "parse SUB without queue" {
    const r = try parseClientOp("SUB foo.bar 1\r\n");
    try testing.expectEqualStrings("foo.bar", r.op.sub.subject);
    try testing.expectEqual(@as(?[]const u8, null), r.op.sub.queue);
    try testing.expectEqualStrings("1", r.op.sub.sid);
}

test "parse SUB with queue" {
    const r = try parseClientOp("SUB foo.bar Q1 42\r\n");
    try testing.expectEqualStrings("foo.bar", r.op.sub.subject);
    try testing.expectEqualStrings("Q1", r.op.sub.queue.?);
    try testing.expectEqualStrings("42", r.op.sub.sid);
}

test "parse UNSUB with max" {
    const r = try parseClientOp("UNSUB 1 5\r\n");
    try testing.expectEqualStrings("1", r.op.unsub.sid);
    try testing.expectEqual(@as(u64, 5), r.op.unsub.max_msgs.?);
}

test "parse PUB without reply" {
    const r = try parseClientOp("PUB foo 5\r\nhello\r\n");
    try testing.expectEqualStrings("foo", r.op.pub_msg.subject);
    try testing.expectEqual(@as(?[]const u8, null), r.op.pub_msg.reply);
    try testing.expectEqualStrings("hello", r.op.pub_msg.payload);
    try testing.expectEqual(@as(usize, 18), r.consumed);
}

test "parse PUB with reply" {
    const r = try parseClientOp("PUB foo bar 3\r\nhey\r\n");
    try testing.expectEqualStrings("foo", r.op.pub_msg.subject);
    try testing.expectEqualStrings("bar", r.op.pub_msg.reply.?);
    try testing.expectEqualStrings("hey", r.op.pub_msg.payload);
}

test "parse PUB partial payload returns NeedMoreData" {
    try testing.expectError(error.NeedMoreData, parseClientOp("PUB foo 5\r\nhel"));
}

test "parse PUB without full header returns NeedMoreData" {
    try testing.expectError(error.NeedMoreData, parseClientOp("PUB foo"));
}

test "parse unknown verb" {
    try testing.expectError(error.UnknownOp, parseClientOp("NOPE\r\n"));
}

test "case insensitive verbs" {
    const r1 = try parseClientOp("ping\r\n");
    try testing.expectEqual(ClientOp.ping, r1.op);
    const r2 = try parseClientOp("Sub foo 1\r\n");
    try testing.expectEqualStrings("foo", r2.op.sub.subject);
}

test "writeMsg basic" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    try writeMsg(testing.allocator, &buf, "foo", "1", null, "hi");
    try testing.expectEqualStrings("MSG foo 1 2\r\nhi\r\n", buf.items);
}

test "writeMsg with reply" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    try writeMsg(testing.allocator, &buf, "foo", "1", "reply.box", "hi");
    try testing.expectEqualStrings("MSG foo 1 reply.box 2\r\nhi\r\n", buf.items);
}
