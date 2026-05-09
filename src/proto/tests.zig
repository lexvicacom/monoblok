const std = @import("std");

const proto = @import("../proto.zig");
const testing = std.testing;

const ClientOp = proto.ClientOp;
const max_control_line = proto.max_control_line;
const max_payload = proto.max_payload;
const parseClientOp = proto.parseClientOp;
const parseConnect = proto.parseConnect;
const parseServerOp = proto.parseServerOp;
const writeHmsg = proto.writeHmsg;
const writeMsg = proto.writeMsg;

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
    try testing.expectEqual(proto.ServerMsgKind.msg, r.op.msg.kind);
    try testing.expectEqualStrings("foo", r.op.msg.subject);
    try testing.expectEqualStrings("7", r.op.msg.sid);
    try testing.expectEqualStrings(" 5\r\nhello\r\n", r.op.msg.suffix_after_sid);
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

test "writeServerMsgWithSid preserves suffix" {
    const r = try parseServerOp("MSG foo 7 reply.box 3\r\nhey\r\n");
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    try proto.writeServerMsgWithSid(testing.allocator, &buf, r.op.msg.kind, r.op.msg.subject, "client-42", r.op.msg.suffix_after_sid);
    try testing.expectEqualStrings("MSG foo client-42 reply.box 3\r\nhey\r\n", buf.items);
}

test "parseServerMsgEnvelope MSG with reply" {
    const r = try proto.parseServerMsgEnvelope("MSG foo 7 reply.box 3\r\nhey\r\n");
    try testing.expectEqual(proto.ServerMsgKind.msg, r.kind);
    try testing.expectEqualStrings("foo", r.subject);
    try testing.expectEqualStrings("7", r.sid);
    try testing.expectEqualStrings(" reply.box 3\r\nhey\r\n", r.suffix_after_sid);
    try testing.expectEqual(@as(usize, "MSG foo 7 reply.box 3\r\nhey\r\n".len), r.consumed);
}

test "parseServerMsgEnvelope ignores non-message ops" {
    try testing.expectError(error.UnknownOp, proto.parseServerMsgEnvelope("PING\r\n"));
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
    try testing.expectEqual(proto.ServerMsgKind.hmsg, r.op.msg.kind);
    try testing.expectEqualStrings("foo", r.op.msg.subject);
    try testing.expectEqualStrings("1", r.op.msg.sid);
    try testing.expectEqualStrings(" rep 18 23\r\nNATS/1.0\r\nX: y\r\n\r\nhello\r\n", r.op.msg.suffix_after_sid);
    try testing.expectEqualStrings("rep", r.op.msg.reply.?);
    try testing.expectEqualStrings(headers, r.op.msg.headers.?);
    try testing.expectEqualStrings("hello", r.op.msg.payload);
}

test "parseServerMsgEnvelope HMSG with reply" {
    var wire: std.ArrayList(u8) = .empty;
    defer wire.deinit(testing.allocator);
    const headers = "NATS/1.0\r\nX: y\r\n\r\n";
    try writeHmsg(testing.allocator, &wire, "foo", "1", "rep", headers, "hello");
    const r = try proto.parseServerMsgEnvelope(wire.items);
    try testing.expectEqual(proto.ServerMsgKind.hmsg, r.kind);
    try testing.expectEqualStrings("foo", r.subject);
    try testing.expectEqualStrings("1", r.sid);
    try testing.expectEqualStrings(" rep 18 23\r\nNATS/1.0\r\nX: y\r\n\r\nhello\r\n", r.suffix_after_sid);
    try testing.expectEqual(wire.items.len, r.consumed);
}
