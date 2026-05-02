//! In-process microbench for Router.publish on wildcard fan-out.
//!
//! Measures pure routing cost (no TCP, no parser, no syscalls) so a
//! routing-table change shows up cleanly. Subscribers are real Conn
//! objects with a no-op kick callback; their out buffers fill but nothing
//! drains them, so each run is bounded by the sub count + msg count.
//!
//! Run with: `zig build bench-router -- [MODE] [N] [M] [PUBS]`
//!   MODE  "buc" (default) puts subs in first-token buckets; "lin" forces
//!         every sub through wildcard_global (the leading-wildcard path)
//!         so we can measure the bucketing win head-to-head.
//!   N     total wildcard subs (default 200)
//!   M     distinct first-token namespaces (default 20)
//!   PUBS  publishes per run (default 1_000_000)
//!
//! `buc` subscribes `ns0.*, ns1.*, …` (literal first token → bucket).
//! `lin` subscribes `*.k0, *.k1, …` (leading wildcard → global list)
//! with the second token shared across N/M subs so the same N/M subs
//! actually match each ns0.kX publish. Same matching cost, same fan-out
//! cost; only the pre-match scan differs. Run both modes back-to-back.

const std = @import("std");
const router_mod = @import("router.zig");

const Router = router_mod.Router;
const Conn = router_mod.Conn;

fn noopKick(_: *anyopaque) void {}

/// Monotonic ns. `std.time.nanoTimestamp` was removed in Zig 0.16.
fn monoNs() i128 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
    return @as(i128, ts.sec) * std.time.ns_per_s + @as(i128, ts.nsec);
}

fn makeConn(gpa: std.mem.Allocator, id: u64) !*Conn {
    const c = try gpa.create(Conn);
    c.* = .{ .id = id, .gpa = gpa };
    // Install a no-op kick so deliveries don't NPE looking for a callback.
    c.kick_ctx = c;
    c.kick_fn = noopKick;
    return c;
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;

    var it = try init.minimal.args.iterateAllocator(gpa);
    defer it.deinit();
    _ = it.skip(); // exe name

    const mode_arg: []const u8 = it.next() orelse "buc";
    const mode: enum { buc, lin } =
        if (std.mem.eql(u8, mode_arg, "buc")) .buc
        else if (std.mem.eql(u8, mode_arg, "lin")) .lin
        else {
            std.debug.print("bad mode '{s}' (want 'buc' or 'lin')\n", .{mode_arg});
            return error.BadArgs;
        };
    const n: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 200;
    const m: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 20;
    const pubs: usize = if (it.next()) |s| try std.fmt.parseInt(usize, s, 10) else 1_000_000;

    if (m == 0 or n == 0) {
        std.debug.print("N and M must be > 0\n", .{});
        return error.BadArgs;
    }

    var router: Router = .init(gpa, false);
    defer router.deinit();

    // One conn per sub (so deliveries don't dedup kicks). Buffers grow as
    // payloads pile up but never drain, so keep payloads short.
    var conns = try gpa.alloc(*Conn, n);
    defer gpa.free(conns);
    for (0..n) |i| conns[i] = try makeConn(gpa, @intCast(i));
    defer for (conns) |c| {
        c.markClosed();
        router.removeAllFor(c);
        c.deinit();
    };

    // Subscribe N wildcard filters with the same matching cardinality in
    // both modes (N/M subs match each publish), but different first-token
    // shape so they land in different router data structures.
    //   buc: nsX.*   (X = i%M, literal first token → wildcard_buckets[ns0])
    //   lin: *.tagX  (X = i%M, leading wildcard   → wildcard_global)
    var sid_buf: [32]u8 = undefined;
    var filter_buf: [64]u8 = undefined;
    for (0..n) |i| {
        const slot = i % m;
        const filter = switch (mode) {
            .buc => try std.fmt.bufPrint(&filter_buf, "ns{d}.*", .{slot}),
            .lin => try std.fmt.bufPrint(&filter_buf, "*.tag{d}", .{slot}),
        };
        const sid = try std.fmt.bufPrint(&sid_buf, "{d}", .{i});
        try router.subscribe(conns[i], filter, sid, null);
    }

    // Warm up: a few hundred publishes to get the alloc paths hot. Match
    // subset is the same shape as the timed loop so caches warm correctly.
    var subj_buf: [64]u8 = undefined;
    for (0..1000) |i| {
        const subj = switch (mode) {
            .buc => try std.fmt.bufPrint(&subj_buf, "ns0.k{d}", .{i % 100}),
            .lin => try std.fmt.bufPrint(&subj_buf, "ns0.tag0", .{}),
        };
        try router.publish(subj, null, "x");
    }

    // Now reset every sub conn's out buffer so we don't measure realloc
    // of an unbounded growing buffer (this isn't the hot path under
    // realistic load — clients drain).
    for (conns) |c| c.out.clearRetainingCapacity();

    // Timed loop. Both modes target a publish that matches exactly N/M
    // subs. Difference is what the router has to scan to find them.
    //   buc → scan wildcard_buckets["ns0"] (N/M entries)
    //   lin → scan wildcard_global (N entries, N/M pass matchesTokens)
    const start = monoNs();
    for (0..pubs) |i| {
        const subj = switch (mode) {
            .buc => try std.fmt.bufPrint(&subj_buf, "ns0.k{d}", .{i % 1024}),
            // Constant subject keeps lin's matching set identical per pub.
            .lin => try std.fmt.bufPrint(&subj_buf, "ns0.tag0", .{}),
        };
        try router.publish(subj, null, "x");
        // Periodically drain so out buffers don't blow up.
        if (i % 4096 == 0) for (conns) |c| c.out.clearRetainingCapacity();
    }
    const elapsed_ns = monoNs() - start;

    const elapsed_s: f64 = @as(f64, @floatFromInt(elapsed_ns)) / 1e9;
    const rate: f64 = @as(f64, @floatFromInt(pubs)) / elapsed_s;
    const matching_subs = n / m;

    std.debug.print(
        "mode={s} N={d} M={d} matching/pub={d} | {d} pubs in {d:.3}s = {d:.0} pubs/s\n",
        .{ @tagName(mode), n, m, matching_subs, pubs, elapsed_s, rate },
    );
}
