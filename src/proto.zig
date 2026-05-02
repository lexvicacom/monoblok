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
        /// Optional NATS protocol-v1 header chunk (HPUB). When non-null, the
        /// bytes start with `NATS/1.0\r\n` and end with `\r\n\r\n`. The server
        /// treats this as opaque (forward to subscribers, do not interpret),
        /// except for the one signal it acts on: `no_responders`.
        headers: ?[]const u8 = null,
        /// Set when headers contain `Nats-Request-No-Responders: true`. Parsed
        /// at the wire boundary so the router can short-circuit empty-fanout
        /// requests without re-scanning the header bytes per publish.
        no_responders: bool = false,
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

/// Server-to-client frames the mixer needs to interpret coming back from
/// a worker. Slices alias into the input buffer.
pub const ServerOp = union(enum) {
    info: []const u8, // raw JSON body
    msg: Msg,
    pong,
    ping,
    ok,
    err: []const u8,

    pub const Msg = struct {
        subject: []const u8,
        sid: []const u8,
        reply: ?[]const u8,
        /// Optional NATS protocol-v1 header chunk (HMSG). Same shape and
        /// opacity rules as `Pub.headers`.
        headers: ?[]const u8 = null,
        payload: []const u8,
    };
};

pub const ServerParseResult = struct {
    op: ServerOp,
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
    if (eqIgnoreCase(verb, "HPUB")) return parseHpub(buf, line_end, rest);
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

    // Need payload bytes + trailing CRLF (or at least LF; we're lenient).
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

/// HPUB <subject> [reply] <hdr_len> <total_len>\r\n<headers><payload>\r\n
/// `headers` is opaque to us (the v1 protocol's NATS/1.0\r\n...\r\n\r\n chunk);
/// `payload` is the bytes after the header chunk. Both are slices into `buf`.
fn parseHpub(buf: []const u8, line_end: usize, rest: []const u8) ParseError!ParseResult {
    var it = std.mem.tokenizeAny(u8, rest, " \t");
    const subject = it.next() orelse return error.InvalidArgs;
    const a = it.next() orelse return error.InvalidArgs;
    const b = it.next() orelse return error.InvalidArgs;
    const c = it.next();
    if (it.next() != null) return error.InvalidArgs;

    var reply: ?[]const u8 = null;
    var hdr_str: []const u8 = undefined;
    var total_str: []const u8 = undefined;
    if (c) |last| {
        reply = a;
        hdr_str = b;
        total_str = last;
    } else {
        hdr_str = a;
        total_str = b;
    }
    const hdr_len = std.fmt.parseInt(usize, hdr_str, 10) catch return error.InvalidArgs;
    const total_len = std.fmt.parseInt(usize, total_str, 10) catch return error.InvalidArgs;
    if (hdr_len > total_len) return error.InvalidArgs;
    if (total_len > max_payload) return error.PayloadTooLarge;

    const after_header = buf[line_end..];
    if (after_header.len < total_len + 1) return error.NeedMoreData;
    const headers = after_header[0..hdr_len];
    const payload = after_header[hdr_len..total_len];
    const tail = after_header[total_len..];
    const trailer_len: usize = switch (tail[0]) {
        '\r' => if (tail.len < 2) return error.NeedMoreData
                else if (tail[1] == '\n') 2
                else return error.MalformedOp,
        '\n' => 1,
        else => return error.MalformedOp,
    };

    const no_responders = headerHasNoResponders(headers);
    return .{
        .op = .{ .pub_msg = .{
            .subject = subject,
            .reply = reply,
            .headers = headers,
            .no_responders = no_responders,
            .payload = payload,
        } },
        .consumed = line_end + total_len + trailer_len,
    };
}

/// Single-pass scan for `Nats-Request-No-Responders: true`. The header chunk
/// is small in practice (a few lines), so a substring search beats a full
/// line-by-line parse. Match is ASCII-case-sensitive on the canonical name
/// real clients send; the value just needs to start with `t` (true vs false).
fn headerHasNoResponders(headers: []const u8) bool {
    const needle = "Nats-Request-No-Responders:";
    var i: usize = 0;
    while (std.mem.indexOfPos(u8, headers, i, needle)) |at| {
        // Confirm line-anchored: previous byte is start-of-buffer or LF.
        if (at == 0 or headers[at - 1] == '\n') {
            var j = at + needle.len;
            while (j < headers.len and (headers[j] == ' ' or headers[j] == '\t')) : (j += 1) {}
            if (j < headers.len and (headers[j] == 't' or headers[j] == 'T')) return true;
            return false;
        }
        i = at + 1;
    }
    return false;
}

/// Parse one server-to-client op from `buf`. Symmetric with parseClientOp.
pub fn parseServerOp(buf: []const u8) ParseError!ServerParseResult {
    const nl = std.mem.indexOfScalar(u8, buf, '\n') orelse {
        if (buf.len > max_control_line) return error.ControlLineTooLong;
        return error.NeedMoreData;
    };
    if (nl > max_control_line) return error.ControlLineTooLong;

    const line_end = nl + 1;
    const line = buf[0..nl];
    const trimmed = std.mem.trimEnd(u8, line, "\r");
    if (trimmed.len == 0) return error.MalformedOp;

    const verb_end = std.mem.indexOfAny(u8, trimmed, " \t") orelse trimmed.len;
    const verb = trimmed[0..verb_end];
    const rest_raw = if (verb_end == trimmed.len) trimmed[0..0] else trimmed[verb_end + 1 ..];
    const rest = std.mem.trim(u8, rest_raw, " \t");

    if (eqIgnoreCase(verb, "PING")) return .{ .op = .ping, .consumed = line_end };
    if (eqIgnoreCase(verb, "PONG")) return .{ .op = .pong, .consumed = line_end };
    if (eqIgnoreCase(verb, "INFO")) return .{ .op = .{ .info = rest }, .consumed = line_end };
    if (eqIgnoreCase(verb, "+OK")) return .{ .op = .ok, .consumed = line_end };
    if (std.mem.startsWith(u8, trimmed, "-ERR")) {
        return .{ .op = .{ .err = rest }, .consumed = line_end };
    }
    if (eqIgnoreCase(verb, "MSG")) return parseMsg(buf, line_end, rest);
    if (eqIgnoreCase(verb, "HMSG")) return parseHmsg(buf, line_end, rest);
    return error.UnknownOp;
}

fn parseMsg(buf: []const u8, line_end: usize, rest: []const u8) ParseError!ServerParseResult {
    var it = std.mem.tokenizeAny(u8, rest, " \t");
    const subject = it.next() orelse return error.InvalidArgs;
    const sid = it.next() orelse return error.InvalidArgs;
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
        .op = .{ .msg = .{ .subject = subject, .sid = sid, .reply = reply, .payload = payload } },
        .consumed = line_end + nbytes + trailer_len,
    };
}

/// HMSG <subject> <sid> [reply] <hdr_len> <total_len>\r\n<headers><payload>\r\n
fn parseHmsg(buf: []const u8, line_end: usize, rest: []const u8) ParseError!ServerParseResult {
    var it = std.mem.tokenizeAny(u8, rest, " \t");
    const subject = it.next() orelse return error.InvalidArgs;
    const sid = it.next() orelse return error.InvalidArgs;
    const a = it.next() orelse return error.InvalidArgs;
    const b = it.next() orelse return error.InvalidArgs;
    const c = it.next();
    if (it.next() != null) return error.InvalidArgs;

    var reply: ?[]const u8 = null;
    var hdr_str: []const u8 = undefined;
    var total_str: []const u8 = undefined;
    if (c) |last| {
        reply = a;
        hdr_str = b;
        total_str = last;
    } else {
        hdr_str = a;
        total_str = b;
    }
    const hdr_len = std.fmt.parseInt(usize, hdr_str, 10) catch return error.InvalidArgs;
    const total_len = std.fmt.parseInt(usize, total_str, 10) catch return error.InvalidArgs;
    if (hdr_len > total_len) return error.InvalidArgs;
    if (total_len > max_payload) return error.PayloadTooLarge;

    const after_header = buf[line_end..];
    if (after_header.len < total_len + 1) return error.NeedMoreData;
    const headers = after_header[0..hdr_len];
    const payload = after_header[hdr_len..total_len];
    const tail = after_header[total_len..];
    const trailer_len: usize = switch (tail[0]) {
        '\r' => if (tail.len < 2) return error.NeedMoreData
                else if (tail[1] == '\n') 2
                else return error.MalformedOp,
        '\n' => 1,
        else => return error.MalformedOp,
    };

    return .{
        .op = .{ .msg = .{ .subject = subject, .sid = sid, .reply = reply, .headers = headers, .payload = payload } },
        .consumed = line_end + total_len + trailer_len,
    };
}

/// Parsed subset of the CONNECT JSON body. Fields default to false / null when
/// absent. Unknown fields are ignored.
pub const ConnectOptions = struct {
    verbose: bool = false,
    pedantic: bool = false,
};

/// Parse a CONNECT JSON body. Returns defaults on parse error or non-object
/// bodies. Allocator is for the scanner's depth stack; an arena is fine.
pub fn parseConnect(gpa: Allocator, body: []const u8) ConnectOptions {
    var scanner = std.json.Scanner.initCompleteInput(gpa, body);
    defer scanner.deinit();

    var opts: ConnectOptions = .{};
    if ((scanner.next() catch return opts) != .object_begin) return opts;
    while (true) {
        const tok = scanner.next() catch return opts;
        switch (tok) {
            .object_end => break,
            .string => |key| {
                const val = scanner.next() catch return opts;
                if (std.mem.eql(u8, key, "verbose")) {
                    opts.verbose = (val == .true);
                } else if (std.mem.eql(u8, key, "pedantic")) {
                    opts.pedantic = (val == .true);
                } else {
                    // Unknown key: skip its value. Scanner already consumed
                    // a primitive; for nested objects/arrays, keep skipping
                    // until depth returns to zero.
                    var depth: i32 = switch (val) {
                        .object_begin, .array_begin => 1,
                        else => 0,
                    };
                    while (depth > 0) {
                        const inner = scanner.next() catch return opts;
                        switch (inner) {
                            .object_begin, .array_begin => depth += 1,
                            .object_end, .array_end => depth -= 1,
                            else => {},
                        }
                    }
                }
            },
            else => return opts,
        }
    }
    return opts;
}

fn eqIgnoreCase(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| if (std.ascii.toUpper(x) != std.ascii.toUpper(y)) return false;
    return true;
}

// --- Server-to-client writers -------------------------------------------
// These append into a caller-owned ArrayList(u8) (the per-conn outbound buffer).

/// Process role advertised in INFO. Lets clients (and humans tcpdumping the
/// wire) tell whether they're talking to the routing mixer or a backend
/// worker. Daemons report .standalone.
pub const Mode = enum { standalone, mixer, worker };

pub fn writeInfo(
    gpa: Allocator,
    out: *std.ArrayList(u8),
    server_id: []const u8,
    client_id: u64,
    host: []const u8,
    port: u16,
    mode: Mode,
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
            "\"monoblok_mode\":\"{s}\"," ++
            "\"headers\":true," ++
            "\"auth_required\":false," ++
            "\"tls_required\":false" ++
        "}}\r\n",
        .{ server_id, server_id, package_version, builtin.zig_version_string, host, port, max_payload, client_id, @tagName(mode) },
    );
}

pub fn writeErr(gpa: Allocator, out: *std.ArrayList(u8), msg: []const u8) !void {
    try out.print(gpa, "-ERR '{s}'\r\n", .{msg});
}

pub fn writePong(gpa: Allocator, out: *std.ArrayList(u8)) !void {
    try out.appendSlice(gpa, "PONG\r\n");
}

pub fn writePing(gpa: Allocator, out: *std.ArrayList(u8)) !void {
    try out.appendSlice(gpa, "PING\r\n");
}

pub fn writeOk(gpa: Allocator, out: *std.ArrayList(u8)) !void {
    try out.appendSlice(gpa, "+OK\r\n");
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

/// HMSG <subject> <sid> [reply] <hdr_len> <total_len>\r\n<headers><payload>\r\n
/// `headers` is the opaque header chunk (NATS/1.0\r\n...\r\n\r\n) emitted by
/// the original publisher; we forward it byte-for-byte.
pub fn writeHmsg(
    gpa: Allocator,
    out: *std.ArrayList(u8),
    subject: []const u8,
    sid: []const u8,
    reply: ?[]const u8,
    headers: []const u8,
    payload: []const u8,
) !void {
    const total = headers.len + payload.len;
    if (reply) |r| {
        try out.print(gpa, "HMSG {s} {s} {s} {d} {d}\r\n", .{ subject, sid, r, headers.len, total });
    } else {
        try out.print(gpa, "HMSG {s} {s} {d} {d}\r\n", .{ subject, sid, headers.len, total });
    }
    try out.appendSlice(gpa, headers);
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

test "parse rejects oversized control line" {
    // No LF before the cap, so the parser must give up rather than wait
    // forever for a line that legally cannot exist.
    var buf: [max_control_line + 16]u8 = undefined;
    @memset(&buf, 'A');
    try testing.expectError(error.ControlLineTooLong, parseClientOp(&buf));
}

test "parse rejects empty control line" {
    try testing.expectError(error.MalformedOp, parseClientOp("\r\n"));
}

test "parse PUB rejects oversized payload length" {
    var line_buf: [64]u8 = undefined;
    const line = try std.fmt.bufPrint(&line_buf, "PUB foo {d}\r\n", .{max_payload + 1});
    try testing.expectError(error.PayloadTooLarge, parseClientOp(line));
}

test "parse PUB rejects garbage trailer after payload" {
    // Length header says 5 bytes, payload is "hello", but the trailer is
    // "XX" instead of the expected CRLF / LF.
    try testing.expectError(error.MalformedOp, parseClientOp("PUB foo 5\r\nhelloXX"));
}

test "parse PUB rejects non-numeric length" {
    try testing.expectError(error.InvalidArgs, parseClientOp("PUB foo abc\r\n"));
}

test "parse SUB rejects extra trailing args" {
    try testing.expectError(error.InvalidArgs, parseClientOp("SUB foo Q1 42 extra\r\n"));
}

test "parse UNSUB rejects non-numeric max" {
    try testing.expectError(error.InvalidArgs, parseClientOp("UNSUB 1 lots\r\n"));
}

test "parseConnect picks up verbose + pedantic" {
    const o = parseConnect(testing.allocator, "{\"verbose\":true,\"pedantic\":false,\"name\":\"nats-cli\"}");
    try testing.expect(o.verbose);
    try testing.expect(!o.pedantic);
}

test "parseConnect tolerates empty body" {
    const o = parseConnect(testing.allocator, "");
    try testing.expect(!o.verbose);
}

test "parseConnect skips unknown nested objects" {
    const o = parseConnect(testing.allocator, "{\"jwt\":{\"x\":1},\"verbose\":true}");
    try testing.expect(o.verbose);
}

test "writeMsg basic" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    try writeMsg(testing.allocator, &buf, "foo", "1", null, "hi");
    try testing.expectEqualStrings("MSG foo 1 2\r\nhi\r\n", buf.items);
}

test "parseServerOp INFO" {
    const r = try parseServerOp("INFO {\"server_id\":\"x\"}\r\n");
    try testing.expectEqualStrings("{\"server_id\":\"x\"}", r.op.info);
}

test "parseServerOp MSG without reply" {
    const r = try parseServerOp("MSG foo 7 5\r\nhello\r\n");
    try testing.expectEqualStrings("foo", r.op.msg.subject);
    try testing.expectEqualStrings("7", r.op.msg.sid);
    try testing.expectEqual(@as(?[]const u8, null), r.op.msg.reply);
    try testing.expectEqualStrings("hello", r.op.msg.payload);
}

test "parseServerOp MSG with reply" {
    const r = try parseServerOp("MSG foo 7 reply.box 3\r\nhey\r\n");
    try testing.expectEqualStrings("foo", r.op.msg.subject);
    try testing.expectEqualStrings("reply.box", r.op.msg.reply.?);
    try testing.expectEqualStrings("hey", r.op.msg.payload);
}

test "parseServerOp -ERR" {
    const r = try parseServerOp("-ERR 'bad'\r\n");
    try testing.expectEqualStrings("'bad'", r.op.err);
}

test "parseServerOp partial MSG payload returns NeedMoreData" {
    try testing.expectError(error.NeedMoreData, parseServerOp("MSG foo 7 5\r\nhel"));
}

test "writeMsg with reply" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    try writeMsg(testing.allocator, &buf, "foo", "1", "reply.box", "hi");
    try testing.expectEqualStrings("MSG foo 1 reply.box 2\r\nhi\r\n", buf.items);
}

test "parse HPUB without reply" {
    // HPUB foo <hdr_len> <total_len>\r\n<headers><payload>\r\n
    const headers = "NATS/1.0\r\nFoo: bar\r\n\r\n";
    const payload = "hello";
    var msg: [128]u8 = undefined;
    const wire = try std.fmt.bufPrint(&msg, "HPUB foo {d} {d}\r\n{s}{s}\r\n", .{ headers.len, headers.len + payload.len, headers, payload });
    const r = try parseClientOp(wire);
    try testing.expectEqualStrings("foo", r.op.pub_msg.subject);
    try testing.expectEqual(@as(?[]const u8, null), r.op.pub_msg.reply);
    try testing.expectEqualStrings(headers, r.op.pub_msg.headers.?);
    try testing.expectEqualStrings(payload, r.op.pub_msg.payload);
}

test "parse HPUB with reply" {
    const headers = "NATS/1.0\r\nKey: val\r\n\r\n";
    const payload = "hi";
    var msg: [128]u8 = undefined;
    const wire = try std.fmt.bufPrint(&msg, "HPUB foo reply.x {d} {d}\r\n{s}{s}\r\n", .{ headers.len, headers.len + payload.len, headers, payload });
    const r = try parseClientOp(wire);
    try testing.expectEqualStrings("foo", r.op.pub_msg.subject);
    try testing.expectEqualStrings("reply.x", r.op.pub_msg.reply.?);
    try testing.expectEqualStrings(headers, r.op.pub_msg.headers.?);
    try testing.expectEqualStrings(payload, r.op.pub_msg.payload);
}

test "parse HPUB with empty payload" {
    const headers = "NATS/1.0\r\n\r\n";
    var msg: [128]u8 = undefined;
    const wire = try std.fmt.bufPrint(&msg, "HPUB foo {d} {d}\r\n{s}\r\n", .{ headers.len, headers.len, headers });
    const r = try parseClientOp(wire);
    try testing.expectEqualStrings(headers, r.op.pub_msg.headers.?);
    try testing.expectEqualStrings("", r.op.pub_msg.payload);
}

test "parse HPUB hdr_len > total_len rejected" {
    try testing.expectError(error.InvalidArgs, parseClientOp("HPUB foo 10 5\r\nshort\r\n"));
}

test "parse HPUB detects Nats-Request-No-Responders" {
    const headers = "NATS/1.0\r\nNats-Request-No-Responders: true\r\n\r\n";
    var msg: [256]u8 = undefined;
    const wire = try std.fmt.bufPrint(&msg, "HPUB foo {d} {d}\r\n{s}\r\n", .{ headers.len, headers.len, headers });
    const r = try parseClientOp(wire);
    try testing.expect(r.op.pub_msg.no_responders);
}

test "parse HPUB no_responders default false" {
    const headers = "NATS/1.0\r\nNats-Request-No-Responders: false\r\n\r\n";
    var msg: [256]u8 = undefined;
    const wire = try std.fmt.bufPrint(&msg, "HPUB foo {d} {d}\r\n{s}\r\n", .{ headers.len, headers.len, headers });
    const r = try parseClientOp(wire);
    try testing.expect(!r.op.pub_msg.no_responders);
}

test "parse HPUB no_responders ignores prefix-match in header value" {
    // A header whose *value* contains the needle should not trigger.
    const headers = "NATS/1.0\r\nX-Note: Nats-Request-No-Responders: true is a feature\r\n\r\n";
    var msg: [256]u8 = undefined;
    const wire = try std.fmt.bufPrint(&msg, "HPUB foo {d} {d}\r\n{s}\r\n", .{ headers.len, headers.len, headers });
    const r = try parseClientOp(wire);
    try testing.expect(!r.op.pub_msg.no_responders);
}

test "writeHmsg basic" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    const headers = "NATS/1.0\r\nA: 1\r\n\r\n";
    try writeHmsg(testing.allocator, &buf, "foo", "7", null, headers, "hi");
    var expected: [128]u8 = undefined;
    const want = try std.fmt.bufPrint(&expected, "HMSG foo 7 {d} {d}\r\n{s}hi\r\n", .{ headers.len, headers.len + 2, headers });
    try testing.expectEqualStrings(want, buf.items);
}

test "parseServerOp HMSG round-trip" {
    var wire: std.ArrayList(u8) = .empty;
    defer wire.deinit(testing.allocator);
    const headers = "NATS/1.0\r\nX: y\r\n\r\n";
    try writeHmsg(testing.allocator, &wire, "foo", "1", "rep", headers, "hello");
    const r = try parseServerOp(wire.items);
    try testing.expectEqualStrings("foo", r.op.msg.subject);
    try testing.expectEqualStrings("1", r.op.msg.sid);
    try testing.expectEqualStrings("rep", r.op.msg.reply.?);
    try testing.expectEqualStrings(headers, r.op.msg.headers.?);
    try testing.expectEqualStrings("hello", r.op.msg.payload);
}
