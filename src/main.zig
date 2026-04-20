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

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const arena = init.arena.allocator();
    const fsio = init.io; // Only used for loading the patchbay file at startup.

    const args = try init.minimal.args.toSlice(arena);

    var port: u16 = 4222;
    var patchbay_path: ?[]const u8 = null;
    var lvc_enabled = true;

    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        const a = args[i];
        if (std.mem.eql(u8, a, "--port")) {
            i += 1;
            if (i >= args.len) fatal("--port requires a value");
            port = std.fmt.parseInt(u16, args[i], 10) catch fatal("invalid port");
        } else if (std.mem.eql(u8, a, "--patchbay") or std.mem.eql(u8, a, "--rules")) {
            // `--rules` is kept as a silent alias so existing scripts don't break.
            i += 1;
            if (i >= args.len) fatal("--patchbay requires a path");
            patchbay_path = args[i];
        } else if (std.mem.eql(u8, a, "--no-lvc")) {
            lvc_enabled = false;
        } else if (std.mem.eql(u8, a, "--help") or std.mem.eql(u8, a, "-h")) {
            printUsage();
            return;
        } else fatal("unknown argument");
    }

    const loaded_rules: []rules.Rule = blk: {
        if (patchbay_path) |path| {
            const src = try readFile(fsio, arena, path);
            break :blk try rules.loadRules(arena, src);
        }
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
    };

    const address: std.Io.net.IpAddress = .{ .ip4 = .unspecified(port) };
    try srv.listen(address);
    std.log.info("monoblok listening on {f} id={s}", .{ address, server_id });

    try loop.run(.until_done);
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
        \\
    , .{});
}

test {
    _ = subject;
    _ = proto;
    _ = sexpr;
    _ = rules;
}
