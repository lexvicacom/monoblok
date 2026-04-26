const std = @import("std");
const builtin = @import("builtin");
const Io = std.Io;
const xev = @import("xev");

pub const subject = @import("subject.zig");
pub const proto = @import("proto.zig");
pub const sexpr = @import("sexpr.zig");
pub const rules = @import("rules.zig");
pub const router = @import("router.zig");
pub const server = @import("server.zig");
pub const snapshot = @import("snapshot.zig");
pub const bridge = if (build_options.bridge) @import("bridge.zig") else {};

const manifest = @import("manifest");
const build_options = @import("build_options");

const Flag = enum { port, patchbay, no_lvc, stats, trace, snapshot, snapshot_every, help, version };

const flag_map = std.StaticStringMap(Flag).initComptime(.{
    .{ "--port", .port },
    .{ "--patchbay", .patchbay },
    // `--rules` is kept as a silent alias so existing scripts don't break.
    .{ "--rules", .patchbay },
    .{ "--no-lvc", .no_lvc },
    .{ "--stats", .stats },
    .{ "--trace", .trace },
    .{ "--snapshot", .snapshot },
    .{ "--snapshot-every", .snapshot_every },
    .{ "--help", .help },
    .{ "-h", .help },
    .{ "--version", .version },
    .{ "-V", .version },
});

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const arena = init.arena.allocator();
    const fsio = init.io; // Only used for loading the patchbay file at startup.

    var port: u16 = 4222;
    var patchbay_path: ?[]const u8 = null;
    var lvc_enabled = true;
    var stats_enabled = false;
    var trace_enabled = false;
    var snapshot_path: ?[]const u8 = null;
    var snapshot_every_s: u32 = 0;

    var it = try init.minimal.args.iterateAllocator(gpa);
    defer it.deinit();
    _ = it.skip();
    while (it.next()) |a| {
        const flag = flag_map.get(a) orelse fatal("unknown argument");
        switch (flag) {
            .port => {
                const v = it.next() orelse fatal("--port requires a value");
                port = std.fmt.parseInt(u16, v, 10) catch fatal("invalid port");
            },
            .patchbay => patchbay_path = it.next() orelse fatal("--patchbay requires a path"),
            .no_lvc => lvc_enabled = false,
            .stats => stats_enabled = true,
            .trace => trace_enabled = true,
            .snapshot => snapshot_path = it.next() orelse fatal("--snapshot requires a path"),
            .snapshot_every => {
                const v = it.next() orelse fatal("--snapshot-every requires a value in seconds");
                snapshot_every_s = std.fmt.parseInt(u32, v, 10) catch fatal("invalid --snapshot-every value");
            },
            .help => {
                printUsage();
                return;
            },
            .version => {
                printVersion();
                return;
            },
        }
    }

    const patchbay_src: ?[]u8 = if (patchbay_path) |path| try readFile(fsio, arena, path) else null;

    const loaded_rules: []rules.Rule = blk: {
        if (patchbay_src) |src| break :blk try rules.loadRules(arena, src);
        break :blk &.{};
    };
    defer rules.deinitRules(loaded_rules, gpa);
    const ruleset = try rules.buildRuleSet(arena, loaded_rules);
    std.log.info("loaded {d} patchbay form(s)", .{loaded_rules.len});
    std.log.info("libxev backend: {s} (os={s})", .{ @tagName(xev.backend), @tagName(builtin.os.tag) });
    std.log.info("lvc: {s}", .{if (lvc_enabled) "enabled" else "disabled"});
    if (trace_enabled) {
        std.log.warn("--trace enabled: every patchbay evaluation will be printed to stderr (noisy and slow, do not run in production)", .{});
    }

    var loop = try xev.Loop.init(.{});
    defer loop.deinit();

    var r = router.Router.init(gpa, lvc_enabled);
    defer r.deinit();

    // Snapshot: optional LVC warm-start. If `--snapshot PATH` is given and
    // the file exists, populate the cache before we accept any connections
    // so the first `SUB $LVC.*` sees the restored state.
    if (snapshot_path) |sp| {
        if (!lvc_enabled) {
            std.log.warn("snapshot: --snapshot ignored because --no-lvc is set", .{});
        } else if (readFile(fsio, arena, sp)) |bytes| {
            snapshot.loadInto(gpa, bytes, &r, loaded_rules) catch |err| {
                std.log.warn("snapshot: load failed ({s}): {s}", .{ sp, @errorName(err) });
            };
            var rs_total: usize = 0;
            for (loaded_rules) |*rule| rs_total += rule.state.count();
            std.log.info(
                "snapshot: loaded {d} lvc / {d} rule-state entries from {s}",
                .{ r.last_value.count(), rs_total, sp },
            );
        } else |err| switch (err) {
            error.FileNotFound => std.log.info("snapshot: {s} not found, starting empty", .{sp}),
            else => std.log.warn("snapshot: read failed ({s}): {s}", .{ sp, @errorName(err) }),
        }
    }

    // Bridge: optional outbound NATS connection. Parsed from the patchbay
    // file's top-level `(bridge :servers [...] :export [...] ...)` form.
    var bridge_runtime: if (build_options.bridge) ?bridge.Bridge else ?void = null;
    if (build_options.bridge) {
        if (patchbay_src) |src| {
            const cfg_opt = bridge.loadConfig(arena, src) catch |err| blk: {
                std.log.warn("bridge: config parse failed: {s}", .{@errorName(err)});
                break :blk null;
            };
            if (cfg_opt) |cfg_val| {
                const cfg_ptr = try arena.create(bridge.Config);
                cfg_ptr.* = cfg_val;
                bridge_runtime = bridge.Bridge.init(gpa, cfg_ptr);
                bridge_runtime.?.start() catch |err| {
                    std.log.warn("bridge: start failed: {s}", .{@errorName(err)});
                };
                const bref: *bridge.Bridge = &bridge_runtime.?;
                r.bridge_ctx = bref;
                r.bridge_fn = bridgePublishTrampoline;
                std.log.info("bridge: connected ({d} export filter(s), {d} server(s))", .{ cfg_ptr.exports.len, cfg_ptr.servers.len });
            }
        }
    }
    defer if (build_options.bridge) if (bridge_runtime) |*b| b.deinit();

    // Process-wide random ID, regenerated per start. 16 upper-hex chars — a
    // nod to nats-server's nuid without pulling in a nuid library.
    var id_bytes: [8]u8 = undefined;
    fsio.random(&id_bytes);
    var id_buf: [16]u8 = undefined;
    for (id_bytes, 0..) |b, j| {
        _ = std.fmt.bufPrint(id_buf[j * 2 ..][0..2], "{X:0>2}", .{b}) catch unreachable;
    }
    const server_id = try arena.dupe(u8, &id_buf);

    var srv: server.Server = .{
        .gpa = gpa,
        .loop = &loop,
        .router = &r,
        .rules = ruleset,
        .listener = undefined,
        .server_id = server_id,
        .listen_host = "0.0.0.0",
        .listen_port = port,
        .stats_enabled = stats_enabled,
        .trace_enabled = trace_enabled,
        .bridge_stats = if (build_options.bridge and bridge_runtime != null)
            @ptrCast(&bridge_runtime.?.stats)
        else
            null,
        .snapshot_path = if (lvc_enabled) snapshot_path else null,
        .snapshot_every_ms = if (lvc_enabled and snapshot_path != null)
            @as(u64, snapshot_every_s) * 1000
        else
            0,
        .snapshot_io = fsio,
        .shutdown_enabled = true,
    };

    const address: std.Io.net.IpAddress = .{ .ip4 = .unspecified(port) };
    try srv.listen(address);
    defer srv.deinit();
    std.log.info("monoblok listening on {f} id={s}", .{ address, server_id });

    // SIGINT / SIGTERM -> graceful shutdown with a final snapshot. The
    // handler only touches the atomic server pointer and notifies the
    // loop's xev.Async; all real work happens on the loop thread in
    // onShutdown.
    shutdown_server_ptr.store(&srv, .release);
    installShutdownSignals();

    try loop.run(.until_done);
}

/// Published by the main thread before the signal handlers are installed,
/// read by the signal handler (which may fire on any thread). Pointer
/// stores on aligned memory are atomic on every real CPU, but using an
/// explicit atomic keeps it portable-correct and documents intent.
var shutdown_server_ptr: std.atomic.Value(?*server.Server) = .init(null);

fn installShutdownSignals() void {
    const act: std.posix.Sigaction = .{
        .handler = .{ .handler = onSignalRequestShutdown },
        .mask = std.posix.sigemptyset(),
        .flags = 0,
    };
    std.posix.sigaction(.INT, &act, null);
    std.posix.sigaction(.TERM, &act, null);
}

fn onSignalRequestShutdown(_: std.posix.SIG) callconv(.c) void {
    // Async-signal-safe: notify() does a non-blocking write to an
    // eventfd / mach port. No allocation, no locks, no stdio.
    if (shutdown_server_ptr.load(.acquire)) |srv| srv.requestShutdown();
}

fn bridgePublishTrampoline(ctx: *anyopaque, subj: []const u8, payload: []const u8) void {
    if (!build_options.bridge) return;
    const b: *bridge.Bridge = @ptrCast(@alignCast(ctx));
    b.publish(subj, payload);
}

fn readFile(io: Io, arena: std.mem.Allocator, path: []const u8) ![]u8 {
    const cwd = Io.Dir.cwd();
    var file = try cwd.openFile(io, path, .{});
    defer file.close(io);
    const len = try file.length(io);
    const buf = try arena.alloc(u8, @intCast(len));
    var file_reader = file.reader(io, buf);
    try file_reader.interface.readSliceAll(buf);
    return buf;
}

fn fatal(msg: []const u8) noreturn {
    std.debug.print("monoblok: {s}\n", .{msg});
    std.process.exit(2);
}

fn printUsage() void {
    std.debug.print(
        \\Usage: monoblok [--port PORT] [--patchbay FILE] [--no-lvc] [--snapshot FILE]
        \\
        \\A NATS-compatible server with an S-expression routing and signal conditioning DSL ("patchbay").
        \\
        \\Options:
        \\  --port PORT      TCP port to listen on (default 4222)
        \\  --patchbay FILE  Path to patchbay file (optional). --rules is a
        \\                   backwards-compatible alias.
        \\  --no-lvc         Disable the last-value cache and $LVC.* live
        \\                   streams. LVC is on by default; overhead ~2-4%.
        \\  --stats          Log a running summary every 10k PUBs: max
        \\                   rule-publishes-per-input and max per-conn
        \\                   outbound hwm. Useful for spotting headroom
        \\                   under threshold.
        \\  --trace          Print each patchbay evaluation step (form,
        \\                   result, elapsed time) to stderr. Loud: every
        \\                   inbound PUB produces output. Debug-only, do
        \\                   not run in production.
        \\  --snapshot FILE  LVC snapshot path. Loaded on startup if it
        \\                   exists. Combine with --snapshot-every to also
        \\                   dump periodically.
        \\  --snapshot-every SECONDS
        \\                   Write the snapshot every SECONDS (atomic
        \\                   write via .tmp + rename). 0 or omitted =
        \\                   load-only. Requires --snapshot.
        \\
    , .{});
}

fn printVersion() void {
    std.debug.print("monoblok {s} (zig {d}.{d}.{d})\n", .{
        manifest.version,
        builtin.zig_version.major,
        builtin.zig_version.minor,
        builtin.zig_version.patch,
    });
}

test {
    _ = subject;
    _ = proto;
    _ = sexpr;
    _ = rules;
    _ = snapshot;
    if (build_options.bridge) _ = bridge;
}
