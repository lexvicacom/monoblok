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
pub const bridge = if (build_options.bridge) @import("bridge.zig") else {};

const manifest = @import("manifest");
const build_options = @import("build_options");

const Flag = enum { port, patchbay, no_lvc, stats, help, version };

const flag_map = std.StaticStringMap(Flag).initComptime(.{
    .{ "--port", .port },
    .{ "--patchbay", .patchbay },
    // `--rules` is kept as a silent alias so existing scripts don't break.
    .{ "--rules", .patchbay },
    .{ "--no-lvc", .no_lvc },
    .{ "--stats", .stats },
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
    std.log.info("loaded {d} patchbay form(s)", .{loaded_rules.len});
    std.log.info("libxev backend: {s} (os={s})", .{ @tagName(xev.backend), @tagName(builtin.os.tag) });
    std.log.info("lvc: {s}", .{if (lvc_enabled) "enabled" else "disabled"});

    var loop = try xev.Loop.init(.{});
    defer loop.deinit();

    var r = router.Router.init(gpa, lvc_enabled);
    defer r.deinit();

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
        .rules = loaded_rules,
        .listener = undefined,
        .server_id = server_id,
        .listen_host = "0.0.0.0",
        .listen_port = port,
        .stats_enabled = stats_enabled,
        .bridge_stats = if (build_options.bridge and bridge_runtime != null)
            @ptrCast(&bridge_runtime.?.stats)
        else
            null,
    };

    const address: std.Io.net.IpAddress = .{ .ip4 = .unspecified(port) };
    try srv.listen(address);
    defer srv.deinit();
    std.log.info("monoblok listening on {f} id={s}", .{ address, server_id });

    try loop.run(.until_done);
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
        \\Usage: monoblok [--port PORT] [--patchbay FILE] [--no-lvc]
        \\
        \\A NATS-compatible daemon with an S-expression routing DSL ("patchbay").
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
    if (build_options.bridge) _ = bridge;
}
