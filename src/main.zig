const std = @import("std");
const builtin = @import("builtin");
const Io = std.Io;
const xev = @import("xev");

pub const patchbay = @import("patchbay");
pub const subject = patchbay.subject;
pub const sexpr = patchbay.sexpr;
pub const rules = patchbay.eval;
pub const proto = @import("proto.zig");
pub const router = @import("router.zig");
pub const server = @import("server.zig");
pub const snapshot = @import("snapshot.zig");
pub const bridge = @import("bridge.zig");

const manifest = @import("manifest");

const Flag = enum { port, unix_socket, patchbay, no_lvc, stats, trace, snapshot, snapshot_every, clock_tick_ms, stats_tick_ms, validate, help, version };

const flag_map = std.StaticStringMap(Flag).initComptime(.{
    .{ "--port", .port },
    .{ "--unix-socket", .unix_socket },
    .{ "--patchbay", .patchbay },
    // `--rules` is kept as a silent alias so existing scripts don't break.
    .{ "--rules", .patchbay },
    .{ "--no-lvc", .no_lvc },
    .{ "--stats", .stats },
    .{ "--trace", .trace },
    .{ "--snapshot", .snapshot },
    .{ "--snapshot-every", .snapshot_every },
    .{ "--clock-tick-ms", .clock_tick_ms },
    .{ "--stats-tick-ms", .stats_tick_ms },
    .{ "--validate", .validate },
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
    var unix_socket_path: ?[]const u8 = null;
    var patchbay_path: ?[]const u8 = null;
    var lvc_enabled = true;
    var stats_enabled = false;
    var trace_enabled = false;
    var snapshot_path: ?[]const u8 = null;
    var snapshot_every_s: u32 = 0;
    var clock_tick_ms: u64 = server.default_clock_tick_ms;
    var stats_tick_ms: u64 = server.default_stats_tick_ms;
    var validate_only = false;

    var it = try init.minimal.args.iterateAllocator(gpa);
    defer it.deinit();
    _ = it.skip();
    while (it.next()) |a| {
        // First non-flag word is the patchbay path. Only one allowed.
        if (a.len == 0 or a[0] != '-') {
            if (patchbay_path != null) fatal("patchbay path specified twice");
            patchbay_path = a;
            continue;
        }
        const flag = flag_map.get(a) orelse fatal("unknown argument");
        switch (flag) {
            .port => {
                const v = it.next() orelse fatal("--port requires a value");
                port = std.fmt.parseInt(u16, v, 10) catch fatal("invalid port");
            },
            .unix_socket => {
                unix_socket_path = it.next() orelse fatal("--unix-socket requires a path");
            },
            .patchbay => {
                if (patchbay_path != null) fatal("patchbay path specified twice");
                patchbay_path = it.next() orelse fatal("--patchbay requires a path");
            },
            .no_lvc => lvc_enabled = false,
            .stats => stats_enabled = true,
            .trace => trace_enabled = true,
            .snapshot => snapshot_path = it.next() orelse fatal("--snapshot requires a path"),
            .snapshot_every => {
                const v = it.next() orelse fatal("--snapshot-every requires a value in seconds");
                snapshot_every_s = std.fmt.parseInt(u32, v, 10) catch fatal("invalid --snapshot-every value");
            },
            .clock_tick_ms => {
                const v = it.next() orelse fatal("--clock-tick-ms requires a value in milliseconds");
                clock_tick_ms = std.fmt.parseInt(u64, v, 10) catch fatal("invalid --clock-tick-ms value");
                if (clock_tick_ms == 0) fatal("--clock-tick-ms must be > 0");
            },
            .stats_tick_ms => {
                const v = it.next() orelse fatal("--stats-tick-ms requires a value in milliseconds");
                stats_tick_ms = std.fmt.parseInt(u64, v, 10) catch fatal("invalid --stats-tick-ms value");
                if (stats_tick_ms == 0) fatal("--stats-tick-ms must be > 0");
            },
            .validate => validate_only = true,
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
        if (patchbay_src) |src| {
            var parse_err_offset: usize = 0;
            break :blk rules.loadRulesReporting(arena, src, &parse_err_offset) catch |err| switch (err) {
                error.UnexpectedEof,
                error.UnexpectedRParen,
                error.UnterminatedString,
                error.InvalidEscape,
                error.InvalidNumber,
                => fatalParseError(patchbay_path.?, src, parse_err_offset, err),
                else => return err,
            };
        }
        break :blk &.{};
    };
    defer rules.deinitRules(loaded_rules, gpa);
    // Form-lint every rule before opening a socket so arity bugs surface
    // at startup, not on the first matching PUB.
    if (validate_only and patchbay_path == null) fatal("--validate requires a patchbay path");
    const failures = try rules.validate(arena, gpa, loaded_rules);
    if (failures.len > 0) {
        for (failures) |f| {
            std.log.err("patchbay rule {d} (on \"{s}\") failed validation when test-publishing \"{s}\": {s}", .{
                f.rule_index, f.filter, f.synthetic_subject, @errorName(f.err),
            });
        }
        fatal("patchbay validation failed");
    }
    if (validate_only) {
        std.debug.print("{s}: ok ({d} rule{s})\n", .{
            patchbay_path.?, loaded_rules.len, if (loaded_rules.len == 1) "" else "s",
        });
        return;
    }
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

    // Warm-start the LVC before accepting connections so the first
    // `SUB $LVC.*` sees the restored state.
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

    // Bridge config comes from the top-level `(bridge ...)` form in the
    // patchbay file.
    var bridge_runtime: ?bridge.Bridge = null;
    if (patchbay_src) |src| {
        const cfg_opt = bridge.loadConfig(arena, src) catch |err| blk: {
            std.log.warn("bridge: config parse failed: {s}", .{@errorName(err)});
            break :blk null;
        };
        if (cfg_opt) |cfg_val| {
            const cfg_ptr = try arena.create(bridge.Config);
            cfg_ptr.* = cfg_val;
            bridge_runtime = bridge.Bridge.init(gpa, cfg_ptr);
            if (bridge_runtime.?.start()) |_| {
                const bref: *bridge.Bridge = &bridge_runtime.?;
                r.bridge_ctx = bref;
                r.bridge_fn = bridgePublishTrampoline;
                std.log.info("bridge: connected ({d} export filter(s), {d} server(s))", .{ cfg_ptr.exports.len, cfg_ptr.servers.len });
            } else |err| {
                std.log.warn("bridge: start failed: {s} (forwarding disabled)", .{@errorName(err)});
            }
        }
    }
    defer if (bridge_runtime) |*b| b.deinit();

    // Process-wide random ID, 16 upper-hex chars (nats-server nuid-like).
    var id_bytes: [8]u8 = undefined;
    fsio.random(&id_bytes);
    var id_buf: [16]u8 = undefined;
    for (id_bytes, 0..) |b, j| {
        _ = try std.fmt.bufPrint(id_buf[j * 2 ..][0..2], "{X:0>2}", .{b});
    }
    const server_id = try arena.dupe(u8, &id_buf);

    if (port == 0 and unix_socket_path == null) {
        fatal("nothing to listen on (--port 0 disables TCP and --unix-socket was not given)");
    }

    var srv: server.Server = .{
        .gpa = gpa,
        .loop = &loop,
        .router = &r,
        .rules = ruleset,
        .server_id = server_id,
        .listen_host = "0.0.0.0",
        .listen_port = port,
        .tcp_enabled = port != 0,
        .stats_enabled = stats_enabled,
        .trace_enabled = trace_enabled,
        .bridge_stats = if (bridge_runtime != null)
            @ptrCast(&bridge_runtime.?.stats)
        else
            null,
        .snapshot_path = if (lvc_enabled) snapshot_path else null,
        .snapshot_every_ms = if (lvc_enabled and snapshot_path != null)
            @as(u64, snapshot_every_s) * 1000
        else
            0,
        .snapshot_io = fsio,
        .clock_tick_ms = clock_tick_ms,
        .stats_tick_ms = stats_tick_ms,
        .shutdown_enabled = true,
    };

    const address: std.Io.net.IpAddress = .{ .ip4 = .unspecified(port) };
    try srv.listen(address);
    defer srv.deinit();
    if (port != 0) {
        std.log.info("monoblok listening on {f} id={s}", .{ address, server_id });
    }
    if (unix_socket_path) |usp| {
        srv.listenUnix(fsio, usp) catch |err| switch (err) {
            error.PathExistsAndIsNotASocket => fatal("--unix-socket path exists but is not a socket; refusing to overwrite"),
            error.PathTooLong => fatal("--unix-socket path is too long (max 103 bytes)"),
            else => return err,
        };
        std.log.info("monoblok listening on unix:{s} id={s}", .{ usp, server_id });
    }

    // SIGINT/SIGTERM: signal handler notifies an xev.Async; real work runs
    // on the loop thread in onShutdown.
    shutdown_server_ptr.store(&srv, .release);
    installShutdownSignals();

    try loop.run(.until_done);
}

/// Set by main before installing signal handlers; read by the handler
/// (which may fire on any thread).
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
    // Async-signal-safe: `notify()` is a non-blocking eventfd/mach write.
    if (shutdown_server_ptr.load(.acquire)) |srv| srv.requestShutdown();
}

fn bridgePublishTrampoline(ctx: *anyopaque, subj: []const u8, payload: []const u8) void {
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

fn fatalParseError(path: []const u8, src: []const u8, offset: usize, err: anyerror) noreturn {
    const clamped = if (offset > src.len) src.len else offset;
    var line: usize = 1;
    var col: usize = 1;
    var i: usize = 0;
    while (i < clamped) : (i += 1) {
        if (src[i] == '\n') {
            line += 1;
            col = 1;
        } else col += 1;
    }
    // Find the start and end of the offending line so we can echo it back.
    var line_start: usize = clamped;
    while (line_start > 0 and src[line_start - 1] != '\n') : (line_start -= 1) {}
    var line_end: usize = clamped;
    while (line_end < src.len and src[line_end] != '\n') : (line_end += 1) {}
    const line_text = src[line_start..line_end];
    std.debug.print("monoblok: {s}:{d}:{d}: {s}\n", .{ path, line, col, @errorName(err) });
    std.debug.print("    {s}\n", .{line_text});
    // Caret line: spaces up to col-1, then ^.
    std.debug.print("    ", .{});
    var k: usize = 1;
    while (k < col) : (k += 1) std.debug.print(" ", .{});
    std.debug.print("^\n", .{});
    std.process.exit(2);
}

fn printUsage() void {
    std.debug.print(
        \\Usage: monoblok [PATCHBAY] [--port PORT] [--unix-socket PATH] [--no-lvc] [--snapshot FILE]
        \\
        \\A NATS-compatible server with an S-expression routing and signal conditioning DSL ("patchbay").
        \\
        \\Options:
        \\  PATCHBAY         Path to patchbay file. The first non-flag
        \\                   argument is treated as the patchbay path, so
        \\                   `monoblok patchbay.edn` and `monoblok
        \\                   --validate patchbay.edn` both work.
        \\  --port PORT      TCP port to listen on (default 4222). Pass 0 to
        \\                   disable TCP (in which case --unix-socket must
        \\                   be given).
        \\  --unix-socket PATH
        \\                   Also listen on an AF_UNIX stream socket at
        \\                   PATH, speaking the same NATS protocol. Stale
        \\                   socket files from a prior run are removed
        \\                   automatically; the file is created with mode
        \\                   0600 and unlinked on graceful shutdown.
        \\  --patchbay FILE  Explicit form of the positional argument.
        \\                   --rules is a backwards-compatible alias.
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
        \\  --clock-tick-ms MS
        \\                   Patchbay clock-walker cadence (default 2000).
        \\                   Lower = tighter time-bar close / time-ring
        \\                   eviction latency, higher idle CPU. Only
        \\                   `(bar (window-ms ...))` and `(window-ms ...)
        \\                   moving-*` rules care; if you have neither,
        \\                   crank it up.
        \\  --stats-tick-ms MS
        \\                   `$STATS.*` publish cadence (default 60000).
        \\  --validate       Form-lint the patchbay file and exit. Same
        \\                   check that runs at startup; useful for
        \\                   editor / pre-commit / CI flows. Requires
        \\                   a patchbay path.
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
    _ = router;
    _ = snapshot;
    _ = bridge;
}
