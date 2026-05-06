const std = @import("std");
const Allocator = std.mem.Allocator;

const router_mod = @import("../router.zig");
const Conn = router_mod.Conn;
const Router = router_mod.Router;

const testing = std.testing;

fn makeConn(gpa: Allocator, id: u64) !*Conn {
    const c = try gpa.create(Conn);
    c.* = .{ .id = id, .gpa = gpa };
    return c;
}

fn deliveredMsgCount(c: *Conn) usize {
    var n: usize = 0;
    var i: usize = 0;
    while (i < c.out.items.len) : (i += 1) {
        if (c.out.items[i] == '\n') n += 1;
    }
    // Each MSG occupies two lines (header + payload).
    return n / 2;
}

test "router: literal subs go in the literal bucket, wildcards in wildcard list" {
    var router: Router = .init(testing.allocator, true);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.bar", "1", null);
    try router.subscribe(a, "foo.*", "2", null);
    try router.subscribe(a, "baz.>", "3", null);
    try router.subscribe(a, "qux.zot", "4", null);

    try testing.expectEqual(@as(usize, 2), router.literal_subs.count());
    // foo.* and baz.> both have literal first tokens, so they go into
    // wildcard_buckets keyed by "foo" and "baz" respectively.
    try testing.expectEqual(@as(usize, 2), router.wildcard_buckets.count());
    try testing.expectEqual(@as(usize, 0), router.wildcard_global.items.len);
    try testing.expect(router.literal_subs.contains("foo.bar"));
    try testing.expect(router.literal_subs.contains("qux.zot"));
    try testing.expect(router.wildcard_buckets.contains("foo"));
    try testing.expect(router.wildcard_buckets.contains("baz"));
}

test "router: literal-only publish does not touch wildcard subs" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.bar", "1", null);
    try router.subscribe(b, "other.>", "2", null);

    try router.publish("foo.bar", null, "hi");

    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 0), deliveredMsgCount(b));
}

test "router: wildcard sub matches via wildcard path" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.*", "1", null);

    try router.publish("foo.bar", null, "x");
    try router.publish("foo.baz", null, "y");
    try router.publish("nope.bar", null, "z");

    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(a));
}

test "router: max_msgs auto-unsub drops sub from literal bucket" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.bar", "1", null);
    try router.unsubscribe(a, "1", 2);

    try router.publish("foo.bar", null, "x");
    try router.publish("foo.bar", null, "y");
    try router.publish("foo.bar", null, "z");

    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(a));
    // Bucket should be cleaned up after the second delivery removed the sub.
    try testing.expectEqual(@as(usize, 0), router.literal_subs.count());
}

test "router: max_msgs auto-unsub drops wildcard sub" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.*", "1", null);
    try router.unsubscribe(a, "1", 1);

    try router.publish("foo.bar", null, "x");
    try router.publish("foo.baz", null, "y");

    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 0), router.wildcard_buckets.count());
    try testing.expectEqual(@as(usize, 0), router.wildcard_global.items.len);
}

test "router: removeAllFor cleans up across both paths" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.bar", "1", null);
    try router.subscribe(a, "foo.*", "2", null);
    try router.subscribe(b, "foo.bar", "3", null);
    try router.subscribe(b, "wild.>", "4", null);

    a.markClosed();
    router.removeAllFor(a);
    a.deinit();

    try testing.expectEqual(@as(usize, 1), router.literal_subs.count());
    try testing.expectEqual(@as(usize, 1), router.wildcard_buckets.count());
    try testing.expect(router.wildcard_buckets.contains("wild"));

    try router.publish("foo.bar", null, "x");
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(b));
}

test "router: empty bucket is reclaimed when last sub leaves" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    try router.subscribe(a, "foo.bar", "1", null);
    try testing.expectEqual(@as(usize, 1), router.literal_subs.count());

    try router.unsubscribe(a, "1", null);
    try testing.expectEqual(@as(usize, 0), router.literal_subs.count());
}

test "router: bucket key survives owner sub leaving while others remain" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.bar", "1", null);
    try router.subscribe(b, "foo.bar", "2", null);

    // Drop the first sub, which contributed the bucket key initially.
    try router.unsubscribe(a, "1", null);

    // Bucket should still exist and still match publishes.
    try testing.expectEqual(@as(usize, 1), router.literal_subs.count());
    try router.publish("foo.bar", null, "x");
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(b));
}

test "router: queue group delivers to one of N round-robin" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();
    const c = try makeConn(testing.allocator, 3);
    defer c.deinit();

    try router.subscribe(a, "work.>", "1", "workers");
    try router.subscribe(b, "work.>", "2", "workers");
    try router.subscribe(c, "work.>", "3", "workers");

    try router.publish("work.a", null, "1");
    try router.publish("work.b", null, "2");
    try router.publish("work.c", null, "3");
    try router.publish("work.d", null, "4");
    try router.publish("work.e", null, "5");
    try router.publish("work.f", null, "6");

    // 6 messages spread across 3 group members, exactly one per message.
    const total = deliveredMsgCount(a) + deliveredMsgCount(b) + deliveredMsgCount(c);
    try testing.expectEqual(@as(usize, 6), total);
    // Round-robin: each conn should get exactly 2.
    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(b));
    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(c));
}

test "router: plain sub + queue group both deliver" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();
    const audit = try makeConn(testing.allocator, 3);
    defer audit.deinit();

    try router.subscribe(a, "evt.>", "1", "workers");
    try router.subscribe(b, "evt.>", "2", "workers");
    try router.subscribe(audit, "evt.>", "3", null); // plain, no group

    try router.publish("evt.x", null, "1");
    try router.publish("evt.y", null, "2");

    // Plain sub gets every message; group splits 2 messages across a/b.
    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(audit));
    try testing.expectEqual(@as(usize, 2), deliveredMsgCount(a) + deliveredMsgCount(b));
}

test "router: distinct queue groups each get one message" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();
    const c = try makeConn(testing.allocator, 3);
    defer c.deinit();
    const d = try makeConn(testing.allocator, 4);
    defer d.deinit();

    try router.subscribe(a, "evt.>", "1", "workers");
    try router.subscribe(b, "evt.>", "2", "workers");
    try router.subscribe(c, "evt.>", "3", "auditors");
    try router.subscribe(d, "evt.>", "4", "auditors");

    try router.publish("evt.x", null, "1");

    // Both groups get exactly one delivery for the same message.
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a) + deliveredMsgCount(b));
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(c) + deliveredMsgCount(d));
}

test "router: queue groups on different filters are independent" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "left.>", "1", "shared");
    try router.subscribe(b, "right.>", "2", "shared");

    try router.publish("left.x", null, "1");
    try router.publish("right.x", null, "2");

    // Same group name but distinct filters: each is its own pool.
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(b));
}

test "router: no-responders synthesizes 503 to reply when zero subs match" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const requestor = try makeConn(testing.allocator, 1);
    defer requestor.deinit();
    try router.subscribe(requestor, "_INBOX.xyz", "1", null);

    // Request to a subject with no subscribers, with no_responders=true.
    try router.publishRequest("svc.req", null, "ping", "_INBOX.xyz", true);

    // Requestor should have received an HMSG with NATS/1.0 503 to _INBOX.xyz.
    // (deliveredMsgCount counts plain MSG \n boundaries; HMSG framing is
    // different, so just check the bytes.)
    try testing.expect(std.mem.indexOf(u8, requestor.out.items, "HMSG _INBOX.xyz 1") != null);
    try testing.expect(std.mem.indexOf(u8, requestor.out.items, "NATS/1.0 503") != null);
}

test "router: no-responders does not fire when at least one sub matches" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const requestor = try makeConn(testing.allocator, 1);
    defer requestor.deinit();
    const responder = try makeConn(testing.allocator, 2);
    defer responder.deinit();

    try router.subscribe(requestor, "_INBOX.xyz", "1", null);
    try router.subscribe(responder, "svc.req", "2", null);

    try router.publishRequest("svc.req", null, "ping", "_INBOX.xyz", true);

    // Responder gets the request; requestor gets nothing yet (responder
    // hasn't replied), so no 503 synthesized.
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(responder));
    try testing.expectEqual(@as(usize, 0), deliveredMsgCount(requestor));
}

test "router: reply subject is forwarded to subscribers" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const responder = try makeConn(testing.allocator, 1);
    defer responder.deinit();
    try router.subscribe(responder, "svc.req", "7", null);

    // Plain request (no NoResponders header) carrying a reply subject.
    try router.publishRequest("svc.req", null, "ping", "_INBOX.abc", false);

    // The 4-token MSG form is `MSG <subject> <sid> <reply> <bytes>`.
    try testing.expect(std.mem.indexOf(u8, responder.out.items, "MSG svc.req 7 _INBOX.abc 4\r\nping\r\n") != null);
}

test "router: wildcard subscribers also receive reply subject" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const responder = try makeConn(testing.allocator, 1);
    defer responder.deinit();
    try router.subscribe(responder, "svc.*", "9", null);

    try router.publishRequest("svc.req", null, "ping", "_INBOX.xyz", false);

    try testing.expect(std.mem.indexOf(u8, responder.out.items, "MSG svc.req 9 _INBOX.xyz 4\r\nping\r\n") != null);
}

test "router: queue-group subscriber receives reply subject" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const responder = try makeConn(testing.allocator, 1);
    defer responder.deinit();
    try router.subscribe(responder, "svc.req", "3", "workers");

    try router.publishRequest("svc.req", null, "ping", "_INBOX.q", false);

    try testing.expect(std.mem.indexOf(u8, responder.out.items, "MSG svc.req 3 _INBOX.q 4\r\nping\r\n") != null);
}

test "router: LVC replay does not carry reply subject" {
    var router: Router = .init(testing.allocator, true);
    defer router.deinit();

    // Seed the LVC, then subscribe to $LVC.foo — the cached value should be
    // delivered as plain MSG (no reply).
    try router.publish("foo", null, "cached");

    const sub = try makeConn(testing.allocator, 1);
    defer sub.deinit();
    try router.subscribe(sub, "$LVC.foo", "11", null);

    // 3-token form, no reply field.
    try testing.expect(std.mem.indexOf(u8, sub.out.items, "MSG $LVC.foo 11 6\r\ncached\r\n") != null);
}

test "router: configured LVC filters gate cache and live stream" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const filters = [_]Router.LvcFilter{
        try Router.LvcFilter.init(arena, "hot.>"),
    };
    var router: Router = .init(testing.allocator, true);
    defer router.deinit();
    try router.configureLvc(&filters);

    try router.publish("hot.one", null, "cached");
    try router.publish("cold.one", null, "hidden");

    const sub = try makeConn(testing.allocator, 1);
    defer sub.deinit();
    try router.subscribe(sub, "$LVC.>", "8", null);
    try testing.expect(std.mem.indexOf(u8, sub.out.items, "MSG $LVC.hot.one 8 6\r\ncached\r\n") != null);
    try testing.expect(std.mem.indexOf(u8, sub.out.items, "cold.one") == null);

    sub.out.clearRetainingCapacity();
    try router.publish("cold.two", null, "live-hidden");
    try router.publish("hot.two", null, "live");
    try testing.expect(std.mem.indexOf(u8, sub.out.items, "cold.two") == null);
    try testing.expect(std.mem.indexOf(u8, sub.out.items, "MSG $LVC.hot.two 8 4\r\nlive\r\n") != null);
}

test "router: no-responders without reply does nothing" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();
    // No subs at all. With reply=null, the synth-503 has nowhere to go and
    // must not panic / loop.
    try router.publishRequest("svc.req", null, "ping", null, true);
}

test "router: removing one group member reroutes to remaining" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "work.>", "1", "g");
    try router.subscribe(b, "work.>", "2", "g");

    try router.publish("work.x", null, "1");
    try router.publish("work.y", null, "2");

    // Each got 1 so far.
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(b));

    try router.unsubscribe(a, "1", null);

    try router.publish("work.z", null, "3");
    try router.publish("work.w", null, "4");

    // a is gone; b gets all subsequent messages.
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 3), deliveredMsgCount(b));
}

test "router: wildcard with leading wildcard token goes to global list" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();

    // First-token wildcards must live in wildcard_global since they could
    // match any first-token bucket.
    try router.subscribe(a, "*.foo", "1", null);
    try router.subscribe(a, ">", "2", null);

    try testing.expectEqual(@as(usize, 0), router.wildcard_buckets.count());
    try testing.expectEqual(@as(usize, 2), router.wildcard_global.items.len);
}

test "router: bucketed wildcard scan skips other buckets" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();
    const b = try makeConn(testing.allocator, 2);
    defer b.deinit();

    try router.subscribe(a, "foo.*", "1", null);
    try router.subscribe(b, "bar.*", "2", null);

    // Publish on foo.x: only `a` should match. Confirms the bar bucket
    // wasn't even scanned (functionally; perf gain is implicit).
    try router.publish("foo.x", null, "hi");
    try testing.expectEqual(@as(usize, 1), deliveredMsgCount(a));
    try testing.expectEqual(@as(usize, 0), deliveredMsgCount(b));
}

test "router: leading-wildcard sub matches subjects from any bucket" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();

    try router.subscribe(a, "*.x", "1", null);

    try router.publish("foo.x", null, "1");
    try router.publish("bar.x", null, "2");
    try router.publish("baz.x", null, "3");
    try router.publish("foo.y", null, "4");

    // Three matches across three first-token buckets, plus one miss.
    try testing.expectEqual(@as(usize, 3), deliveredMsgCount(a));
}

test "router: empty wildcard bucket is reclaimed when last sub leaves" {
    var router: Router = .init(testing.allocator, false);
    defer router.deinit();

    const a = try makeConn(testing.allocator, 1);
    defer a.deinit();

    try router.subscribe(a, "foo.*", "1", null);
    try testing.expectEqual(@as(usize, 1), router.wildcard_buckets.count());

    try router.unsubscribe(a, "1", null);
    try testing.expectEqual(@as(usize, 0), router.wildcard_buckets.count());
}
